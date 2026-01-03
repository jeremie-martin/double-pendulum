"""Music management module for pendulum video post-processing."""

from .database import MusicDatabase, MusicTrack
from .manager import MusicManager
from .weights import MusicState, TrackWeight, get_music_state

__all__ = [
    "MusicDatabase",
    "MusicTrack",
    "MusicManager",
    "MusicState",
    "TrackWeight",
    "get_music_state",
]
