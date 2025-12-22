"""FFmpeg command builder for video processing."""

from __future__ import annotations

import shlex
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class FFmpegCommand:
    """Builder for FFmpeg commands with filter graph support."""

    input_path: Path
    output_path: Path
    video_filters: list[str] = field(default_factory=list)
    options: dict[str, str] = field(default_factory=dict)

    def add_filter(self, filter_str: str) -> "FFmpegCommand":
        """Add a raw video filter string."""
        self.video_filters.append(filter_str)
        return self

    def add_drawtext(
        self,
        text: str,
        x: str = "(w-text_w)/2",
        y: str = "h*0.85",
        fontsize: int = 48,
        fontcolor: str = "white",
        fontfile: Optional[Path] = None,
        enable: Optional[str] = None,
        box: bool = True,
        boxcolor: str = "black@0.5",
        boxborderw: int = 10,
    ) -> "FFmpegCommand":
        """Add a drawtext filter with proper escaping.

        Args:
            text: Text to display (will be escaped)
            x: X position expression (default: centered)
            y: Y position expression (default: 85% from top)
            fontsize: Font size in pixels
            fontcolor: Font color (name or hex)
            fontfile: Path to font file (optional)
            enable: Enable expression (e.g., "between(t,0,5)")
            box: Draw background box behind text
            boxcolor: Background box color with alpha
            boxborderw: Box border/padding width
        """
        parts = [
            f"text='{self._escape_text(text)}'",
            f"fontsize={fontsize}",
            f"fontcolor={fontcolor}",
            f"x={x}",
            f"y={y}",
        ]

        if fontfile:
            parts.append(f"fontfile='{fontfile}'")

        if enable:
            parts.append(f"enable='{enable}'")

        if box:
            parts.append(f"box=1")
            parts.append(f"boxcolor={boxcolor}")
            parts.append(f"boxborderw={boxborderw}")

        self.video_filters.append(f"drawtext={':'.join(parts)}")
        return self

    def add_scale(self, width: int, height: int) -> "FFmpegCommand":
        """Scale video to specific dimensions."""
        self.video_filters.append(f"scale={width}:{height}")
        return self

    def add_pad(
        self,
        width: int,
        height: int,
        x: str = "(ow-iw)/2",
        y: str = "(oh-ih)/2",
        color: str = "black",
    ) -> "FFmpegCommand":
        """Pad video to target dimensions.

        Args:
            width: Target width
            height: Target height
            x: X offset expression for input placement
            y: Y offset expression for input placement
            color: Padding color
        """
        self.video_filters.append(f"pad={width}:{height}:{x}:{y}:{color}")
        return self

    def add_shorts_padding(self, bg_color: str = "black") -> "FFmpegCommand":
        """Pad 1:1 video (1080x1080) to 9:16 (1080x1920) for YouTube Shorts."""
        return self.add_pad(1080, 1920, "(ow-iw)/2", "(oh-ih)/2", bg_color)

    def add_zoompan(
        self,
        zoom_expr: str,
        x_expr: str = "iw/2-(iw/zoom/2)",
        y_expr: str = "ih/2-(ih/zoom/2)",
        fps: int = 60,
        duration: int = 1,
        size: str = "1080x1080",
    ) -> "FFmpegCommand":
        """Add zoompan filter for zoom/pan effects.

        Args:
            zoom_expr: Zoom level expression (1.0 = no zoom, 2.0 = 2x zoom)
            x_expr: X pan expression (default: center)
            y_expr: Y pan expression (default: center)
            fps: Output frame rate
            duration: Duration multiplier (frames per input frame)
            size: Output size
        """
        self.video_filters.append(
            f"zoompan=z='{zoom_expr}':x='{x_expr}':y='{y_expr}'"
            f":fps={fps}:d={duration}:s={size}"
        )
        return self

    def set_quality(self, crf: int = 18, preset: str = "slow") -> "FFmpegCommand":
        """Set H.264 encoding quality.

        Args:
            crf: Constant Rate Factor (0-51, lower = better, 18 = visually lossless)
            preset: Encoding preset (slower = better compression)
        """
        self.options["crf"] = str(crf)
        self.options["preset"] = preset
        return self

    def set_codec(self, codec: str = "libx264") -> "FFmpegCommand":
        """Set video codec."""
        self.options["c:v"] = codec
        return self

    def set_pixel_format(self, pix_fmt: str = "yuv420p") -> "FFmpegCommand":
        """Set pixel format for compatibility."""
        self.options["pix_fmt"] = pix_fmt
        return self

    def copy_audio(self) -> "FFmpegCommand":
        """Copy audio stream without re-encoding."""
        self.options["c:a"] = "copy"
        return self

    def no_audio(self) -> "FFmpegCommand":
        """Remove audio from output."""
        self.options["an"] = ""
        return self

    def build(self) -> list[str]:
        """Build the complete FFmpeg command as argument list."""
        cmd = ["ffmpeg", "-y", "-i", str(self.input_path)]

        if self.video_filters:
            cmd.extend(["-vf", ",".join(self.video_filters)])

        for key, value in self.options.items():
            if value:
                cmd.extend([f"-{key}", value])
            else:
                cmd.append(f"-{key}")

        cmd.append(str(self.output_path))
        return cmd

    def build_string(self) -> str:
        """Build the command as a shell-escaped string."""
        return shlex.join(self.build())

    def run(self, dry_run: bool = False, capture: bool = True) -> Optional[subprocess.CompletedProcess]:
        """Execute the FFmpeg command.

        Args:
            dry_run: If True, print command without executing
            capture: If True, capture stdout/stderr

        Returns:
            CompletedProcess if executed, None if dry_run
        """
        cmd = self.build()

        if dry_run:
            print(self.build_string())
            return None

        return subprocess.run(
            cmd,
            check=True,
            capture_output=capture,
            text=True,
        )

    @staticmethod
    def _escape_text(text: str) -> str:
        """Escape special characters for FFmpeg drawtext filter."""
        # FFmpeg drawtext escape sequence
        text = text.replace("\\", "\\\\")
        text = text.replace("'", "'\\''")
        text = text.replace(":", "\\:")
        text = text.replace("%", "\\%")
        return text


def extract_frame(
    video_path: Path,
    output_path: Path,
    timestamp: float,
    quality: int = 2,
) -> subprocess.CompletedProcess:
    """Extract a single frame from video at given timestamp.

    Args:
        video_path: Path to input video
        output_path: Path for output image (jpg/png)
        timestamp: Time in seconds
        quality: JPEG quality (1-31, lower = better)

    Returns:
        CompletedProcess from FFmpeg execution
    """
    cmd = [
        "ffmpeg",
        "-y",
        "-ss", str(timestamp),
        "-i", str(video_path),
        "-vframes", "1",
        "-q:v", str(quality),
        str(output_path),
    ]

    return subprocess.run(cmd, check=True, capture_output=True, text=True)


def get_video_duration(video_path: Path) -> float:
    """Get video duration in seconds using ffprobe."""
    cmd = [
        "ffprobe",
        "-v", "error",
        "-show_entries", "format=duration",
        "-of", "default=noprint_wrappers=1:nokey=1",
        str(video_path),
    ]

    result = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return float(result.stdout.strip())


def get_video_dimensions(video_path: Path) -> tuple[int, int]:
    """Get video width and height using ffprobe."""
    cmd = [
        "ffprobe",
        "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=width,height",
        "-of", "csv=s=x:p=0",
        str(video_path),
    ]

    result = subprocess.run(cmd, check=True, capture_output=True, text=True)
    width, height = result.stdout.strip().split("x")
    return int(width), int(height)
