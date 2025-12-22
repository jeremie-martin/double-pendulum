"""Text overlay functionality for video processing."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Optional

from ..templates import CAPTION_STYLES, CaptionDefinition, format_count, get_caption_style
from .ffmpeg import FFmpegCommand


def resolve_timing(
    time_expr: float | str,
    boom_seconds: Optional[float],
    video_duration: Optional[float] = None,
) -> float:
    """Resolve a timing expression to absolute seconds.

    Args:
        time_expr: Time expression - can be:
            - float: absolute seconds
            - "boom": boom time
            - "boom+N" or "boom-N": relative to boom
            - "end": video end time
        boom_seconds: Boom time in seconds (required for boom-relative)
        video_duration: Video duration (required for "end")

    Returns:
        Absolute time in seconds

    Raises:
        ValueError: If expression can't be resolved
    """
    if isinstance(time_expr, (int, float)):
        return float(time_expr)

    time_expr = str(time_expr).strip().lower()

    # Handle "end"
    if time_expr == "end":
        if video_duration is None:
            raise ValueError("video_duration required for 'end' expression")
        return video_duration

    # Handle "boom" variants
    if "boom" in time_expr:
        if boom_seconds is None:
            raise ValueError("boom_seconds required for boom-relative expression")

        if time_expr == "boom":
            return boom_seconds

        # Parse boom+N or boom-N
        match = re.match(r"boom\s*([+-])\s*(\d+\.?\d*)", time_expr)
        if match:
            op, value = match.groups()
            offset = float(value)
            if op == "-":
                offset = -offset
            return boom_seconds + offset

    raise ValueError(f"Unknown time expression: {time_expr}")


def apply_captions(
    cmd: FFmpegCommand,
    style_name: str,
    boom_seconds: Optional[float],
    video_duration: Optional[float],
    pendulum_count: int,
    shorts_mode: bool = False,
    fontfile: Optional[Path] = None,
    fontsize: int = 48,
) -> FFmpegCommand:
    """Apply caption overlays based on a style.

    Args:
        cmd: FFmpegCommand to add drawtext filters to
        style_name: Caption style name from CAPTION_STYLES
        boom_seconds: Boom time in seconds
        video_duration: Total video duration
        pendulum_count: Number of pendulums (for {count} placeholder)
        shorts_mode: If True, adjust positioning for 9:16 aspect
        fontfile: Path to font file (optional)
        fontsize: Base font size

    Returns:
        The modified FFmpegCommand
    """
    style = get_caption_style(style_name)

    if not style:
        # No captions for this style (e.g., "minimal")
        return cmd

    # Format count for display
    count_formatted = format_count(pendulum_count)

    # Position: 82% from top for Shorts (avoids bottom UI), 88% for standard
    y_position = "h*0.82" if shorts_mode else "h*0.88"

    # Adjust font size for Shorts (larger for vertical format)
    actual_fontsize = int(fontsize * 1.2) if shorts_mode else fontsize

    for phase_name, caption_def in style.items():
        text, start_expr, end_expr = caption_def

        # Replace {count} placeholder
        text = text.replace("{count}", count_formatted)

        try:
            start_time = resolve_timing(start_expr, boom_seconds, video_duration)
            end_time = resolve_timing(end_expr, boom_seconds, video_duration)
        except ValueError as e:
            # Skip captions that can't be resolved (e.g., no boom_seconds)
            print(f"Warning: Skipping caption '{phase_name}': {e}")
            continue

        # Ensure times are valid
        start_time = max(0, start_time)
        if video_duration:
            end_time = min(end_time, video_duration)

        if start_time >= end_time:
            continue

        # Add the drawtext filter
        cmd.add_drawtext(
            text=text,
            x="(w-text_w)/2",  # Centered
            y=y_position,
            fontsize=actual_fontsize,
            fontcolor="white",
            fontfile=fontfile,
            enable=f"between(t,{start_time:.3f},{end_time:.3f})",
            box=True,
            boxcolor="black@0.6",
            boxborderw=12,
        )

    return cmd


def get_available_styles() -> list[str]:
    """Get list of available caption style names."""
    return list(CAPTION_STYLES.keys())
