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
    # For filter_complex (more advanced graphs)
    filter_complex: Optional[str] = None
    filter_complex_output: str = "[v]"  # Output label for filter_complex

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

    def add_subtitles(self, ass_path: Path, force_style: Optional[str] = None) -> "FFmpegCommand":
        """Add ASS subtitles using libass.

        Args:
            ass_path: Path to .ass subtitle file
            force_style: Optional style overrides (e.g., "FontSize=80,OutlineColour=&H40000000")
        """
        # Escape path for FFmpeg filter
        escaped_path = str(ass_path).replace("\\", "\\\\").replace(":", "\\:").replace("'", "\\'")

        if force_style:
            self.video_filters.append(f"subtitles='{escaped_path}':force_style='{force_style}'")
        else:
            self.video_filters.append(f"subtitles='{escaped_path}'")
        return self

    def set_blurred_background(
        self,
        blur_strength: int = 50,
        brightness: float = 0.3,
        target_width: int = 1080,
        target_height: int = 1920,
    ) -> "FFmpegCommand":
        """Use blurred/scaled version of video as background for vertical format.

        Creates a filter_complex that:
        1. Scales + blurs video to fill vertical frame (background)
        2. Overlays original video centered on top

        Args:
            blur_strength: Gaussian blur sigma (higher = more blur)
            brightness: Background brightness multiplier (0.3 = 30%)
            target_width: Output width
            target_height: Output height
        """
        # filter_complex graph:
        # [0:v] -> split into [bg] and [fg]
        # [bg] -> scale to fill, blur, darken
        # [fg] -> scale to fit centered
        # [bg][fg] -> overlay

        # Calculate scale to fill (cover) the target
        # For 1080x1080 -> 1080x1920, we scale up to 1920x1920, then crop to 1080x1920
        self.filter_complex = (
            f"[0:v]split[bg][fg];"
            # Background: scale to cover, blur, darken
            f"[bg]scale={target_height}:{target_height}:force_original_aspect_ratio=increase,"
            f"crop={target_width}:{target_height},"
            f"boxblur={blur_strength}:{blur_strength},"
            f"eq=brightness={brightness - 1.0}[bg_out];"
            # Foreground: keep original size, center it
            f"[fg]scale={target_width}:-1:force_original_aspect_ratio=decrease[fg_scaled];"
            # Overlay foreground on background
            f"[bg_out][fg_scaled]overlay=(W-w)/2:(H-h)/2[v]"
        )
        self.filter_complex_output = "[v]"
        return self

    def set_movflags(self, flags: str = "faststart") -> "FFmpegCommand":
        """Set movflags for MP4 output.

        Args:
            flags: Movflags value (e.g., "faststart" for web optimization)
        """
        self.options["movflags"] = f"+{flags}"
        return self

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

        # Use filter_complex if set, otherwise use simple -vf chain
        if self.filter_complex:
            # Append any additional video_filters to the filter_complex output
            if self.video_filters:
                # Chain additional filters after the filter_complex output
                full_filter = (
                    f"{self.filter_complex};"
                    f"{self.filter_complex_output}{','.join(self.video_filters)}[vout]"
                )
                cmd.extend(["-filter_complex", full_filter])
                cmd.extend(["-map", "[vout]"])
            else:
                cmd.extend(["-filter_complex", self.filter_complex])
                cmd.extend(["-map", self.filter_complex_output])
            # Map audio if present
            cmd.extend(["-map", "0:a?"])
        elif self.video_filters:
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
