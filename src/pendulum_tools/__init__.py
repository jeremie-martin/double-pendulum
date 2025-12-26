"""Pendulum Uploader - YouTube uploader for double pendulum simulations."""

from .models import ScoreData, VideoMetadata
from .templates import (
    CAPTION_STYLES,
    generate_description,
    generate_tags,
    generate_title,
    get_caption_style,
)
from .uploader import YouTubeUploader
from .processing import ProcessingConfig, ProcessingPipeline

__version__ = "0.1.0"
__all__ = [
    # Models
    "VideoMetadata",
    "ScoreData",
    # Uploader
    "YouTubeUploader",
    # Templates
    "generate_title",
    "generate_description",
    "generate_tags",
    "CAPTION_STYLES",
    "get_caption_style",
    # Processing
    "ProcessingConfig",
    "ProcessingPipeline",
]
