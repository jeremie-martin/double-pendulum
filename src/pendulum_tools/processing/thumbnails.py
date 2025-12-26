"""Thumbnail extraction from simulation videos."""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Optional

from .ffmpeg import extract_frame

logger = logging.getLogger(__name__)

# Offset after boom for thumbnail (video time)
POST_BOOM_THUMBNAIL_OFFSET = 1.0


def extract_thumbnails(
    video_path: Path,
    output_dir: Path,
    video_boom_seconds: Optional[float] = None,
    video_duration: Optional[float] = None,
) -> list[Path]:
    """Extract thumbnail at 1 second after boom (in video time).

    Args:
        video_path: Path to input video
        output_dir: Directory to save thumbnails
        video_boom_seconds: Time of boom in VIDEO seconds (boom_frame / fps)
        video_duration: Total video duration (for bounds checking)

    Returns:
        List of paths to extracted thumbnail files
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    thumbnails: list[Path] = []

    # Calculate thumbnail time: 1s after boom (video time)
    if video_boom_seconds is not None:
        thumbnail_time = video_boom_seconds + POST_BOOM_THUMBNAIL_OFFSET
    elif video_duration:
        # Fallback: middle of video
        thumbnail_time = video_duration / 2
    else:
        logger.warning("No boom time or video duration - cannot extract thumbnail")
        return thumbnails

    # Bounds check
    if video_duration and thumbnail_time > video_duration:
        thumbnail_time = max(0.1, video_duration - 0.5)

    output_path = output_dir / "thumbnail.jpg"

    try:
        extract_frame(video_path, output_path, thumbnail_time)
        thumbnails.append(output_path)
        logger.info(f"Extracted thumbnail at {thumbnail_time:.1f}s")
    except Exception as e:
        logger.warning(f"Failed to extract thumbnail: {e}")

    return thumbnails


def extract_thumbnail_at(
    video_path: Path,
    output_path: Path,
    timestamp: float,
) -> Path:
    """Extract a single thumbnail at a specific timestamp.

    Args:
        video_path: Path to input video
        output_path: Path for output image
        timestamp: Time in seconds

    Returns:
        Path to extracted thumbnail
    """
    output_path.parent.mkdir(parents=True, exist_ok=True)
    extract_frame(video_path, output_path, timestamp)
    return output_path
