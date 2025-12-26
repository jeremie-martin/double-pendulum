"""Music database models and loading."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass
class MusicTrack:
    """A music track with drop timing metadata."""

    id: str
    title: str
    filepath: Path
    drop_time_ms: int

    @property
    def drop_time_seconds(self) -> float:
        """Convert drop time to seconds."""
        return self.drop_time_ms / 1000.0


class MusicDatabase:
    """Loads and manages music tracks from database.json."""

    def __init__(self, music_dir: Path | str = "music"):
        """Load music database from directory.

        Args:
            music_dir: Path to directory containing database.json and audio files
        """
        self.music_dir = Path(music_dir)
        self.tracks: list[MusicTrack] = []
        self._load()

    def _load(self) -> None:
        """Load tracks from database.json."""
        db_path = self.music_dir / "database.json"
        if not db_path.exists():
            raise FileNotFoundError(f"Music database not found: {db_path}")

        with open(db_path) as f:
            data = json.load(f)

        # Handle both array and object with "tracks" key
        tracks_data = data if isinstance(data, list) else data.get("tracks", [])

        for t in tracks_data:
            if t.get("id") and t.get("filepath") and t.get("drop_time_ms"):
                self.tracks.append(
                    MusicTrack(
                        id=t["id"],
                        title=t.get("title", ""),
                        filepath=self.music_dir / t["filepath"],
                        drop_time_ms=t["drop_time_ms"],
                    )
                )

    def get_track(self, track_id: str) -> Optional[MusicTrack]:
        """Get a track by its ID.

        Args:
            track_id: Track identifier

        Returns:
            MusicTrack if found, None otherwise
        """
        for track in self.tracks:
            if track.id == track_id:
                return track
        return None

    def get_valid_tracks_for_boom(self, boom_seconds: float) -> list[MusicTrack]:
        """Return tracks where drop happens after the boom.

        This ensures the visual boom syncs with or leads the music drop.

        Args:
            boom_seconds: Time of visual boom in seconds

        Returns:
            List of tracks with drop_time > boom_seconds
        """
        return [t for t in self.tracks if t.drop_time_seconds > boom_seconds]
