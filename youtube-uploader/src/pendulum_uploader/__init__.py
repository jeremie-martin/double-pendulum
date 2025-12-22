"""Pendulum Uploader - YouTube uploader for double pendulum simulations."""

from .models import VideoMetadata
from .templates import generate_description, generate_tags, generate_title
from .uploader import YouTubeUploader

__version__ = "0.1.0"
__all__ = [
    "VideoMetadata",
    "YouTubeUploader",
    "generate_title",
    "generate_description",
    "generate_tags",
]
