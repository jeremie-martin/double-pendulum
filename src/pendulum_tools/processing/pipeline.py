"""Main processing pipeline for video post-processing."""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from ..models import VideoMetadata
from ..constants import (
    DEFAULT_BLUR_STRENGTH,
    DEFAULT_BACKGROUND_BRIGHTNESS,
    DEFAULT_CRF_QUALITY,
    DEFAULT_NVENC_CQ,
    FALLBACK_BOOM_SECONDS,
)
from ..exceptions import FFmpegError
from .ffmpeg import FFmpegCommand, get_video_dimensions, is_nvenc_available
from .motion import apply_motion_effects, build_motion_filters, MotionConfig, SlowZoomConfig
from .subtitles_ass import generate_ass_from_resolved, CaptionPreset
from .templates import (
    TemplateLibrary,
    TextPoolLibrary,
    resolve_template,
    load_template_system,
)
from .thumbnails import extract_thumbnails

logger = logging.getLogger(__name__)


@dataclass
class ProcessingConfig:
    """Configuration for video processing pipeline."""

    # Template selection
    template: str = "minimal_science"  # Template name from templates.toml
    seed: Optional[int] = None  # Random seed for reproducible text selection

    # Format settings
    shorts: bool = False  # Pad to 9:16 for YouTube Shorts
    blurred_background: bool = False  # Use blurred video as background
    blur_strength: int = DEFAULT_BLUR_STRENGTH  # Blur sigma for background
    background_brightness: float = DEFAULT_BACKGROUND_BRIGHTNESS  # (0-1)

    # Thumbnail settings
    extract_thumbnails: bool = True

    # Quality settings
    crf_quality: int = DEFAULT_CRF_QUALITY  # Lower = better (17 = high quality)
    preset: str = "medium"  # Encoding preset (slower = better compression)
    use_nvenc: bool = True  # Use NVIDIA hardware encoding (much faster)
    nvenc_cq: int = DEFAULT_NVENC_CQ  # NVENC quality (0-51, 19 = high quality)

    # Motion overrides (None = use template defaults)
    slow_zoom_start: Optional[float] = None  # Override slow zoom start scale
    slow_zoom_end: Optional[float] = None  # Override slow zoom end scale


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
    # Processing parameters used
    zoom_start: Optional[float] = None
    zoom_end: Optional[float] = None
    blur_strength: Optional[int] = None
    background_brightness: Optional[float] = None

    def save_to_metadata(self, metadata_path: Path) -> None:
        """Save processing parameters to metadata.json."""
        import json
        with open(metadata_path) as f:
            data = json.load(f)

        data["processing"] = {
            "template": self.template_used,
            "zoom_start": self.zoom_start,
            "zoom_end": self.zoom_end,
            "blur_strength": self.blur_strength,
            "background_brightness": self.background_brightness,
        }

        with open(metadata_path, "w") as f:
            json.dump(data, f, indent=2)
            f.write("\n")


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

        # Get boom time with fallback warning
        boom_seconds = self.metadata.boom_seconds
        using_fallback_boom = False
        if boom_seconds is None:
            boom_seconds = FALLBACK_BOOM_SECONDS
            using_fallback_boom = True
            logger.warning(
                f"No boom detected in metadata, using fallback time of {FALLBACK_BOOM_SECONDS}s. "
                "Motion effects may not sync correctly."
            )

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

        # Build motion config with optional overrides
        motion_config = template.motion
        if self.config.slow_zoom_start is not None or self.config.slow_zoom_end is not None:
            # Apply slow zoom overrides
            slow_zoom = SlowZoomConfig(
                start=self.config.slow_zoom_start if self.config.slow_zoom_start is not None
                      else (motion_config.slow_zoom.start if motion_config and motion_config.slow_zoom else 1.0),
                end=self.config.slow_zoom_end if self.config.slow_zoom_end is not None
                    else (motion_config.slow_zoom.end if motion_config and motion_config.slow_zoom else 1.1),
            )
            motion_config = MotionConfig(
                slow_zoom=slow_zoom,
                boom_punch=motion_config.boom_punch if motion_config else None,
                shake=motion_config.shake if motion_config else None,
            )

        # Track final zoom values for result
        final_zoom_start = motion_config.slow_zoom.start if motion_config and motion_config.slow_zoom else None
        final_zoom_end = motion_config.slow_zoom.end if motion_config and motion_config.slow_zoom else None

        # Build motion filters if template has motion effects
        # Motion filters use FOREGROUND dimensions (after scaling), not input dimensions
        motion_filters: list[str] | None = None
        if motion_config and boom_seconds:
            motion_filters = build_motion_filters(
                motion=motion_config,
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
            if motion_config:
                apply_motion_effects(
                    cmd=cmd,
                    motion=motion_config,
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
        # Automatically fall back to libx264 if NVENC is requested but unavailable
        use_nvenc = self.config.use_nvenc and is_nvenc_available()
        if self.config.use_nvenc and not use_nvenc:
            logger.info("NVENC not available, falling back to libx264")

        cmd.set_pixel_format("yuv420p")
        if use_nvenc:
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
        except FFmpegError as e:
            # Detailed FFmpeg error with stderr
            return ProcessingResult(
                success=False,
                output_dir=output_dir,
                error=str(e),
                ffmpeg_command=ffmpeg_command,
                template_used=template_name,
            )
        except Exception as e:
            return ProcessingResult(
                success=False,
                output_dir=output_dir,
                error=f"Unexpected error: {e}",
                ffmpeg_command=ffmpeg_command,
                template_used=template_name,
            )

        # Extract thumbnails (using VIDEO time, not simulation time)
        thumbnails: list[Path] = []
        if self.config.extract_thumbnails:
            # Calculate video boom time from boom_frame / fps
            video_boom_seconds = None
            if (self.metadata.results and self.metadata.results.boom_frame
                    and self.metadata.config.video_fps):
                video_boom_seconds = (
                    self.metadata.results.boom_frame / self.metadata.config.video_fps
                )

            try:
                thumbnails = extract_thumbnails(
                    self.input_video,
                    output_dir,
                    video_boom_seconds=video_boom_seconds,
                    video_duration=self.metadata.video_duration,
                )
            except Exception as e:
                logger.warning(f"Thumbnail extraction failed: {e}")

        return ProcessingResult(
            success=True,
            output_dir=output_dir,
            video_path=output_video,
            thumbnails=thumbnails,
            ffmpeg_command=ffmpeg_command,
            template_used=template_name,
            captions_text=captions_text,
            zoom_start=final_zoom_start,
            zoom_end=final_zoom_end,
            blur_strength=self.config.blur_strength if self.config.blurred_background else None,
            background_brightness=self.config.background_brightness if self.config.blurred_background else None,
        )

    def preview(self) -> dict:
        """Preview what processing would do without executing."""
        template = self.template_lib.get(self.config.template)

        boom_seconds = self.metadata.boom_seconds
        if boom_seconds is None:
            boom_seconds = FALLBACK_BOOM_SECONDS

        resolved_captions = resolve_template(
            template=template,
            text_pools=self.text_pools,
            boom_seconds=boom_seconds,
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
