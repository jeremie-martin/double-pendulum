"""Motion effects for video post-processing (zoom, shake, etc.).

Uses FFmpeg scale+crop filters for smooth video zoom effects.
The zoompan filter is designed for stills and doesn't work well with video.
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


def build_combined_zoom_expr(
    boom_seconds: float,
    video_duration: float,
    slow_zoom: Optional[SlowZoomConfig] = None,
    boom_punch: Optional[BoomPunchConfig] = None,
) -> str:
    """Build a combined zoom expression for slow zoom + boom punch.

    The zoom levels are multiplied together:
    - slow_zoom: gradual increase from start to end over the video
    - boom_punch: spike at boom moment, returns to 1.0

    Args:
        boom_seconds: Time of boom in seconds
        video_duration: Total video duration
        slow_zoom: Slow zoom configuration (or None)
        boom_punch: Boom punch configuration (or None)

    Returns:
        FFmpeg expression string for combined zoom factor
    """
    parts = []

    # Slow zoom: linear interpolation from start to end
    if slow_zoom:
        # Zoom from start to end over the duration up to boom, then continue at same rate
        # This creates a subtle push-in effect
        zoom_rate = (slow_zoom.end - slow_zoom.start) / boom_seconds if boom_seconds > 0 else 0
        # slow_zoom(t) = start + rate * min(t, video_duration)
        parts.append(f"({slow_zoom.start:.4f}+{zoom_rate:.6f}*min(t,{video_duration:.3f}))")

    # Boom punch: spike at boom moment
    if boom_punch:
        # Timeline: [1.0] -> [ramp up] -> [hold at peak] -> [ramp down] -> [1.0]
        punch_start = boom_seconds - boom_punch.ramp_in
        punch_peak = boom_seconds
        punch_hold_end = boom_seconds + boom_punch.hold
        punch_end = punch_hold_end + boom_punch.ramp_out

        # Amount to add at peak (factor - 1.0)
        punch_amount = boom_punch.factor - 1.0

        # Build nested if expression for punch envelope
        punch_expr = (
            f"if(lt(t,{punch_start:.3f}),1,"  # Before: no punch
            f"if(lt(t,{punch_peak:.3f}),"  # Ramp up
            f"1+{punch_amount:.4f}*((t-{punch_start:.3f})/{boom_punch.ramp_in:.3f}),"
            f"if(lt(t,{punch_hold_end:.3f}),{boom_punch.factor:.4f},"  # Hold
            f"if(lt(t,{punch_end:.3f}),"  # Ramp down
            f"{boom_punch.factor:.4f}-{punch_amount:.4f}*((t-{punch_hold_end:.3f})/{boom_punch.ramp_out:.3f}),"
            f"1))))"  # After: no punch
        )
        parts.append(f"({punch_expr})")

    # Combine parts by multiplication
    if not parts:
        return "1"
    elif len(parts) == 1:
        return parts[0]
    else:
        return "*".join(parts)


def apply_zoom_effects(
    cmd: FFmpegCommand,
    boom_seconds: float,
    video_duration: float,
    width: int,
    height: int,
    slow_zoom: Optional[SlowZoomConfig] = None,
    boom_punch: Optional[BoomPunchConfig] = None,
) -> FFmpegCommand:
    """Apply combined zoom effects using scale+crop.

    Uses scale to enlarge the frame, then crop to extract the center.
    This approach works correctly for video (unlike zoompan which is for stills).

    The key fix for the "shift bug": use floor division and ensure the crop
    offset is always calculated as an integer to avoid sub-pixel drift.

    Args:
        cmd: FFmpegCommand to add filters to
        boom_seconds: Time of boom in seconds
        video_duration: Total video duration
        width: Original video width
        height: Original video height
        slow_zoom: Slow zoom configuration
        boom_punch: Boom punch configuration

    Returns:
        The modified FFmpegCommand
    """
    if not slow_zoom and not boom_punch:
        return cmd

    zoom_expr = build_combined_zoom_expr(
        boom_seconds=boom_seconds,
        video_duration=video_duration,
        slow_zoom=slow_zoom,
        boom_punch=boom_punch,
    )

    # Scale up by zoom factor
    # Use trunc to ensure even dimensions (required for most codecs)
    # The /2*2 pattern ensures even numbers
    scale_w = f"trunc(iw*({zoom_expr})/2)*2"
    scale_h = f"trunc(ih*({zoom_expr})/2)*2"

    # Crop from center - FIX: use floor() to ensure integer offsets
    # This prevents sub-pixel drift that causes the "shift" bug
    # We use trunc() which is equivalent to floor() for positive numbers
    crop_x = f"trunc((in_w-{width})/2)"
    crop_y = f"trunc((in_h-{height})/2)"

    # Add filters - quote expressions with colons to protect them from FFmpeg parser
    # FFmpeg interprets colons as option separators, so expressions like min(t,13.330)
    # need single quotes to avoid being misinterpreted
    cmd.add_filter(f"scale=w='{scale_w}':h='{scale_h}':eval=frame")
    cmd.add_filter(f"crop={width}:{height}:{crop_x}:{crop_y}")

    return cmd


def apply_shake_effect(
    cmd: FFmpegCommand,
    boom_seconds: float,
    fps: int,
    shake: ShakeConfig,
) -> FFmpegCommand:
    """Apply screen shake effect at boom moment.

    Uses the scroll filter with a pseudo-random offset based on frame number.
    The shake is applied for N frames starting at boom.

    Args:
        cmd: FFmpegCommand to add filters to
        boom_seconds: Time of boom in seconds
        fps: Video frame rate
        shake: Shake configuration

    Returns:
        The modified FFmpegCommand
    """
    # Calculate frame range for shake
    boom_frame = int(boom_seconds * fps)
    shake_end_frame = boom_frame + shake.frames

    # Build shake expression using frame number (n)
    # We use sin/cos with prime multipliers for pseudo-random but deterministic shake
    # The shake is only active for frames between boom_frame and shake_end_frame
    intensity = shake.intensity

    # X offset: pseudo-random based on frame number
    # sin(n * 17) gives a varying value between -1 and 1
    shake_x = (
        f"if(gte(n,{boom_frame})*lt(n,{shake_end_frame}),"
        f"trunc({intensity}*sin(n*17)),"
        f"0)"
    )

    # Y offset: different frequency for variety
    shake_y = (
        f"if(gte(n,{boom_frame})*lt(n,{shake_end_frame}),"
        f"trunc({intensity}*cos(n*23)),"
        f"0)"
    )

    # Use the scroll filter for translation
    # scroll: h and v are horizontal and vertical scroll speeds
    # But we want per-frame offset, so we use the pad+crop trick instead
    #
    # Alternative approach: pad the video, then crop with offset
    # This allows per-frame translation without the scroll filter's limitations

    # Pad video with extra space on all sides
    pad_size = intensity * 2
    padded_w = f"iw+{pad_size * 2}"
    padded_h = f"ih+{pad_size * 2}"

    # Crop back to original size with shake offset
    # Quote the offset expressions to protect commas inside if() from being interpreted
    # as filter parameter separators
    crop_x = f"'{pad_size}+{shake_x}'"
    crop_y = f"'{pad_size}+{shake_y}'"

    cmd.add_filter(f"pad={padded_w}:{padded_h}:{pad_size}:{pad_size}:black")
    cmd.add_filter(f"crop=iw-{pad_size * 2}:ih-{pad_size * 2}:{crop_x}:{crop_y}:exact=1")

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

    Order of application:
    1. Zoom effects (slow zoom + boom punch combined)
    2. Shake effect

    Args:
        cmd: FFmpegCommand to add filters to
        motion: Motion configuration
        boom_seconds: Time of boom in seconds
        video_duration: Total video duration
        width: Video width
        height: Video height
        fps: Video frame rate

    Returns:
        The modified FFmpegCommand
    """
    # Apply zoom effects
    apply_zoom_effects(
        cmd=cmd,
        boom_seconds=boom_seconds,
        video_duration=video_duration,
        width=width,
        height=height,
        slow_zoom=motion.slow_zoom,
        boom_punch=motion.boom_punch,
    )

    # Apply shake effect
    if motion.shake:
        apply_shake_effect(
            cmd=cmd,
            boom_seconds=boom_seconds,
            fps=fps,
            shake=motion.shake,
        )

    return cmd


# Legacy compatibility - keep the old function name working
def add_zoom_effect(
    cmd: FFmpegCommand,
    boom_seconds: float,
    zoom_factor: float = 1.3,
    ramp_duration: float = 0.5,
    hold_duration: float = 0.5,
    width: int = 1080,
    height: int = 1080,
) -> FFmpegCommand:
    """Legacy function - wraps apply_zoom_effects with boom punch only.

    DEPRECATED: Use apply_zoom_effects or apply_motion_effects instead.
    """
    return apply_zoom_effects(
        cmd=cmd,
        boom_seconds=boom_seconds,
        video_duration=boom_seconds + hold_duration + ramp_duration + 10,  # Estimate
        width=width,
        height=height,
        boom_punch=BoomPunchConfig(
            factor=zoom_factor,
            ramp_in=ramp_duration,
            hold=hold_duration,
            ramp_out=ramp_duration,
        ),
    )
