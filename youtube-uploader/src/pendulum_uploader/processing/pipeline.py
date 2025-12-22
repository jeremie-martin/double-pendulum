"""Main processing pipeline for video post-processing."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from ..models import VideoMetadata
from .effects import add_zoom_effect
from .ffmpeg import FFmpegCommand, get_video_dimensions
from .overlays import apply_captions
from .thumbnails import extract_thumbnails


def get_default_font() -> Optional[Path]:
    """Get the path to the bundled default font.

    Returns:
        Path to Inter-Bold.ttf if it exists, None otherwise
    """
    # Look for bundled font relative to this module
    module_dir = Path(__file__).parent.parent.parent.parent
    font_path = module_dir / "assets" / "fonts" / "Inter-Bold.ttf"

    if font_path.exists():
        return font_path

    # Also check alternate location
    alt_path = Path(__file__).parent.parent.parent / "assets" / "fonts" / "Inter-Bold.ttf"
    if alt_path.exists():
        return alt_path

    return None


@dataclass
class ProcessingConfig:
    """Configuration for video processing pipeline."""

    # Caption/overlay settings
    style: str = "wait_for_it"

    # Format settings
    shorts: bool = False  # Pad to 9:16 for YouTube Shorts

    # Effect settings
    zoom: bool = False
    zoom_factor: float = 1.3

    # Thumbnail settings
    extract_thumbnails: bool = True

    # Quality settings
    crf_quality: int = 18  # Lower = better (18 = visually lossless)
    preset: str = "slow"  # Slower = better compression

    # Font settings
    font_path: Optional[Path] = None
    fontsize: int = 48


@dataclass
class ProcessingResult:
    """Result from processing pipeline."""

    success: bool
    output_dir: Path
    video_path: Optional[Path] = None
    thumbnails: list[Path] = field(default_factory=list)
    error: Optional[str] = None
    ffmpeg_command: Optional[str] = None  # For dry-run/debugging


class ProcessingPipeline:
    """Orchestrates the full video processing workflow."""

    def __init__(self, video_dir: Path, config: Optional[ProcessingConfig] = None):
        """Initialize the processing pipeline.

        Args:
            video_dir: Directory containing video.mp4 and metadata.json
            config: Processing configuration (uses defaults if not provided)
        """
        self.video_dir = Path(video_dir)
        self.config = config or ProcessingConfig()

        # Load metadata
        metadata_path = self.video_dir / "metadata.json"
        if not metadata_path.exists():
            raise FileNotFoundError(f"metadata.json not found in {video_dir}")

        self.metadata = VideoMetadata.from_file(metadata_path)

        # Locate input video
        self.input_video = self.video_dir / "video.mp4"
        if not self.input_video.exists():
            raise FileNotFoundError(f"video.mp4 not found in {video_dir}")

        # Resolve font path
        if self.config.font_path is None:
            self.config.font_path = get_default_font()

    def run(
        self,
        output_dir: Optional[Path] = None,
        dry_run: bool = False,
        force: bool = False,
    ) -> ProcessingResult:
        """Execute the full processing pipeline.

        Args:
            output_dir: Output directory (default: video_dir/processed/)
            dry_run: If True, print FFmpeg command without executing
            force: If True, overwrite existing processed output

        Returns:
            ProcessingResult with success status and output paths
        """
        # Set up output directory
        output_dir = output_dir or (self.video_dir / "processed")

        # Check for existing output (skip check for dry-run)
        if output_dir.exists() and not force and not dry_run:
            return ProcessingResult(
                success=False,
                output_dir=output_dir,
                error="Output directory already exists. Use --force to overwrite.",
            )

        if not dry_run:
            output_dir.mkdir(parents=True, exist_ok=True)

        output_video = output_dir / "video.mp4"

        # Build the FFmpeg command
        cmd = FFmpegCommand(self.input_video, output_video)

        # Get video dimensions
        try:
            width, height = get_video_dimensions(self.input_video)
        except Exception:
            width, height = self.metadata.config.width, self.metadata.config.height

        # Apply zoom effect (must come before text overlays if using zoompan)
        # Note: zoompan changes the frame timing, so we apply it first
        if self.config.zoom and self.metadata.boom_seconds is not None:
            add_zoom_effect(
                cmd,
                boom_seconds=self.metadata.boom_seconds,
                zoom_factor=self.config.zoom_factor,
                output_size=f"{width}x{height}",
                fps=self.metadata.config.video_fps,
            )

        # Apply text overlays
        if self.config.style != "minimal":
            apply_captions(
                cmd,
                style_name=self.config.style,
                boom_seconds=self.metadata.boom_seconds,
                video_duration=self.metadata.video_duration,
                pendulum_count=self.metadata.config.pendulum_count,
                shorts_mode=self.config.shorts,
                fontfile=self.config.font_path,
                fontsize=self.config.fontsize,
            )

        # Apply Shorts padding (must be last, after all other effects)
        if self.config.shorts:
            cmd.add_shorts_padding()

        # Set output quality
        cmd.set_codec("libx264")
        cmd.set_pixel_format("yuv420p")
        cmd.set_quality(crf=self.config.crf_quality, preset=self.config.preset)
        cmd.copy_audio()  # Preserve audio if present

        # Store command for dry run
        ffmpeg_command = cmd.build_string()

        if dry_run:
            return ProcessingResult(
                success=True,
                output_dir=output_dir,
                ffmpeg_command=ffmpeg_command,
            )

        # Execute FFmpeg
        try:
            cmd.run()
        except Exception as e:
            return ProcessingResult(
                success=False,
                output_dir=output_dir,
                error=f"FFmpeg failed: {e}",
                ffmpeg_command=ffmpeg_command,
            )

        # Extract thumbnails
        thumbnails: list[Path] = []
        if self.config.extract_thumbnails:
            try:
                thumbnails = extract_thumbnails(
                    self.input_video,  # Use original for best quality
                    output_dir,
                    boom_seconds=self.metadata.boom_seconds,
                    best_frame_seconds=self.metadata.best_frame_seconds,
                    video_duration=self.metadata.video_duration,
                )
            except Exception as e:
                # Don't fail pipeline on thumbnail errors
                print(f"Warning: Thumbnail extraction failed: {e}")

        return ProcessingResult(
            success=True,
            output_dir=output_dir,
            video_path=output_video,
            thumbnails=thumbnails,
            ffmpeg_command=ffmpeg_command,
        )

    def preview(self) -> dict:
        """Preview what processing would do without executing.

        Returns:
            Dict with processing details
        """
        return {
            "input_video": str(self.input_video),
            "boom_seconds": self.metadata.boom_seconds,
            "video_duration": self.metadata.video_duration,
            "pendulum_count": self.metadata.config.pendulum_count,
            "config": {
                "style": self.config.style,
                "shorts": self.config.shorts,
                "zoom": self.config.zoom,
                "zoom_factor": self.config.zoom_factor,
                "extract_thumbnails": self.config.extract_thumbnails,
                "crf_quality": self.config.crf_quality,
                "font_path": str(self.config.font_path) if self.config.font_path else None,
            },
            "score": {
                "peak_causticness": self.metadata.score.peak_causticness if self.metadata.score else None,
                "best_frame": self.metadata.score.best_frame if self.metadata.score else None,
            },
        }
