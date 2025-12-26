"""Thumbnail extraction from simulation videos."""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Optional

from .ffmpeg import extract_frame
from ..constants import PRE_BOOM_OFFSET_SECONDS

logger = logging.getLogger(__name__)


def extract_thumbnails(
    video_path: Path,
    output_dir: Path,
    boom_seconds: Optional[float] = None,
    best_frame_seconds: Optional[float] = None,
    video_duration: Optional[float] = None,
) -> list[Path]:
    """Extract key frames as thumbnail images.

    Extracts thumbnails at significant moments:
    - pre_boom: PRE_BOOM_OFFSET_SECONDS before boom (tension moment)
    - boom: Exact moment of chaos emergence
    - best_caustic: Frame with peak causticness (if score available)

    Args:
        video_path: Path to input video
        output_dir: Directory to save thumbnails
        boom_seconds: Time of boom in seconds (from metadata)
        best_frame_seconds: Time of best causticness frame
        video_duration: Total video duration (for bounds checking)

    Returns:
        List of paths to extracted thumbnail files
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    thumbnails: list[Path] = []

    # Define extraction points
    extractions: list[tuple[str, Optional[float]]] = []

    if boom_seconds is not None:
        # Pre-boom: offset before (but not negative)
        pre_boom_time = max(0.0, boom_seconds - PRE_BOOM_OFFSET_SECONDS)
        extractions.append(("pre_boom", pre_boom_time))

        # Boom moment
        extractions.append(("boom", boom_seconds))

    if best_frame_seconds is not None:
        # Best causticness frame
        extractions.append(("best_caustic", best_frame_seconds))

    # If no boom info, try to get a mid-video frame
    if not extractions and video_duration:
        extractions.append(("mid", video_duration / 2))

    # Extract each thumbnail
    for name, timestamp in extractions:
        if timestamp is None:
            continue

        # Bounds check
        if video_duration and timestamp > video_duration:
            timestamp = video_duration - 0.1

        output_path = output_dir / f"thumbnail_{name}.jpg"

        try:
            extract_frame(video_path, output_path, timestamp)
            thumbnails.append(output_path)
        except Exception as e:
            # Log but don't fail on individual thumbnail errors
            logger.warning(f"Failed to extract {name} thumbnail: {e}")

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
