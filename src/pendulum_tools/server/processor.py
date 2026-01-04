"""Video processor with observable state and interruptible waits.

This module provides a background video processor that:
- Scans for new videos in a batch directory
- Processes them using the existing pipeline
- Maintains observable state for UI display
- Supports pause/resume and settings changes at runtime
- Uses interruptible waits so settings can change mid-delay
"""

from __future__ import annotations

import json
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable, Optional

from ..logging import get_logger

if TYPE_CHECKING:
    from ..config import UserConfig
    from ..uploader import YouTubeUploader

log = get_logger(__name__)


class ProcessorStatus(Enum):
    """Overall processor status."""

    STARTING = "starting"
    RUNNING = "running"
    PAUSED = "paused"
    WAITING = "waiting"  # Waiting between uploads
    AUTH_REQUIRED = "auth_required"
    STOPPED = "stopped"


@dataclass
class VideoJob:
    """Information about a video being processed."""

    dir_name: str
    dir_path: Path
    first_seen: datetime
    status: str = "pending"  # pending, settling, processing, completed, failed
    progress: str = ""
    error: Optional[str] = None
    video_id: Optional[str] = None
    video_url: Optional[str] = None
    music_track: Optional[str] = None
    template: Optional[str] = None


@dataclass
class ProcessorState:
    """Observable state of the video processor.

    All fields are read-only from outside the processor.
    Use snapshot() to get a thread-safe copy for UI rendering.
    """

    status: ProcessorStatus = ProcessorStatus.STOPPED
    current_job: Optional[VideoJob] = None
    pending_queue: list[VideoJob] = field(default_factory=list)
    completed: deque[VideoJob] = field(default_factory=lambda: deque(maxlen=50))
    failed: deque[VideoJob] = field(default_factory=lambda: deque(maxlen=50))

    # Settings (can be modified at runtime)
    upload_delay: float = 60.0
    poll_interval: float = 5.0
    settle_time: float = 10.0
    privacy: str = "private"
    playlist_id: Optional[str] = None
    delete_after_upload: bool = False

    # Counters
    total_processed: int = 0
    total_failed: int = 0

    # Auth state
    auth_error: Optional[str] = None

    # Wait state (for showing remaining time in UI)
    wait_until: Optional[datetime] = None
    wait_reason: str = ""

    def snapshot(self) -> dict[str, Any]:
        """Get a thread-safe snapshot for UI rendering."""
        return {
            "status": self.status.value,
            "current_job": self._job_dict(self.current_job) if self.current_job else None,
            "pending_queue": [self._job_dict(j) for j in self.pending_queue],
            "completed": [self._job_dict(j) for j in self.completed],
            "failed": [self._job_dict(j) for j in self.failed],
            "upload_delay": self.upload_delay,
            "poll_interval": self.poll_interval,
            "settle_time": self.settle_time,
            "privacy": self.privacy,
            "playlist_id": self.playlist_id,
            "delete_after_upload": self.delete_after_upload,
            "total_processed": self.total_processed,
            "total_failed": self.total_failed,
            "auth_error": self.auth_error,
            "wait_until": self.wait_until.isoformat() if self.wait_until else None,
            "wait_reason": self.wait_reason,
        }

    @staticmethod
    def _job_dict(job: VideoJob) -> dict[str, Any]:
        """Convert job to dictionary for UI."""
        return {
            "dir_name": job.dir_name,
            "status": job.status,
            "progress": job.progress,
            "error": job.error,
            "video_id": job.video_id,
            "video_url": job.video_url,
            "music_track": job.music_track,
            "template": job.template,
        }


class VideoProcessor(threading.Thread):
    """Background video processor with observable state.

    Usage:
        state = ProcessorState()
        processor = VideoProcessor(
            state=state,
            batch_dir=Path("/path/to/batch"),
            credentials_dir=Path("/path/to/credentials"),
            music_dir=Path("/path/to/music"),
            user_config=user_config,
        )
        processor.start()

        # Read state for UI
        snapshot = state.snapshot()

        # Change settings at runtime
        state.upload_delay = 120.0
        processor.interrupt_wait()  # Apply immediately

        # Control
        processor.pause()
        processor.resume()
        processor.stop()
    """

    def __init__(
        self,
        state: ProcessorState,
        batch_dir: Path,
        credentials_dir: Path,
        music_dir: Optional[Path],
        user_config: "UserConfig",
    ):
        super().__init__(daemon=True, name="VideoProcessor")
        self.state = state
        self.batch_dir = batch_dir
        self.credentials_dir = credentials_dir
        self.music_dir = music_dir
        self.user_config = user_config

        # Control events
        self._stop_event = threading.Event()
        self._pause_event = threading.Event()
        self._pause_event.set()  # Start unpaused
        self._interrupt_event = threading.Event()

        # Uploader (lazy init)
        self._uploader: Optional["YouTubeUploader"] = None

        # Track processed directories
        self._processed: set[str] = set()
        self._pending: dict[str, datetime] = {}  # dir_name -> first_seen

        # Lock for state modifications
        self._lock = threading.Lock()

    def stop(self) -> None:
        """Stop the processor."""
        log.info("Stopping video processor")
        self._stop_event.set()
        self._interrupt_event.set()  # Wake from any wait
        self._pause_event.set()  # Wake from pause

    def pause(self) -> None:
        """Pause processing."""
        log.info("Pausing video processor")
        self._pause_event.clear()
        with self._lock:
            self.state.status = ProcessorStatus.PAUSED

    def resume(self) -> None:
        """Resume processing."""
        log.info("Resuming video processor")
        self._pause_event.set()
        with self._lock:
            if self.state.status == ProcessorStatus.PAUSED:
                self.state.status = ProcessorStatus.RUNNING

    def is_paused(self) -> bool:
        """Check if processor is paused."""
        return not self._pause_event.is_set()

    def interrupt_wait(self) -> None:
        """Interrupt current wait (e.g., when settings change)."""
        self._interrupt_event.set()

    def trigger_reauth(self) -> bool:
        """Trigger re-authentication."""
        log.info("Re-authenticating")
        self._uploader = None
        if self._authenticate():
            self.resume()
            return True
        return False

    def _wait_if_paused(self) -> bool:
        """Wait while paused. Returns False if stop requested."""
        while not self._pause_event.is_set():
            if self._stop_event.is_set():
                return False
            self._pause_event.wait(timeout=0.5)
        return not self._stop_event.is_set()

    def _interruptible_wait(self, seconds: float, reason: str) -> bool:
        """Wait for specified seconds, but can be interrupted.

        Returns True if wait completed, False if interrupted or stopped.
        """
        if seconds <= 0:
            return True

        with self._lock:
            self.state.wait_until = datetime.now() + __import__("datetime").timedelta(seconds=seconds)
            self.state.wait_reason = reason

        self._interrupt_event.clear()
        end_time = time.time() + seconds

        while time.time() < end_time:
            if self._stop_event.is_set():
                return False
            if self._interrupt_event.is_set():
                log.debug(f"Wait interrupted: {reason}")
                return False
            # Check pause
            if not self._pause_event.is_set():
                # Paused - stop the wait
                return False
            time.sleep(min(0.5, end_time - time.time()))

        with self._lock:
            self.state.wait_until = None
            self.state.wait_reason = ""

        return True

    def _authenticate(self) -> bool:
        """Initialize or refresh YouTube authentication."""
        from ..uploader import YouTubeUploader

        try:
            if self._uploader is None:
                self._uploader = YouTubeUploader(self.credentials_dir)

            self._uploader.authenticate()

            with self._lock:
                self.state.auth_error = None

            log.info("YouTube authentication successful")
            return True

        except FileNotFoundError as e:
            log.error(f"Credentials not found: {e}")
            with self._lock:
                self.state.auth_error = f"Credentials not found: {e}"
            return False

        except Exception as e:
            error_str = str(e).lower()
            if any(word in error_str for word in ["token", "credential", "auth", "expired", "invalid"]):
                log.error(f"Authentication failed: {e}")
            else:
                log.error(f"Authentication error: {e}")
            with self._lock:
                self.state.auth_error = str(e)
            return False

    def _scan_for_new_videos(self) -> None:
        """Scan batch directory for new video directories."""
        if not self.batch_dir.exists():
            return

        current_time = datetime.now()

        for item in sorted(self.batch_dir.iterdir(), key=lambda x: x.name):
            if self._stop_event.is_set():
                return

            if not item.is_dir() or not item.name.startswith("video_"):
                continue

            dir_name = item.name

            # Skip already processed
            if dir_name in self._processed:
                continue

            # Skip if already in pending queue
            if any(j.dir_name == dir_name for j in self.state.pending_queue):
                continue

            # Skip if currently processing
            if self.state.current_job and self.state.current_job.dir_name == dir_name:
                continue

            # Check for required files
            has_metadata = (item / "metadata.json").exists()
            has_video = (item / "video.mp4").exists() or (item / "video_raw.mp4").exists()

            if not has_metadata or not has_video:
                continue

            # Check if already uploaded
            try:
                with open(item / "metadata.json") as f:
                    meta = json.load(f)
                if "upload" in meta and meta["upload"].get("video_id"):
                    self._processed.add(dir_name)
                    continue
            except (json.JSONDecodeError, FileNotFoundError):
                pass

            # Check if already has final video
            if (item / "video_processed_final.mp4").exists():
                self._processed.add(dir_name)
                continue

            # Track when we first saw this directory
            if dir_name not in self._pending:
                self._pending[dir_name] = current_time
                log.info(f"New video detected: {dir_name}")

            # Check settle time
            elapsed = (current_time - self._pending[dir_name]).total_seconds()
            if elapsed < self.state.settle_time:
                continue

            # Ready for processing - add to queue
            del self._pending[dir_name]

            job = VideoJob(
                dir_name=dir_name,
                dir_path=item,
                first_seen=current_time,
                status="pending",
            )

            with self._lock:
                self.state.pending_queue.append(job)

            log.info(f"Video ready for processing: {dir_name}")

    def _scan_existing(self) -> None:
        """Scan for already-processed videos on startup."""
        if not self.batch_dir.exists():
            return

        for item in self.batch_dir.iterdir():
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
                        meta = json.load(f)
                    if "upload" in meta and meta["upload"].get("video_id"):
                        self._processed.add(item.name)
                except (json.JSONDecodeError, FileNotFoundError):
                    pass

        log.info(f"Found {len(self._processed)} already-processed videos")

    def _process_job(self, job: VideoJob) -> bool:
        """Process a single video job. Returns True on success."""
        from ..cli import _auto_process_single, AutoProcessResult

        log.info(f"Processing: {job.dir_name}")

        with self._lock:
            job.status = "processing"
            job.progress = "Starting..."
            self.state.current_job = job

        # Use existing _auto_process_single
        result = _auto_process_single(
            video_dir=job.dir_path,
            uploader=self._uploader,
            music_dir=self.music_dir,
            privacy=self.state.privacy,
            dry_run=False,
            user_config=self.user_config,
            log=log,
            playlist_id=self.state.playlist_id,
            delete_after_upload=self.state.delete_after_upload,
        )

        with self._lock:
            if result.succeeded:
                job.status = "completed"
                job.video_id = result.video_id
                job.video_url = f"https://youtu.be/{result.video_id}" if result.video_id else None
                self.state.completed.appendleft(job)
                self.state.total_processed += 1
                self._processed.add(job.dir_name)
                log.info(f"Completed: {job.dir_name} -> {job.video_url}")
            else:
                job.status = "failed"
                job.error = result.error
                self.state.failed.appendleft(job)
                self.state.total_failed += 1
                self._processed.add(job.dir_name)

                # Check for auth errors
                if result.status == "upload_failed" and result.error:
                    error_lower = result.error.lower()
                    if any(word in error_lower for word in ["401", "403", "auth", "credential", "token"]):
                        self.state.auth_error = result.error
                        self.state.status = ProcessorStatus.AUTH_REQUIRED
                        log.error(f"Auth error: {result.error}")

                log.error(f"Failed: {job.dir_name} - {result.error}")

            self.state.current_job = None

        return result.succeeded

    def run(self) -> None:
        """Main processor loop."""
        log.info("Video processor starting")

        with self._lock:
            self.state.status = ProcessorStatus.STARTING

        # Authenticate
        if not self._authenticate():
            log.error("Initial authentication failed")
            with self._lock:
                self.state.status = ProcessorStatus.AUTH_REQUIRED
        else:
            with self._lock:
                self.state.status = ProcessorStatus.RUNNING

        # Scan for existing processed videos
        self._scan_existing()

        # Main loop
        while not self._stop_event.is_set():
            try:
                # Wait if paused
                if not self._wait_if_paused():
                    break

                # Check auth state
                if self.state.status == ProcessorStatus.AUTH_REQUIRED:
                    time.sleep(1.0)
                    continue

                # Scan for new videos
                self._scan_for_new_videos()

                # Process next job if available
                job = None
                with self._lock:
                    if self.state.pending_queue:
                        job = self.state.pending_queue.pop(0)

                if job:
                    success = self._process_job(job)

                    # Wait between uploads (if successful and delay > 0)
                    if success and self.state.upload_delay > 0:
                        with self._lock:
                            self.state.status = ProcessorStatus.WAITING

                        log.info(f"Waiting {self.state.upload_delay}s before next upload")
                        self._interruptible_wait(self.state.upload_delay, "Upload delay")

                        with self._lock:
                            if self.state.status == ProcessorStatus.WAITING:
                                self.state.status = ProcessorStatus.RUNNING

                # Poll interval
                self._interruptible_wait(self.state.poll_interval, "Polling")

            except Exception as e:
                log.error(f"Processor loop error: {e}")
                time.sleep(1.0)

        with self._lock:
            self.state.status = ProcessorStatus.STOPPED

        log.info("Video processor stopped")
