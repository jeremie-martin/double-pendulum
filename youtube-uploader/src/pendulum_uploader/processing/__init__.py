"""Video post-processing module for pendulum simulation videos."""

from .ffmpeg import FFmpegCommand
from .thumbnails import extract_thumbnails
from .overlays import apply_captions, resolve_timing
from .effects import add_zoom_effect
from .pipeline import ProcessingPipeline, ProcessingConfig

__all__ = [
    "FFmpegCommand",
    "extract_thumbnails",
    "apply_captions",
    "resolve_timing",
    "add_zoom_effect",
    "ProcessingPipeline",
    "ProcessingConfig",
]
