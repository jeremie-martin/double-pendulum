"""Video effects for post-processing (zoom, pan, etc.)."""

from __future__ import annotations

from typing import Optional

from .ffmpeg import FFmpegCommand


def add_zoom_effect(
    cmd: FFmpegCommand,
    boom_seconds: float,
    zoom_factor: float = 1.3,
    ramp_duration: float = 0.5,
    hold_duration: float = 0.5,
    output_size: str = "1080x1080",
    fps: int = 60,
) -> FFmpegCommand:
    """Add a zoom punch-in effect centered on the boom moment.

    The effect smoothly zooms in before the boom, holds at peak zoom,
    then smoothly zooms back out.

    Timeline:
        [normal] -> [ramp up] -> [peak hold] -> [ramp down] -> [normal]
        |          |            |              |              |
        t=0    boom-ramp     boom          boom+hold    boom+hold+ramp

    Args:
        cmd: FFmpegCommand to add zoom filter to
        boom_seconds: Time of boom in seconds
        zoom_factor: Maximum zoom level (1.3 = 130% = 30% zoom in)
        ramp_duration: Duration of zoom in/out ramp in seconds
        hold_duration: Duration to hold at peak zoom
        output_size: Output video dimensions
        fps: Output frame rate

    Returns:
        The modified FFmpegCommand
    """
    # Calculate key time points
    zoom_start = boom_seconds - ramp_duration
    zoom_peak = boom_seconds
    zoom_hold_end = boom_seconds + hold_duration
    zoom_end = zoom_hold_end + ramp_duration

    # Build the zoom expression
    # FFmpeg zoompan 'z' is the zoom level where 1.0 = no zoom
    # Note: zoompan uses 'in_time' for input timestamp, not 't'
    #
    # The expression uses nested if() for different time segments:
    # 1. Before zoom_start: z = 1.0 (no zoom)
    # 2. zoom_start to zoom_peak: linear ramp from 1.0 to zoom_factor
    # 3. zoom_peak to zoom_hold_end: z = zoom_factor (hold)
    # 4. zoom_hold_end to zoom_end: linear ramp from zoom_factor to 1.0
    # 5. After zoom_end: z = 1.0 (no zoom)
    #
    # Linear interpolation: z = start + (end - start) * ((in_time - t_start) / duration)

    zoom_expr = (
        f"if(lt(in_time,{zoom_start:.3f}),1,"  # Before ramp: no zoom
        f"if(lt(in_time,{zoom_peak:.3f}),"  # During ramp up
        f"1+{zoom_factor - 1:.3f}*((in_time-{zoom_start:.3f})/{ramp_duration:.3f}),"
        f"if(lt(in_time,{zoom_hold_end:.3f}),{zoom_factor:.3f},"  # Hold at peak
        f"if(lt(in_time,{zoom_end:.3f}),"  # During ramp down
        f"{zoom_factor:.3f}-{zoom_factor - 1:.3f}*((in_time-{zoom_hold_end:.3f})/{ramp_duration:.3f}),"
        f"1))))"  # After: no zoom
    )

    # Center the zoom (pan to keep center point fixed)
    x_expr = "iw/2-(iw/zoom/2)"
    y_expr = "ih/2-(ih/zoom/2)"

    cmd.add_zoompan(
        zoom_expr=zoom_expr,
        x_expr=x_expr,
        y_expr=y_expr,
        fps=fps,
        duration=1,  # 1 output frame per input frame
        size=output_size,
    )

    return cmd


def add_simple_zoom(
    cmd: FFmpegCommand,
    zoom_factor: float = 1.2,
    output_size: str = "1080x1080",
    fps: int = 60,
) -> FFmpegCommand:
    """Add a constant zoom effect (static zoom, no animation).

    Useful for cropping/zooming into the center of the frame.

    Args:
        cmd: FFmpegCommand to add zoom filter to
        zoom_factor: Zoom level (1.2 = 120% = 20% zoom in)
        output_size: Output video dimensions
        fps: Output frame rate

    Returns:
        The modified FFmpegCommand
    """
    cmd.add_zoompan(
        zoom_expr=str(zoom_factor),
        x_expr="iw/2-(iw/zoom/2)",
        y_expr="ih/2-(ih/zoom/2)",
        fps=fps,
        duration=1,
        size=output_size,
    )

    return cmd


def add_slow_zoom(
    cmd: FFmpegCommand,
    start_zoom: float = 1.0,
    end_zoom: float = 1.1,
    video_duration: float = 10.0,
    output_size: str = "1080x1080",
    fps: int = 60,
) -> FFmpegCommand:
    """Add a slow, continuous zoom effect over the entire video.

    Creates a subtle "Ken Burns" style zoom.

    Args:
        cmd: FFmpegCommand to add zoom filter to
        start_zoom: Initial zoom level
        end_zoom: Final zoom level
        video_duration: Total video duration in seconds
        output_size: Output video dimensions
        fps: Output frame rate

    Returns:
        The modified FFmpegCommand
    """
    # Linear interpolation over video duration
    # Note: zoompan uses 'in_time' for input timestamp
    zoom_delta = end_zoom - start_zoom
    zoom_expr = f"{start_zoom}+{zoom_delta}*(in_time/{video_duration:.3f})"

    cmd.add_zoompan(
        zoom_expr=zoom_expr,
        x_expr="iw/2-(iw/zoom/2)",
        y_expr="ih/2-(ih/zoom/2)",
        fps=fps,
        duration=1,
        size=output_size,
    )

    return cmd
