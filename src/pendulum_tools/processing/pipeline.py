"""Main processing pipeline for video post-processing."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from ..models import VideoMetadata
from .ffmpeg import FFmpegCommand, get_video_dimensions
from .motion import apply_motion_effects, build_motion_filters
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
    preset: str = "medium"  # Encoding preset (slower = better compression)
    use_nvenc: bool = True  # Use NVIDIA hardware encoding (much faster)
    nvenc_cq: int = 23  # NVENC quality (0-51, lower = better)


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
            video_dir: Directory containing video.mp4 or video_raw.mp4 and metadata.json
            config: Processing configuration (uses defaults if not provided)
        """
        self.video_dir = Path(video_dir)
        self.config = config or ProcessingConfig()

        # Load metadata
        metadata_path = self.video_dir / "metadata.json"
        if not metadata_path.exists():
            raise FileNotFoundError(f"metadata.json not found in {video_dir}")

        self.metadata = VideoMetadata.from_file(metadata_path)

        # Locate input video: prefer video.mp4, fallback to video_raw.mp4
        self.input_video = self.video_dir / "video.mp4"
        if not self.input_video.exists():
            self.input_video = self.video_dir / "video_raw.mp4"
        if not self.input_video.exists():
            raise FileNotFoundError(f"No video file found in {video_dir}")

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
            output_dir: Output directory for auxiliary files (default: video_dir)
            dry_run: If True, print FFmpeg command without executing
            force: If True, overwrite existing processed output

        Returns:
            ProcessingResult with success status and output paths
        """
        # Output directory for auxiliary files (captions.ass, thumbnails)
        output_dir = output_dir or self.video_dir

        if not dry_run:
            output_dir.mkdir(parents=True, exist_ok=True)

        # Output video is always video_processed.mp4 in the video directory
        output_video = self.video_dir / "video_processed.mp4"

        if output_video.exists() and not force and not dry_run:
            return ProcessingResult(
                success=False,
                output_dir=output_dir,
                error=f"{output_video.name} already exists. Use --force to overwrite.",
            )

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

        # Calculate foreground size for Shorts (scale input to fit output width)
        # This ensures motion effects work at the correct visual scale
        if self.config.shorts:
            # Scale to fit output width, maintaining input aspect ratio
            input_aspect = width / height
            fg_width = out_width
            fg_height = int(fg_width / input_aspect)
        else:
            fg_width, fg_height = width, height

        # Build motion filters if template has motion effects
        # Motion filters use FOREGROUND dimensions (after scaling), not input dimensions
        motion_filters: list[str] | None = None
        if template.motion and boom_seconds:
            motion_filters = build_motion_filters(
                motion=template.motion,
                boom_seconds=boom_seconds,
                video_duration=video_duration,
                width=fg_width,
                height=fg_height,
                fps=fps,
            )

        # Apply background/padding for Shorts, or motion effects for regular video
        if self.config.shorts:
            if self.config.blurred_background:
                # For blurred background, motion effects are applied to foreground
                # inside the filter_complex graph (AFTER scaling to foreground size)
                cmd.set_blurred_background(
                    blur_strength=self.config.blur_strength,
                    brightness=self.config.background_brightness,
                    target_width=out_width,
                    target_height=out_height,
                    foreground_width=fg_width,
                    foreground_height=fg_height,
                    foreground_filters=motion_filters,
                )
            else:
                # Scale to foreground size first, apply motion effects, then pad
                cmd.add_filter(f"scale={fg_width}:{fg_height}")
                if motion_filters:
                    for f in motion_filters:
                        cmd.add_filter(f)
                cmd.add_shorts_padding()
        else:
            # Regular video: just apply motion effects
            if motion_filters:
                apply_motion_effects(
                    cmd=cmd,
                    motion=template.motion,
                    boom_seconds=boom_seconds,
                    video_duration=video_duration,
                    width=width,
                    height=height,
                    fps=fps,
                )

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

        # Set output quality and encoding
        cmd.set_pixel_format("yuv420p")
        if self.config.use_nvenc:
            cmd.use_nvenc(cq=self.config.nvenc_cq)
        else:
            cmd.set_codec("libx264")
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
                "use_nvenc": self.config.use_nvenc,
            },
        }
