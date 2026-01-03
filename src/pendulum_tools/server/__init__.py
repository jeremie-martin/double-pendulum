"""Pendulum Tools Web Server.

Provides a NiceGUI-based web dashboard for monitoring and controlling
the video processing pipeline.
"""

from .app import create_app, run_server
from .state import (
    ErrorCategory,
    JobInfo,
    JobStatus,
    WatcherState,
    WatcherStatus,
)

__all__ = [
    "create_app",
    "run_server",
    "ErrorCategory",
    "JobInfo",
    "JobStatus",
    "WatcherState",
    "WatcherStatus",
]
