"""Dashboard server for pendulum video processing.

Provides a web-based UI for monitoring and controlling video processing,
with proper integration into the existing codebase.
"""

from .app import run_dashboard
from .processor import ProcessorState, ProcessorStatus, VideoProcessor

__all__ = [
    "VideoProcessor",
    "ProcessorState",
    "ProcessorStatus",
    "run_dashboard",
]
