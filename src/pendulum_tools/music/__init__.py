"""Music management module for pendulum video post-processing.

Tracks are loaded from database.json in the music directory.
User preferences (weights, enabled) are stored in preferences.json.
"""

from .database import MusicDatabase, MusicTrack
from .manager import MusicManager

__all__ = [
    "MusicDatabase",
    "MusicTrack",
    "MusicManager",
]
