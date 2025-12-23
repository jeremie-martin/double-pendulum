"""Motion effects for video post-processing (zoom, shake, etc.).

Uses FFmpeg scale+crop filters for smooth video zoom effects.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from .ffmpeg import FFmpegCommand


@dataclass
class SlowZoomConfig:
    """Configuration for slow/gradual zoom (Ken Burns style)."""

    start: float = 1.0  # Starting zoom level (1.0 = no zoom)
    end: float = 1.1  # Ending zoom level


@dataclass
class BoomPunchConfig:
    """Configuration for boom punch-in effect."""

    factor: float = 1.3  # Peak zoom level
    ramp_in: float = 0.4  # Seconds to reach peak
    hold: float = 0.4  # Seconds at peak
    ramp_out: float = 0.4  # Seconds to return to normal


@dataclass
class ShakeConfig:
    """Configuration for screen shake effect."""

    frames: int = 4  # Number of frames to shake
    intensity: int = 8  # Pixels of displacement


@dataclass
class MotionConfig:
    """Combined motion configuration for a video."""

    slow_zoom: Optional[SlowZoomConfig] = None
    boom_punch: Optional[BoomPunchConfig] = None
    shake: Optional[ShakeConfig] = None


def build_slow_zoom_expr(
    slow_zoom: SlowZoomConfig,
    video_duration: float,
    time_var: str = "in_time",
) -> str:
    """Build linear zoom expression from start to end over video duration.

    Args:
        slow_zoom: Slow zoom configuration
        video_duration: Total video duration in seconds
        time_var: Time variable name ('t' for most filters, 'in_time' for zoompan)

    Returns:
        FFmpeg expression for zoom factor.
    """
    # Linear interpolation: start + (end - start) * (t / duration)
    # Clamp t to video_duration to avoid extrapolation
    start = slow_zoom.start
    delta = slow_zoom.end - slow_zoom.start
    return f"({start}+{delta}*min({time_var}/{video_duration},1))"


def build_boom_punch_expr(
    boom_punch: BoomPunchConfig,
    boom_seconds: float,
    time_var: str = "in_time",
) -> str:
    """Build boom punch zoom expression.

    Creates a zoom spike at boom_seconds:
    1.0 -> ramp to peak -> hold -> ramp back to 1.0

    Args:
        boom_punch: Boom punch configuration
        boom_seconds: Time of boom in seconds
        time_var: Time variable name ('t' for most filters, 'in_time' for zoompan)

    Returns:
        FFmpeg expression for zoom factor.
    """
    # Timeline points
    t0 = boom_seconds - boom_punch.ramp_in  # Start ramping
    t1 = boom_seconds  # Reach peak
    t2 = boom_seconds + boom_punch.hold  # Start ramping down
    t3 = t2 + boom_punch.ramp_out  # Back to 1.0

    peak = boom_punch.factor
    ramp_in = boom_punch.ramp_in
    ramp_out = boom_punch.ramp_out

    # Piecewise function using nested if():
    # - t < t0: 1.0 (no zoom)
    # - t0 <= t < t1: linear ramp from 1.0 to peak
    # - t1 <= t < t2: hold at peak
    # - t2 <= t < t3: linear ramp from peak to 1.0
    # - t >= t3: 1.0 (no zoom)
    t = time_var

    expr = (
        f"if(lt({t},{t0}),1,"
        f"if(lt({t},{t1}),1+{peak-1}*({t}-{t0})/{ramp_in},"
        f"if(lt({t},{t2}),{peak},"
        f"if(lt({t},{t3}),{peak}-{peak-1}*({t}-{t2})/{ramp_out},"
        f"1))))"
    )
    return expr


def build_zoom_filters(
    width: int,
    height: int,
    zoom_expr: str,
    fps: int = 60,
) -> list[str]:
    """Build zoom filter using zoompan for proper center-locked zoom.

    Args:
        width: Original/output width
        height: Original/output height
        zoom_expr: FFmpeg expression for zoom factor (use 'in_time' for time)
        fps: Frame rate for output

    Returns:
        List containing the zoompan filter string
    """
    # zoompan filter with:
    # - z: zoom expression (must use 'in_time' not 't' for time)
    # - x/y: position formulas that keep center locked
    # - d=1: process each frame (not for stills)
    # - s: output size
    # - fps: maintain frame rate
    #
    # The x/y formulas: iw/2-(iw/zoom/2) centers the zoom on the middle
    return [
        f"zoompan=z='{zoom_expr}':"
        f"x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':"
        f"d=1:s={width}x{height}:fps={fps}"
    ]


def build_shake_filters(
    boom_seconds: float,
    fps: int,
    shake: ShakeConfig,
) -> list[str]:
    """Build shake filter strings.

    Args:
        boom_seconds: Time of boom in seconds
        fps: Video frame rate
        shake: Shake configuration

    Returns:
        List of filter strings
    """
    boom_frame = int(boom_seconds * fps)
    end_frame = boom_frame + shake.frames
    intensity = shake.intensity

    shake_x = f"if(gte(n,{boom_frame})*lt(n,{end_frame}),{intensity}*sin(n*17),0)"
    shake_y = f"if(gte(n,{boom_frame})*lt(n,{end_frame}),{intensity}*cos(n*23),0)"

    pad = intensity * 2
    return [
        f"pad=iw+{pad*2}:ih+{pad*2}:{pad}:{pad}:black",
        f"crop=iw-{pad*2}:ih-{pad*2}:'{pad}+{shake_x}':'{pad}+{shake_y}'",
    ]


def build_motion_filters(
    motion: MotionConfig,
    boom_seconds: float,
    video_duration: float,
    width: int,
    height: int,
    fps: int = 60,
) -> list[str]:
    """Build all motion filter strings from a MotionConfig.

    Args:
        motion: Motion configuration
        boom_seconds: Time of boom in seconds
        video_duration: Total video duration
        width: Original video width
        height: Original video height
        fps: Video frame rate

    Returns:
        List of filter strings (can be joined with ',')
    """
    filters = []

    # Build zoom expression (using 'in_time' for zoompan filter)
    zoom_parts = []
    if motion.slow_zoom:
        zoom_parts.append(build_slow_zoom_expr(motion.slow_zoom, video_duration, time_var="in_time"))
    if motion.boom_punch and boom_seconds > 0:
        zoom_parts.append(build_boom_punch_expr(motion.boom_punch, boom_seconds, time_var="in_time"))

    if zoom_parts:
        zoom_expr = "*".join(zoom_parts) if len(zoom_parts) > 1 else zoom_parts[0]
        filters.extend(build_zoom_filters(width, height, zoom_expr, fps=fps))

    # Add shake after zoom (shake uses frame number 'n', not time)
    if motion.shake and boom_seconds > 0:
        filters.extend(build_shake_filters(boom_seconds, fps, motion.shake))

    return filters


def apply_zoom(
    cmd: FFmpegCommand,
    width: int,
    height: int,
    zoom_expr: str,
) -> FFmpegCommand:
    """Apply zoom effect using a simple, robust scale+crop approach.

    The approach:
    1. Scale the video by zoom factor (making it larger)
    2. Crop from center back to original size

    Args:
        cmd: FFmpegCommand to modify
        width: Original/output width
        height: Original/output height
        zoom_expr: FFmpeg expression for zoom factor (e.g., "1.2" or "1+0.1*t")

    Returns:
        Modified FFmpegCommand
    """
    for f in build_zoom_filters(width, height, zoom_expr):
        cmd.add_filter(f)
    return cmd


def apply_shake(
    cmd: FFmpegCommand,
    boom_seconds: float,
    fps: int,
    shake: ShakeConfig,
) -> FFmpegCommand:
    """Apply screen shake effect at boom moment.

    Uses pad+crop to translate the frame.

    Args:
        cmd: FFmpegCommand to modify
        boom_seconds: Time of boom in seconds
        fps: Video frame rate
        shake: Shake configuration

    Returns:
        Modified FFmpegCommand
    """
    for f in build_shake_filters(boom_seconds, fps, shake):
        cmd.add_filter(f)

    return cmd


def apply_motion_effects(
    cmd: FFmpegCommand,
    motion: MotionConfig,
    boom_seconds: float,
    video_duration: float,
    width: int,
    height: int,
    fps: int = 60,
) -> FFmpegCommand:
    """Apply all motion effects from a MotionConfig.

    Effects are combined multiplicatively for zoom:
    total_zoom = slow_zoom * boom_punch

    Args:
        cmd: FFmpegCommand to modify
        motion: Motion configuration
        boom_seconds: Time of boom in seconds
        video_duration: Total video duration
        width: Original video width
        height: Original video height
        fps: Video frame rate

    Returns:
        Modified FFmpegCommand
    """
    # Build combined zoom expression
    zoom_parts = []

    if motion.slow_zoom:
        zoom_parts.append(build_slow_zoom_expr(motion.slow_zoom, video_duration))

    if motion.boom_punch and boom_seconds > 0:
        zoom_parts.append(build_boom_punch_expr(motion.boom_punch, boom_seconds))

    # Apply zoom if any zoom effects configured
    if zoom_parts:
        if len(zoom_parts) == 1:
            zoom_expr = zoom_parts[0]
        else:
            zoom_expr = "*".join(zoom_parts)
        apply_zoom(cmd, width, height, zoom_expr)

    # Apply shake after zoom
    if motion.shake and boom_seconds > 0:
        apply_shake(cmd, boom_seconds, fps, motion.shake)

    return cmd
