"""Video post-processing module for pendulum simulation videos."""

from .ffmpeg import FFmpegCommand, is_nvenc_available
from .motion import (
    BoomPunchConfig,
    MotionConfig,
    ShakeConfig,
    SlowZoomConfig,
    apply_motion_effects,
    build_motion_filters,
)
from .pipeline import ProcessingConfig, ProcessingPipeline, ProcessingResult
from .subtitles_ass import (
    ASSGenerator,
    CaptionPreset,
    generate_ass_from_resolved,
)
from .templates import (
    ResolvedCaption,
    TemplateConfig,
    TemplateLibrary,
    TextPoolLibrary,
    load_template_system,
    resolve_template,
)
from .thumbnails import extract_thumbnails

__all__ = [
    # FFmpeg
    "FFmpegCommand",
    "is_nvenc_available",
    # Thumbnails
    "extract_thumbnails",
    # Motion effects
    "apply_motion_effects",
    "build_motion_filters",
    "MotionConfig",
    "SlowZoomConfig",
    "BoomPunchConfig",
    "ShakeConfig",
    # Pipeline
    "ProcessingPipeline",
    "ProcessingConfig",
    "ProcessingResult",
    # Subtitles
    "ASSGenerator",
    "CaptionPreset",
    "generate_ass_from_resolved",
    # Templates
    "TemplateLibrary",
    "TextPoolLibrary",
    "TemplateConfig",
    "ResolvedCaption",
    "load_template_system",
    "resolve_template",
]
