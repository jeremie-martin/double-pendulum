"""Music muxing and management."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Optional

from .database import MusicDatabase, MusicTrack


class MusicManager:
    """Manages music selection and video muxing.

    Selection uses weighted random based on track preferences stored
    in preferences.json (same directory as database.json).
    """

    def __init__(self, music_dir: Path | str = "music"):
        """Initialize music manager.

        Args:
            music_dir: Path to directory containing database.json and audio files
        """
        self.music_dir = Path(music_dir)
        self.database = MusicDatabase(self.music_dir)

    def reload(self) -> None:
        """Reload the music database and preferences from disk."""
        self.database.reload()

    @property
    def tracks(self) -> list[MusicTrack]:
        """Get all tracks (including disabled ones)."""
        return self.database.tracks

    @property
    def enabled_tracks(self) -> list[MusicTrack]:
        """Get only enabled tracks with weight > 0."""
        return self.database.get_enabled_tracks()

    def get_track(self, track_id: str) -> Optional[MusicTrack]:
        """Get a track by ID."""
        return self.database.get_track(track_id)

    def random_track(self, seed: Optional[int] = None) -> MusicTrack:
        """Get a random track using weighted selection.

        Args:
            seed: Optional random seed for reproducibility

        Returns:
            Random track (weighted by preferences)

        Raises:
            ValueError: If no enabled tracks available
        """
        candidates = self.database.get_enabled_tracks()
        if not candidates:
            raise ValueError("No enabled tracks available")

        selected = self.database.pick_weighted(candidates, seed)
        if selected:
            self.database.record_use(selected.id)
            return selected

        raise ValueError("No tracks available for selection")

    def pick_track_for_boom(
        self,
        boom_seconds: float,
        seed: Optional[int] = None,
    ) -> Optional[MusicTrack]:
        """Pick a weighted random track where drop > boom_seconds.

        This ensures the visual boom syncs with or leads the music drop.
        Uses weighted selection based on track preferences.

        Args:
            boom_seconds: Time of visual boom in seconds
            seed: Optional random seed

        Returns:
            Track where drop happens after boom, or None if no valid track
        """
        valid = self.database.get_valid_tracks_for_boom(boom_seconds)
        if not valid:
            return None

        selected = self.database.pick_weighted(valid, seed)
        if selected:
            self.database.record_use(selected.id)
            return selected

        return None

    # Preference management (delegates to database)
    def set_weight(self, track_id: str, weight: float) -> bool:
        """Set the weight for a track."""
        return self.database.set_weight(track_id, weight)

    def set_enabled(self, track_id: str, enabled: bool) -> bool:
        """Enable or disable a track."""
        return self.database.set_enabled(track_id, enabled)

    def reset_all_weights(self) -> None:
        """Reset all track weights to 1.0."""
        self.database.reset_all_weights()

    def enable_all(self) -> None:
        """Enable all tracks."""
        self.database.enable_all()

    def disable_all(self) -> None:
        """Disable all tracks."""
        self.database.disable_all()

    @staticmethod
    def mux_with_audio(
        video_path: Path,
        audio_path: Path,
        output_path: Path,
        boom_frame: int,
        drop_time_ms: int,
        video_fps: int,
        verbose: bool = False,
    ) -> bool:
        """Mux video with audio, aligning boom with drop.

        Uses lossless stream copy for video. Audio is copied if possible,
        otherwise transcoded to AAC.

        Args:
            video_path: Path to input video (no audio or will be replaced)
            audio_path: Path to audio file
            output_path: Path for output video
            boom_frame: Frame number of visual boom
            drop_time_ms: Time of music drop in milliseconds
            video_fps: Video frame rate
            verbose: If True, print FFmpeg output

        Returns:
            True if successful, False otherwise
        """
        if not video_path.exists():
            raise FileNotFoundError(f"Video file not found: {video_path}")
        if not audio_path.exists():
            raise FileNotFoundError(f"Audio file not found: {audio_path}")

        # Calculate audio offset to align boom with drop
        # boom_time_video = boom_frame / video_fps (seconds)
        # drop_time_audio = drop_time_ms / 1000 (seconds)
        # We want: at boom, audio should be at drop_time
        # offset = drop_time - boom_time
        boom_time = boom_frame / video_fps
        drop_time = drop_time_ms / 1000.0
        audio_offset = drop_time - boom_time

        cmd = ["ffmpeg", "-y"]

        if audio_offset >= 0:
            # Audio starts after video starts - seek into audio
            cmd.extend(["-i", str(video_path)])
            cmd.extend(["-ss", str(audio_offset)])
            cmd.extend(["-i", str(audio_path)])
        else:
            # Audio drop before boom - start audio at beginning
            # (Could pad video with black frames for perfect sync, but this is simpler)
            cmd.extend(["-i", str(video_path)])
            cmd.extend(["-i", str(audio_path)])

        # Lossless video copy, copy or transcode audio
        cmd.extend(
            [
                "-c:v",
                "copy",
                "-c:a",
                "aac",  # Transcode to AAC for compatibility
                "-map",
                "0:v:0",
                "-map",
                "1:a:0",
                "-shortest",
                str(output_path),
            ]
        )

        if verbose:
            print(f"Running: {' '.join(cmd)}")
            result = subprocess.run(cmd)
        else:
            result = subprocess.run(cmd, capture_output=True, text=True)

        return result.returncode == 0

    @staticmethod
    def update_metadata_with_music(
        metadata_path: Path,
        track: MusicTrack,
    ) -> None:
        """Update metadata.json with music track information.

        Args:
            metadata_path: Path to metadata.json
            track: Music track that was added
        """
        with open(metadata_path) as f:
            data = json.load(f)

        data["music"] = {
            "track_id": track.id,
            "title": track.title,
            "drop_time_ms": track.drop_time_ms,
        }

        with open(metadata_path, "w") as f:
            json.dump(data, f, indent=2)
