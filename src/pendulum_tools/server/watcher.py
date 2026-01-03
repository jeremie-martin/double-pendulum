"""Background watcher thread for video processing."""

from __future__ import annotations

import json
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import TYPE_CHECKING, Any, Optional

from ..exceptions import AuthenticationError, RateLimitError, UploadError
from ..logging import get_logger
from .state import ErrorCategory, JobInfo, JobStatus, WatcherState, WatcherStatus

if TYPE_CHECKING:
    from ..config import UserConfig
    from ..uploader import YouTubeUploader


class WatcherThread(threading.Thread):
    """Background thread that monitors a directory and processes videos.

    This thread:
    - Scans for new video directories
    - Processes them (video effects, music, upload)
    - Updates shared state for UI display
    - Handles pause/resume via threading.Event
    - Categorizes errors for appropriate handling
    """

    def __init__(
        self,
        state: WatcherState,
        credentials_dir: Path,
        music_dir: Optional[Path],
        user_config: "UserConfig",
    ):
        super().__init__(daemon=True, name="WatcherThread")

        self.state = state
        self.credentials_dir = credentials_dir
        self.music_dir = music_dir
        self.user_config = user_config

        # Control events
        self._stop_event = threading.Event()
        self._pause_event = threading.Event()
        self._pause_event.set()  # Start unpaused

        # Uploader (initialized on first use)
        self._uploader: Optional["YouTubeUploader"] = None

        # Logger
        self.log = get_logger("watcher")

        # Track processed directories (to avoid re-processing)
        self._processed: set[str] = set()

    def stop(self) -> None:
        """Signal the thread to stop."""
        self._stop_event.set()
        self._pause_event.set()  # Unpause to allow clean exit

    def pause(self) -> None:
        """Pause processing."""
        self._pause_event.clear()
        self.state.paused = True

    def resume(self) -> None:
        """Resume processing."""
        self._pause_event.set()
        self.state.paused = False

    def is_paused(self) -> bool:
        """Check if processing is paused."""
        return not self._pause_event.is_set()

    def _wait_if_paused(self, timeout: float = 1.0) -> bool:
        """Wait while paused. Returns False if stop requested."""
        while not self._pause_event.is_set():
            if self._stop_event.is_set():
                return False
            self._pause_event.wait(timeout=timeout)
        return not self._stop_event.is_set()

    def _authenticate(self) -> bool:
        """Initialize or refresh YouTube authentication.

        Returns True if authentication succeeded.
        """
        from ..uploader import YouTubeUploader

        try:
            if self._uploader is None:
                self._uploader = YouTubeUploader(self.credentials_dir)

            self._uploader.authenticate()
            self.state.clear_auth_error()
            return True

        except FileNotFoundError as e:
            self.log.error(f"Credentials not found: {e}")
            self.state.set_auth_error(f"Credentials not found: {e}")
            return False

        except Exception as e:
            # Check if this is an auth-related error
            error_str = str(e).lower()
            if any(
                word in error_str
                for word in ["token", "credential", "auth", "expired", "invalid", "revoked"]
            ):
                self.log.error(f"Authentication failed: {e}")
                self.state.set_auth_error(str(e))
                return False

            # Other errors during auth
            self.log.error(f"Authentication error: {e}")
            self.state.set_auth_error(str(e))
            return False

    def trigger_reauth(self) -> bool:
        """Trigger re-authentication from UI.

        This is called when user clicks re-authenticate button.
        Returns True if successful.
        """
        # Clear old uploader to force new auth flow
        self._uploader = None

        if self._authenticate():
            self.resume()
            return True
        return False

    def _scan_for_pending(self) -> None:
        """Scan batch directory for new video directories."""
        batch_dir = self.state.batch_dir
        if not batch_dir or not batch_dir.exists():
            return

        current_time = datetime.now()
        settings = self.state.settings

        for item in sorted(batch_dir.iterdir(), key=lambda x: x.name):
            if self._stop_event.is_set():
                return

            if not item.is_dir() or not item.name.startswith("video_"):
                continue

            dir_name = item.name

            # Skip already processed
            if dir_name in self._processed:
                continue

            # Check if in current queue
            existing_names = {j.dir_name for j in self.state.peek_pending()}
            if dir_name in existing_names:
                continue
            if self.state.get_current_job() and self.state.get_current_job().dir_name == dir_name:
                continue

            # Check for required files
            has_metadata = (item / "metadata.json").exists()
            has_video = (item / "video.mp4").exists() or (item / "video_raw.mp4").exists()

            if not has_metadata or not has_video:
                continue

            # Check if already uploaded (from metadata)
            try:
                with open(item / "metadata.json") as f:
                    meta_data = json.load(f)
                if "upload" in meta_data and meta_data["upload"].get("video_id"):
                    self._processed.add(dir_name)
                    continue
            except (json.JSONDecodeError, KeyError, FileNotFoundError):
                pass

            # Check if already processed (has final video)
            if (item / "video_processed_final.mp4").exists():
                self._processed.add(dir_name)
                continue

            # Add to pending queue
            job = JobInfo(
                dir_name=dir_name,
                dir_path=item,
                status=JobStatus.PENDING,
                first_seen=current_time,
            )

            # Try to extract boom info from metadata
            try:
                with open(item / "metadata.json") as f:
                    meta_data = json.load(f)
                results = meta_data.get("results", {})
                config = meta_data.get("config", {})
                boom_frame = results.get("boom_frame")
                fps = config.get("video_fps", 60)
                if boom_frame:
                    job.boom_seconds = boom_frame / fps
            except Exception:
                pass

            self.state.add_pending(job)
            self.log.info(f"New video detected: {dir_name}")

    def _process_job(self, job: JobInfo) -> None:
        """Process a single video job."""
        from datetime import datetime

        from ..models import VideoMetadata
        from ..music import MusicManager
        from ..processing import ProcessingConfig, ProcessingPipeline
        from ..youtube import generate_description, generate_tags, generate_title

        job.started_at = datetime.now()
        video_dir = job.dir_path
        metadata_path = video_dir / "metadata.json"
        settings = self.state.settings

        # --- Step 1: Process video ---
        self.state.update_current_progress(
            status=JobStatus.PROCESSING,
            message="Processing video effects...",
            percent=0,
        )

        try:
            metadata = VideoMetadata.from_file(metadata_path)
        except Exception as e:
            self.log.error(f"{job.dir_name}: Failed to load metadata: {e}")
            self.state.fail_current(str(e), ErrorCategory.CONTENT)
            return

        config = ProcessingConfig(
            template="random",
            seed=hash(video_dir.name) % (2**32),
            shorts=self.user_config.processing.shorts,
            blurred_background=self.user_config.processing.blur_bg,
            extract_thumbnails=True,
            use_nvenc=self.user_config.use_nvenc,
            nvenc_cq=self.user_config.nvenc_cq,
        )

        try:
            pipeline = ProcessingPipeline(video_dir, config)
            result = pipeline.run(force=self.user_config.processing.force)

            if not result.success:
                self.log.error(f"{job.dir_name}: Processing failed: {result.error}")
                self.state.fail_current(result.error or "Processing failed", ErrorCategory.CONTENT)
                return

            result.save_to_metadata(metadata_path)
            job.template_used = result.template_used
            self.log.info(f"{job.dir_name}: Processing complete (template: {result.template_used})")

        except Exception as e:
            self.log.error(f"{job.dir_name}: Processing exception: {e}")
            self.state.fail_current(str(e), ErrorCategory.CONTENT)
            return

        if not self._wait_if_paused():
            return

        # --- Step 2: Add music ---
        self.state.update_current_progress(
            status=JobStatus.ADDING_MUSIC,
            message="Adding music...",
            percent=33,
        )

        if not self.music_dir:
            self.log.error(f"{job.dir_name}: No music directory configured")
            self.state.fail_current("No music directory configured", ErrorCategory.CONTENT)
            return

        try:
            from ..music import get_music_state

            music_state = get_music_state()
            manager = MusicManager(self.music_dir, music_state=music_state)
        except FileNotFoundError as e:
            self.log.error(f"{job.dir_name}: Music directory not found: {e}")
            self.state.fail_current(f"Music directory not found: {e}", ErrorCategory.CONTENT)
            return

        # Reload metadata after processing
        metadata = VideoMetadata.from_file(metadata_path)

        boom_frame = metadata.results.boom_frame if metadata.results else None
        if not boom_frame or boom_frame <= 0:
            self.log.error(f"{job.dir_name}: No boom frame detected")
            self.state.fail_current("No boom frame detected", ErrorCategory.CONTENT)
            return

        video_fps = metadata.config.video_fps
        video_boom_seconds = boom_frame / video_fps

        # Select track
        selected_track = manager.pick_track_for_boom(video_boom_seconds)
        if not selected_track:
            selected_track = manager.random_track()

        if not selected_track:
            self.log.error(f"{job.dir_name}: No music tracks available")
            self.state.fail_current("No music tracks available", ErrorCategory.CONTENT)
            return

        # Mux audio
        output_path = result.video_path.with_name(result.video_path.stem + "_final.mp4")
        success = MusicManager.mux_with_audio(
            video_path=result.video_path,
            audio_path=selected_track.filepath,
            output_path=output_path,
            boom_frame=boom_frame,
            drop_time_ms=selected_track.drop_time_ms,
            video_fps=video_fps,
        )

        if success:
            MusicManager.update_metadata_with_music(metadata_path, selected_track)
            job.music_track = selected_track.title or selected_track.id
            self.log.info(f"{job.dir_name}: Music added ({selected_track.title})")
        else:
            self.log.error(f"{job.dir_name}: FFmpeg muxing failed")
            self.state.fail_current("FFmpeg muxing failed", ErrorCategory.CONTENT)
            return

        if not self._wait_if_paused():
            return

        # --- Step 3: Upload ---
        self.state.update_current_progress(
            status=JobStatus.UPLOADING,
            message="Uploading to YouTube...",
            percent=66,
        )

        if not self._uploader:
            self.state.fail_current("Uploader not initialized", ErrorCategory.SYSTEMIC)
            return

        # Reload metadata after music
        metadata = VideoMetadata.from_file(metadata_path)
        video_path = output_path  # Use the final video with music

        title = generate_title(metadata)
        description = generate_description(metadata)
        tags = generate_tags(metadata)

        def progress_callback(progress: float) -> None:
            percent = 66 + int(progress * 33)  # 66-99%
            self.state.update_current_progress(
                message=f"Uploading... {int(progress * 100)}%",
                percent=percent,
            )

        try:
            video_id = self._uploader.upload(
                video_path=video_path,
                title=title,
                description=description,
                tags=tags,
                privacy_status=settings.privacy,
                category_id="10",  # Music
                progress_callback=progress_callback,
            )

            if not video_id:
                self.log.error(f"{job.dir_name}: Upload returned no video ID")
                self.state.fail_current("No video ID returned", ErrorCategory.TRANSIENT)
                return

            # Save upload info to metadata
            self._save_upload_metadata(
                metadata_path, video_id, settings.privacy, title, description, tags
            )

            # Archive the upload
            self._archive_upload(metadata_path, video_id, title, description, tags, settings.privacy, job)

            # Add to playlist if configured
            if settings.playlist_id:
                if self._uploader.add_to_playlist(video_id, settings.playlist_id):
                    self.log.info(f"{job.dir_name}: Added to playlist {settings.playlist_id}")

            # Delete if configured
            if settings.delete_after_upload:
                import shutil

                try:
                    shutil.rmtree(video_dir)
                    self.log.info(f"{job.dir_name}: Deleted video directory")
                except Exception as e:
                    self.log.warning(f"{job.dir_name}: Failed to delete directory: {e}")

            # Success!
            self._processed.add(job.dir_name)
            self.state.complete_current(
                video_id=video_id,
                video_url=f"https://youtu.be/{video_id}",
                music_track=job.music_track,
                template_used=job.template_used,
            )
            self.log.info(f"{job.dir_name}: Uploaded successfully: https://youtu.be/{video_id}")

        except RateLimitError as e:
            self.log.warning(f"{job.dir_name}: Rate limited: {e}")
            self.state.fail_current(str(e), ErrorCategory.TRANSIENT)

        except UploadError as e:
            # Check if this looks like an auth error
            error_str = str(e).lower()
            if any(word in error_str for word in ["401", "403", "auth", "credential", "token"]):
                self.log.error(f"{job.dir_name}: Auth error during upload: {e}")
                self.state.set_auth_error(str(e))
                # Don't mark as failed - keep in current so we can retry after reauth
            else:
                self.log.error(f"{job.dir_name}: Upload error: {e}")
                self.state.fail_current(str(e), ErrorCategory.TRANSIENT)

        except Exception as e:
            # Check for auth-related exceptions
            error_str = str(e).lower()
            if any(word in error_str for word in ["401", "403", "auth", "credential", "token", "expired"]):
                self.log.error(f"{job.dir_name}: Auth exception: {e}")
                self.state.set_auth_error(str(e))
            else:
                self.log.error(f"{job.dir_name}: Upload exception: {e}")
                self.state.fail_current(str(e), ErrorCategory.TRANSIENT)

    def _save_upload_metadata(
        self,
        metadata_path: Path,
        video_id: str,
        privacy: str,
        title: str,
        description: str,
        tags: list[str],
    ) -> None:
        """Save upload info to metadata.json."""
        try:
            with open(metadata_path) as f:
                meta_data = json.load(f)

            meta_data["upload"] = {
                "video_id": video_id,
                "url": f"https://youtu.be/{video_id}",
                "privacy": privacy,
                "uploaded_at": datetime.now().isoformat(),
                "title": title,
                "description": description,
                "tags": tags,
            }

            with open(metadata_path, "w") as f:
                json.dump(meta_data, f, indent=2)
                f.write("\n")
        except Exception as e:
            self.log.warning(f"Failed to save upload info: {e}")

    def _archive_upload(
        self,
        metadata_path: Path,
        video_id: str,
        title: str,
        description: str,
        tags: list[str],
        privacy: str,
        job: JobInfo,
    ) -> None:
        """Archive the upload for future analysis."""
        try:
            from ..archive import UploadRecord, append_upload_record

            with open(metadata_path) as f:
                meta_data = json.load(f)

            record = UploadRecord.from_metadata(
                metadata=meta_data,
                video_id=video_id,
                title=title,
                description=description,
                tags=tags,
                privacy=privacy,
                source_dir=job.dir_name,
            )
            append_upload_record(record)
            self.log.debug(f"{job.dir_name}: Archived upload record")
        except Exception as e:
            self.log.warning(f"Failed to archive upload: {e}")

    def run(self) -> None:
        """Main thread loop."""
        self.log.info("Watcher thread starting")
        self.state.status = WatcherStatus.STARTING

        # Authenticate
        if not self._authenticate():
            self.log.error("Initial authentication failed")
            # Don't exit - user can fix via UI
            self.state.status = WatcherStatus.AUTH_REQUIRED
        else:
            self.state.status = WatcherStatus.RUNNING

        # Initial scan for already-processed videos
        self._scan_existing()

        # Main loop
        while not self._stop_event.is_set():
            try:
                # Wait if paused
                if not self._wait_if_paused():
                    break

                # Check auth state
                if self.state.status == WatcherStatus.AUTH_REQUIRED:
                    time.sleep(1.0)
                    continue

                # Scan for new videos
                self._scan_for_pending()

                # Check if there's a job to process
                settings = self.state.settings

                # Check settle time for pending jobs
                pending = self.state.peek_pending()
                if pending:
                    now = datetime.now()
                    for job in pending:
                        elapsed = (now - job.first_seen).total_seconds()
                        if elapsed >= settings.settle_time:
                            # Ready to process - get it from queue
                            next_job = self.state.get_next_pending()
                            if next_job and next_job.dir_name == job.dir_name:
                                self.state.set_current_job(next_job)
                                self.log.info(f"Processing: {next_job.dir_name}")

                                # Process the job
                                self._process_job(next_job)

                                # Upload delay (if successful and configured)
                                if (
                                    settings.upload_delay > 0
                                    and not self._stop_event.is_set()
                                    and self.state.get_current_job() is None  # Was completed
                                ):
                                    self.state.status = WatcherStatus.RUNNING
                                    self.log.info(f"Waiting {settings.upload_delay}s before next upload")

                                    # Wait with interruptibility
                                    delay_end = time.time() + settings.upload_delay
                                    while time.time() < delay_end and not self._stop_event.is_set():
                                        time.sleep(min(1.0, delay_end - time.time()))

                            break  # Only process one job per iteration

                # Poll interval
                poll_end = time.time() + settings.poll_interval
                while time.time() < poll_end and not self._stop_event.is_set():
                    time.sleep(min(0.5, poll_end - time.time()))

            except Exception as e:
                self.log.error(f"Watcher loop error: {e}")
                time.sleep(1.0)

        self.state.status = WatcherStatus.STOPPED
        self.log.info("Watcher thread stopped")

    def _scan_existing(self) -> None:
        """Scan for already-processed videos on startup."""
        batch_dir = self.state.batch_dir
        if not batch_dir or not batch_dir.exists():
            return

        for item in batch_dir.iterdir():
            if not item.is_dir() or not item.name.startswith("video_"):
                continue

            # Check for final video
            if (item / "video_processed_final.mp4").exists():
                self._processed.add(item.name)
                continue

            # Check for upload marker
            metadata_path = item / "metadata.json"
            if metadata_path.exists():
                try:
                    with open(metadata_path) as f:
                        meta_data = json.load(f)
                    if "upload" in meta_data and meta_data["upload"].get("video_id"):
                        self._processed.add(item.name)
                except (json.JSONDecodeError, KeyError):
                    pass

        self.log.info(f"Found {len(self._processed)} already-processed videos")
