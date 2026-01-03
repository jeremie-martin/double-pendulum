"""Music weight persistence and weighted selection."""

from __future__ import annotations

import json
import random
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from threading import Lock
from typing import TYPE_CHECKING, Any, Optional

if TYPE_CHECKING:
    from .database import MusicTrack


def get_weights_path() -> Path:
    """Get the path to the music weights file."""
    import os

    # Use XDG_DATA_HOME if set, otherwise ~/.local/share
    data_home = os.environ.get("XDG_DATA_HOME")
    if data_home:
        base = Path(data_home)
    else:
        base = Path.home() / ".local" / "share"

    return base / "pendulum-tools" / "music_weights.json"


@dataclass
class TrackWeight:
    """Weight configuration for a single music track."""

    track_id: str
    weight: float = 1.0  # 0.0 = disabled, higher = more likely
    enabled: bool = True
    use_count: int = 0  # Track usage statistics
    last_used: Optional[str] = None  # ISO timestamp

    def to_dict(self) -> dict[str, Any]:
        """Convert to dictionary for JSON serialization."""
        return {
            "track_id": self.track_id,
            "weight": self.weight,
            "enabled": self.enabled,
            "use_count": self.use_count,
            "last_used": self.last_used,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "TrackWeight":
        """Create from dictionary."""
        return cls(
            track_id=data["track_id"],
            weight=data.get("weight", 1.0),
            enabled=data.get("enabled", True),
            use_count=data.get("use_count", 0),
            last_used=data.get("last_used"),
        )


class MusicState:
    """Thread-safe music configuration state with persistence.

    Manages weights, enabled/disabled state, and usage statistics
    for music tracks. Persists to JSON file.
    """

    def __init__(self, weights_file: Optional[Path] = None):
        self._lock = Lock()
        self._weights: dict[str, TrackWeight] = {}
        self._weights_file = weights_file or get_weights_path()
        self._load()

    def _load(self) -> None:
        """Load weights from file."""
        if not self._weights_file.exists():
            return

        try:
            with open(self._weights_file) as f:
                data = json.load(f)

            for track_id, weight_data in data.items():
                if isinstance(weight_data, dict):
                    self._weights[track_id] = TrackWeight.from_dict(weight_data)
        except (json.JSONDecodeError, KeyError, TypeError):
            # Corrupted file - start fresh
            self._weights = {}

    def _save(self) -> None:
        """Save weights to file (must be called with lock held)."""
        self._weights_file.parent.mkdir(parents=True, exist_ok=True)

        data = {track_id: tw.to_dict() for track_id, tw in self._weights.items()}

        with open(self._weights_file, "w") as f:
            json.dump(data, f, indent=2)
            f.write("\n")

    def get_weight(self, track_id: str) -> TrackWeight:
        """Get weight for a track (creates default if not exists)."""
        with self._lock:
            if track_id not in self._weights:
                self._weights[track_id] = TrackWeight(track_id=track_id)
            return TrackWeight(
                track_id=self._weights[track_id].track_id,
                weight=self._weights[track_id].weight,
                enabled=self._weights[track_id].enabled,
                use_count=self._weights[track_id].use_count,
                last_used=self._weights[track_id].last_used,
            )

    def set_weight(self, track_id: str, weight: float) -> None:
        """Set weight for a track."""
        with self._lock:
            if track_id not in self._weights:
                self._weights[track_id] = TrackWeight(track_id=track_id)
            self._weights[track_id].weight = max(0.0, weight)
            self._save()

    def set_enabled(self, track_id: str, enabled: bool) -> None:
        """Enable/disable a track."""
        with self._lock:
            if track_id not in self._weights:
                self._weights[track_id] = TrackWeight(track_id=track_id)
            self._weights[track_id].enabled = enabled
            self._save()

    def record_use(self, track_id: str) -> None:
        """Record that a track was used."""
        with self._lock:
            if track_id not in self._weights:
                self._weights[track_id] = TrackWeight(track_id=track_id)
            self._weights[track_id].use_count += 1
            self._weights[track_id].last_used = datetime.now().isoformat()
            self._save()

    def is_enabled(self, track_id: str) -> bool:
        """Check if a track is enabled."""
        with self._lock:
            if track_id not in self._weights:
                return True  # Default: enabled
            return self._weights[track_id].enabled

    def get_all_weights(self) -> dict[str, TrackWeight]:
        """Get a copy of all weights."""
        with self._lock:
            return {
                track_id: TrackWeight(
                    track_id=tw.track_id,
                    weight=tw.weight,
                    enabled=tw.enabled,
                    use_count=tw.use_count,
                    last_used=tw.last_used,
                )
                for track_id, tw in self._weights.items()
            }

    def reset_all_weights(self) -> None:
        """Reset all weights to 1.0."""
        with self._lock:
            for tw in self._weights.values():
                tw.weight = 1.0
            self._save()

    def enable_all(self) -> None:
        """Enable all tracks."""
        with self._lock:
            for tw in self._weights.values():
                tw.enabled = True
            self._save()

    def disable_all(self) -> None:
        """Disable all tracks."""
        with self._lock:
            for tw in self._weights.values():
                tw.enabled = False
            self._save()

    def pick_weighted(
        self,
        valid_tracks: list["MusicTrack"],
        seed: Optional[int] = None,
    ) -> Optional["MusicTrack"]:
        """Pick a track using weights.

        Args:
            valid_tracks: List of tracks that are valid for selection
                         (already filtered by boom time, etc.)
            seed: Optional random seed

        Returns:
            Selected track, or None if no valid enabled tracks
        """
        with self._lock:
            # Filter to enabled tracks with weight > 0
            candidates = []
            weights = []

            for track in valid_tracks:
                tw = self._weights.get(track.id)
                # If no weight record, use defaults (enabled, weight=1.0)
                if tw is None:
                    candidates.append(track)
                    weights.append(1.0)
                elif tw.enabled and tw.weight > 0:
                    candidates.append(track)
                    weights.append(tw.weight)

            if not candidates:
                return None

            # Weighted random selection
            rng = random.Random(seed) if seed is not None else random.Random()
            selected = rng.choices(candidates, weights=weights, k=1)[0]

            return selected

    def snapshot(self) -> dict[str, Any]:
        """Get a snapshot for UI rendering."""
        with self._lock:
            return {
                track_id: tw.to_dict()
                for track_id, tw in self._weights.items()
            }


# Global music state (lazy initialized)
_music_state: Optional[MusicState] = None


def get_music_state() -> MusicState:
    """Get or create the global music state."""
    global _music_state
    if _music_state is None:
        _music_state = MusicState()
    return _music_state


def set_music_state(state: MusicState) -> None:
    """Set the global music state (for testing)."""
    global _music_state
    _music_state = state
