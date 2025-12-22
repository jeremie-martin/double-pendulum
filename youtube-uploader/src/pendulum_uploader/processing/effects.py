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
    width: int = 1080,
    height: int = 1080,
) -> FFmpegCommand:
    """Add a zoom punch-in effect centered on the boom moment.

    Uses scale + crop filters for smooth video zoom (zoompan is for stills).

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
        width: Video width
        height: Video height

    Returns:
        The modified FFmpegCommand
    """
    # Calculate key time points
    zoom_start = boom_seconds - ramp_duration
    zoom_peak = boom_seconds
    zoom_hold_end = boom_seconds + hold_duration
    zoom_end = zoom_hold_end + ramp_duration

    # Build the zoom expression using 't' (time in seconds)
    # zoom goes from 1.0 to zoom_factor and back
    # Using nested if() for different time segments
    zoom_expr = (
        f"if(lt(t,{zoom_start:.3f}),1,"  # Before ramp: no zoom
        f"if(lt(t,{zoom_peak:.3f}),"  # During ramp up
        f"1+{zoom_factor - 1:.3f}*((t-{zoom_start:.3f})/{ramp_duration:.3f}),"
        f"if(lt(t,{zoom_hold_end:.3f}),{zoom_factor:.3f},"  # Hold at peak
        f"if(lt(t,{zoom_end:.3f}),"  # During ramp down
        f"{zoom_factor:.3f}-{zoom_factor - 1:.3f}*((t-{zoom_hold_end:.3f})/{ramp_duration:.3f}),"
        f"1))))"  # After: no zoom
    )

    # Scale up by zoom factor, then crop back to original size (centered)
    # scale: multiply dimensions by zoom (eval=frame enables per-frame expressions)
    # crop: take center portion at original size
    scale_w = f"trunc(iw*({zoom_expr})/2)*2"  # Ensure even dimensions
    scale_h = f"trunc(ih*({zoom_expr})/2)*2"

    # Crop from center (use 'in_w' and 'in_h' for crop input dimensions)
    crop_x = f"(in_w-{width})/2"
    crop_y = f"(in_h-{height})/2"

    # Add scale filter with eval=frame for per-frame expression evaluation
    cmd.add_filter(f"scale=w='{scale_w}':h='{scale_h}':eval=frame")
    cmd.add_filter(f"crop={width}:{height}:{crop_x}:{crop_y}")

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
