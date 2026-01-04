"""Music database models and loading.

Tracks are loaded from database.json (track metadata).
User preferences (weights, enabled) are stored in preferences.json.
Both files live in the same music directory.
"""

from __future__ import annotations

import json
import random
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from threading import Lock
from typing import Any, Optional

from ..logging import get_logger

log = get_logger(__name__)


@dataclass
class MusicTrack:
    """A music track with drop timing metadata and user preferences."""

    id: str
    title: str
    filepath: Path
    drop_time_ms: int

    # User preferences (loaded from preferences.json)
    weight: float = 1.0  # 0.0 = never select, higher = more likely
    enabled: bool = True  # False = excluded from selection
    use_count: int = 0  # How many times this track was used
    last_used: Optional[str] = None  # ISO timestamp

    @property
    def drop_time_seconds(self) -> float:
        """Convert drop time to seconds."""
        return self.drop_time_ms / 1000.0


class MusicDatabase:
    """Loads and manages music tracks from database.json.

    Track metadata comes from database.json (immutable, possibly auto-generated).
    User preferences (weight, enabled) come from preferences.json (mutable).

    Thread-safe for preference updates.
    """

    def __init__(self, music_dir: Path | str = "music"):
        """Load music database from directory.

        Args:
            music_dir: Path to directory containing database.json and audio files
        """
        self.music_dir = Path(music_dir)
        self.tracks: list[MusicTrack] = []
        self._tracks_by_id: dict[str, MusicTrack] = {}
        self._lock = Lock()
        self._load()

    def _load(self) -> None:
        """Load tracks from database.json and merge with preferences."""
        db_path = self.music_dir / "database.json"
        if not db_path.exists():
            raise FileNotFoundError(f"Music database not found: {db_path}")

        with open(db_path) as f:
            data = json.load(f)

        # Handle both array and object with "tracks" key
        tracks_data = data if isinstance(data, list) else data.get("tracks", [])

        # Load preferences if they exist
        prefs = self._load_preferences()

        for t in tracks_data:
            if t.get("id") and t.get("filepath") and t.get("drop_time_ms"):
                track_id = t["id"]
                track_prefs = prefs.get(track_id, {})

                track = MusicTrack(
                    id=track_id,
                    title=t.get("title", ""),
                    filepath=self.music_dir / t["filepath"],
                    drop_time_ms=t["drop_time_ms"],
                    weight=track_prefs.get("weight", 1.0),
                    enabled=track_prefs.get("enabled", True),
                    use_count=track_prefs.get("use_count", 0),
                    last_used=track_prefs.get("last_used"),
                )
                self.tracks.append(track)
                self._tracks_by_id[track_id] = track

    def _load_preferences(self) -> dict[str, dict[str, Any]]:
        """Load user preferences from preferences.json."""
        prefs_path = self.music_dir / "preferences.json"
        if not prefs_path.exists():
            return {}

        try:
            with open(prefs_path) as f:
                return json.load(f)
        except (json.JSONDecodeError, OSError) as e:
            log.warning(f"Failed to load music preferences: {e}")
            return {}

    def _save_preferences(self) -> None:
        """Save user preferences to preferences.json. Must hold lock."""
        prefs_path = self.music_dir / "preferences.json"
        prefs = {}

        for track in self.tracks:
            # Only save non-default values
            track_prefs = {}
            if track.weight != 1.0:
                track_prefs["weight"] = track.weight
            if not track.enabled:
                track_prefs["enabled"] = False
            if track.use_count > 0:
                track_prefs["use_count"] = track.use_count
            if track.last_used:
                track_prefs["last_used"] = track.last_used

            if track_prefs:
                prefs[track.id] = track_prefs

        try:
            with open(prefs_path, "w") as f:
                json.dump(prefs, f, indent=2)
                f.write("\n")
        except OSError as e:
            log.error(f"Failed to save music preferences: {e}")

    def reload(self) -> None:
        """Reload database and preferences from disk."""
        with self._lock:
            self.tracks.clear()
            self._tracks_by_id.clear()
            self._load()

    def get_track(self, track_id: str) -> Optional[MusicTrack]:
        """Get a track by its ID.

        Args:
            track_id: Track identifier

        Returns:
            MusicTrack if found, None otherwise
        """
        return self._tracks_by_id.get(track_id)

    def get_valid_tracks_for_boom(self, boom_seconds: float) -> list[MusicTrack]:
        """Return enabled tracks where drop happens after the boom.

        This ensures the visual boom syncs with or leads the music drop.
        Only returns tracks that are enabled and have weight > 0.

        Args:
            boom_seconds: Time of visual boom in seconds

        Returns:
            List of valid tracks for selection
        """
        return [
            t for t in self.tracks
            if t.enabled and t.weight > 0 and t.drop_time_seconds > boom_seconds
        ]

    def get_enabled_tracks(self) -> list[MusicTrack]:
        """Return all enabled tracks with weight > 0."""
        return [t for t in self.tracks if t.enabled and t.weight > 0]

    def pick_weighted(
        self,
        candidates: list[MusicTrack],
        seed: Optional[int] = None,
    ) -> Optional[MusicTrack]:
        """Pick a track using weighted random selection.

        Args:
            candidates: List of tracks to choose from
            seed: Optional random seed for reproducibility

        Returns:
            Selected track, or None if no valid candidates
        """
        if not candidates:
            return None

        # Filter to enabled tracks with weight > 0
        valid = [t for t in candidates if t.enabled and t.weight > 0]
        if not valid:
            return None

        weights = [t.weight for t in valid]
        rng = random.Random(seed) if seed is not None else random.Random()
        return rng.choices(valid, weights=weights, k=1)[0]

    def record_use(self, track_id: str) -> None:
        """Record that a track was used (updates use_count and last_used)."""
        with self._lock:
            track = self._tracks_by_id.get(track_id)
            if track:
                track.use_count += 1
                track.last_used = datetime.now().isoformat()
                self._save_preferences()
                log.debug(f"Recorded use of track {track_id} (count: {track.use_count})")

    def set_weight(self, track_id: str, weight: float) -> bool:
        """Set the weight for a track.

        Args:
            track_id: Track identifier
            weight: Weight value (0.0 = never select, higher = more likely)

        Returns:
            True if track was found and updated
        """
        with self._lock:
            track = self._tracks_by_id.get(track_id)
            if track:
                track.weight = max(0.0, weight)
                self._save_preferences()
                log.debug(f"Set weight for {track_id}: {track.weight}")
                return True
            return False

    def set_enabled(self, track_id: str, enabled: bool) -> bool:
        """Enable or disable a track.

        Args:
            track_id: Track identifier
            enabled: Whether the track should be available for selection

        Returns:
            True if track was found and updated
        """
        with self._lock:
            track = self._tracks_by_id.get(track_id)
            if track:
                track.enabled = enabled
                self._save_preferences()
                log.debug(f"Set enabled for {track_id}: {enabled}")
                return True
            return False

    def reset_all_weights(self) -> None:
        """Reset all track weights to 1.0."""
        with self._lock:
            for track in self.tracks:
                track.weight = 1.0
            self._save_preferences()
            log.info("Reset all track weights to 1.0")

    def enable_all(self) -> None:
        """Enable all tracks."""
        with self._lock:
            for track in self.tracks:
                track.enabled = True
            self._save_preferences()
            log.info("Enabled all tracks")

    def disable_all(self) -> None:
        """Disable all tracks."""
        with self._lock:
            for track in self.tracks:
                track.enabled = False
            self._save_preferences()
            log.info("Disabled all tracks")
