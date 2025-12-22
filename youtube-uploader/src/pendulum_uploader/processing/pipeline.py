"""Main processing pipeline for video post-processing."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from ..models import VideoMetadata
from .ffmpeg import FFmpegCommand, get_video_dimensions
from .motion import apply_motion_effects
from .subtitles_ass import generate_ass_from_resolved, CaptionPreset
from .templates import (
    TemplateLibrary,
    TextPoolLibrary,
    resolve_template,
    load_template_system,
)
from .thumbnails import extract_thumbnails


@dataclass
class ProcessingConfig:
    """Configuration for video processing pipeline."""

    # Template selection
    template: str = "viral_science"  # Template name from templates.toml
    seed: Optional[int] = None  # Random seed for reproducible text selection

    # Format settings
    shorts: bool = False  # Pad to 9:16 for YouTube Shorts
    blurred_background: bool = False  # Use blurred video as background
    blur_strength: int = 50  # Blur sigma for background
    background_brightness: float = 0.3  # Background brightness (0-1)

    # Thumbnail settings
    extract_thumbnails: bool = True

    # Quality settings
    crf_quality: int = 18  # Lower = better (18 = visually lossless)
    preset: str = "slow"  # Slower = better compression


@dataclass
class ProcessingResult:
    """Result from processing pipeline."""

    success: bool
    output_dir: Path
    video_path: Optional[Path] = None
    thumbnails: list[Path] = field(default_factory=list)
    error: Optional[str] = None
    ffmpeg_command: Optional[str] = None
    template_used: Optional[str] = None
    captions_text: Optional[list[str]] = None  # For preview


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

        # Load template system
        self.template_lib, self.text_pools = load_template_system()

    def run(
        self,
        output_dir: Optional[Path] = None,
        dry_run: bool = False,
        force: bool = False,
    ) -> ProcessingResult:
        """Execute the full processing pipeline.

        Pipeline order:
        1. Motion effects (slow zoom + boom punch + shake)
        2. Background/padding for Shorts
        3. ASS subtitles from resolved template

        Args:
            output_dir: Output directory (default: video_dir/processed/)
            dry_run: If True, print FFmpeg command without executing
            force: If True, overwrite existing processed output

        Returns:
            ProcessingResult with success status and output paths
        """
        # Set up output directory
        output_dir = output_dir or (self.video_dir / "processed")

        if output_dir.exists() and not force and not dry_run:
            return ProcessingResult(
                success=False,
                output_dir=output_dir,
                error="Output directory already exists. Use --force to overwrite.",
            )

        if not dry_run:
            output_dir.mkdir(parents=True, exist_ok=True)

        output_video = output_dir / "video.mp4"

        # Load template
        template_name = self.config.template
        if template_name == "random":
            template = self.template_lib.pick_random(seed=self.config.seed)
            template_name = template.name
        else:
            template = self.template_lib.get(template_name)

        # Build the FFmpeg command
        cmd = FFmpegCommand(self.input_video, output_video)

        # Get video dimensions and metadata
        try:
            width, height = get_video_dimensions(self.input_video)
        except Exception:
            width, height = self.metadata.config.width, self.metadata.config.height

        boom_seconds = self.metadata.boom_seconds or 10.0
        video_duration = self.metadata.video_duration
        fps = self.metadata.config.video_fps

        # Determine output dimensions
        if self.config.shorts:
            out_width, out_height = 1080, 1920
        else:
            out_width, out_height = width, height

        # Step 1: Apply motion effects from template
        if template.motion and boom_seconds:
            apply_motion_effects(
                cmd=cmd,
                motion=template.motion,
                boom_seconds=boom_seconds,
                video_duration=video_duration,
                width=width,
                height=height,
                fps=fps,
            )

        # Step 2: Apply background/padding for Shorts
        if self.config.shorts:
            if self.config.blurred_background:
                cmd.set_blurred_background(
                    blur_strength=self.config.blur_strength,
                    brightness=self.config.background_brightness,
                    target_width=out_width,
                    target_height=out_height,
                )
            else:
                cmd.add_shorts_padding()

        # Step 3: Resolve template and generate ASS subtitles
        resolved_captions = resolve_template(
            template=template,
            text_pools=self.text_pools,
            boom_seconds=boom_seconds,
            video_duration=video_duration,
            pendulum_count=self.metadata.config.pendulum_count,
            seed=self.config.seed,
        )

        captions_text = [cap.text for cap in resolved_captions]

        ass_path: Optional[Path] = None
        if resolved_captions:
            ass_path = output_dir / "captions.ass"
            if not dry_run:
                generate_ass_from_resolved(
                    output_path=ass_path,
                    captions=resolved_captions,
                    video_width=out_width,
                    video_height=out_height,
                )
            if ass_path and (dry_run or ass_path.exists()):
                cmd.add_subtitles(ass_path)

        # Set output quality
        cmd.set_codec("libx264")
        cmd.set_pixel_format("yuv420p")
        cmd.set_quality(crf=self.config.crf_quality, preset=self.config.preset)
        cmd.set_movflags("faststart")
        cmd.copy_audio()

        ffmpeg_command = cmd.build_string()

        if dry_run:
            return ProcessingResult(
                success=True,
                output_dir=output_dir,
                ffmpeg_command=ffmpeg_command,
                template_used=template_name,
                captions_text=captions_text,
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
                template_used=template_name,
            )

        # Extract thumbnails
        thumbnails: list[Path] = []
        if self.config.extract_thumbnails:
            try:
                thumbnails = extract_thumbnails(
                    self.input_video,
                    output_dir,
                    boom_seconds=self.metadata.boom_seconds,
                    best_frame_seconds=self.metadata.best_frame_seconds,
                    video_duration=self.metadata.video_duration,
                )
            except Exception as e:
                print(f"Warning: Thumbnail extraction failed: {e}")

        return ProcessingResult(
            success=True,
            output_dir=output_dir,
            video_path=output_video,
            thumbnails=thumbnails,
            ffmpeg_command=ffmpeg_command,
            template_used=template_name,
            captions_text=captions_text,
        )

    def preview(self) -> dict:
        """Preview what processing would do without executing."""
        template = self.template_lib.get(self.config.template)

        resolved_captions = resolve_template(
            template=template,
            text_pools=self.text_pools,
            boom_seconds=self.metadata.boom_seconds or 10.0,
            video_duration=self.metadata.video_duration,
            pendulum_count=self.metadata.config.pendulum_count,
            seed=self.config.seed,
        )

        return {
            "input_video": str(self.input_video),
            "boom_seconds": self.metadata.boom_seconds,
            "video_duration": self.metadata.video_duration,
            "pendulum_count": self.metadata.config.pendulum_count,
            "template": self.config.template,
            "template_description": template.description,
            "motion": {
                "slow_zoom": template.motion.slow_zoom is not None,
                "boom_punch": template.motion.boom_punch is not None,
                "shake": template.motion.shake is not None,
            } if template.motion else None,
            "captions": [
                {"text": cap.text, "start_ms": cap.start_ms, "end_ms": cap.end_ms}
                for cap in resolved_captions
            ],
            "config": {
                "shorts": self.config.shorts,
                "blurred_background": self.config.blurred_background,
                "crf_quality": self.config.crf_quality,
            },
        }
