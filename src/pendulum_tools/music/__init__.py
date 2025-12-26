"""Music management module for pendulum video post-processing."""

from .database import MusicDatabase, MusicTrack
from .manager import MusicManager

__all__ = ["MusicDatabase", "MusicTrack", "MusicManager"]
