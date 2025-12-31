"""ASS subtitle generation for modern caption styling.

Uses pysubs2 to generate .ass files with proper outline, shadow, and animations,
which are then burned into video using FFmpeg's subtitles filter (libass).
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import TYPE_CHECKING

import pysubs2
from pysubs2 import SSAFile, SSAStyle, SSAEvent

if TYPE_CHECKING:
    from .templates import ResolvedCaption


class CaptionPreset(Enum):
    """Battle-tested caption style presets."""

    CAPCUT_BOLD = "capcut_bold"
    KARAOKE = "karaoke"
    MINIMAL_SCIENCE = "minimal_science"
    MRBEAST = "mrbeast"


@dataclass
class SafeZone:
    """Safe zone configuration for different aspect ratios.

    YouTube Shorts UI covers parts of the screen. These define safe areas.
    """

    # Margins from edges (in pixels for 1080 width)
    margin_left: int = 60
    margin_right: int = 60
    margin_top: int = 180  # Account for status bar, back button
    margin_bottom: int = 350  # Account for comments, like button, description

    # Recommended text positions (as fraction of height, from top)
    hook_y: float = 0.15  # Top hook text
    main_caption_y: float = 0.75  # Main caption area (above bottom UI)

    @classmethod
    def for_shorts(cls) -> "SafeZone":
        """Safe zone for 1080x1920 YouTube Shorts."""
        return cls(
            margin_left=60,
            margin_right=60,
            margin_top=180,
            margin_bottom=350,
            hook_y=0.12,
            main_caption_y=0.72,
        )

    @classmethod
    def for_square(cls) -> "SafeZone":
        """Safe zone for 1080x1080 square video."""
        return cls(
            margin_left=40,
            margin_right=40,
            margin_top=60,
            margin_bottom=60,
            hook_y=0.08,
            main_caption_y=0.88,
        )


@dataclass
class ASSStyleConfig:
    """Configuration for an ASS subtitle style."""

    fontname: str = "Inter Bold"
    fontsize: int = 72
    primary_color: str = "&H00FFFFFF"  # White (AABBGGRR format)
    secondary_color: str = "&H000000FF"  # Red for karaoke
    outline_color: str = "&H00000000"  # Black
    back_color: str = "&H80000000"  # Semi-transparent black shadow
    bold: bool = True
    italic: bool = False
    outline: float = 4.0  # Thick outline for readability
    shadow: float = 2.0
    alignment: int = 2  # Bottom center (numpad position)
    margin_l: int = 60
    margin_r: int = 60
    margin_v: int = 80  # Vertical margin
    # ASS ScaleX/ScaleY for text (100 = normal)
    scale_x: float = 100.0
    scale_y: float = 100.0
    # Spacing between characters
    spacing: float = 0.0
    # Border style: 1 = outline + shadow, 3 = opaque box
    border_style: int = 1


# Pre-configured style presets
ASS_PRESETS: dict[CaptionPreset, ASSStyleConfig] = {
    CaptionPreset.CAPCUT_BOLD: ASSStyleConfig(
        fontname="Inter Bold",
        fontsize=95,  # Larger for better mobile visibility
        primary_color="&H00FFFFFF",
        outline_color="&H00000000",
        back_color="&H80000000",
        outline=6.0,
        shadow=3.5,
        bold=True,
        alignment=2,
        margin_v=120,
    ),
    CaptionPreset.KARAOKE: ASSStyleConfig(
        fontname="Inter Bold",
        fontsize=80,  # +10%
        primary_color="&H00FFFFFF",
        secondary_color="&H0000FFFF",  # Yellow for highlight
        outline_color="&H00000000",
        back_color="&H00000000",
        outline=4.5,
        shadow=2.0,
        bold=True,
        alignment=2,
        margin_v=100,
    ),
    CaptionPreset.MINIMAL_SCIENCE: ASSStyleConfig(
        fontname="Inter",
        fontsize=62,  # +10%
        primary_color="&H00FFFFFF",
        outline_color="&H00000000",
        back_color="&H00000000",
        outline=2.5,
        shadow=1.0,
        bold=False,
        alignment=2,
        margin_v=80,
    ),
    CaptionPreset.MRBEAST: ASSStyleConfig(
        fontname="Inter Bold",
        fontsize=100,  # +10% for maximum impact
        primary_color="&H00FFFFFF",
        outline_color="&H00000000",
        back_color="&H00000000",
        outline=7.0,
        shadow=4.0,
        bold=True,
        alignment=2,
        margin_v=150,
    ),
}


def color_to_ass(hex_color: str, alpha: int = 0) -> str:
    """Convert hex color (#RRGGBB) to ASS format (&HAABBGGRR).

    Args:
        hex_color: Color in #RRGGBB format
        alpha: Alpha value 0-255 (0 = opaque, 255 = transparent)
    """
    if hex_color.startswith("#"):
        hex_color = hex_color[1:]

    r = int(hex_color[0:2], 16)
    g = int(hex_color[2:4], 16)
    b = int(hex_color[4:6], 16)

    return f"&H{alpha:02X}{b:02X}{g:02X}{r:02X}"


def parse_ass_color(ass_color: str) -> pysubs2.Color:
    """Parse ASS color format (&HAABBGGRR) to pysubs2.Color.

    Args:
        ass_color: Color in ASS format (&HAABBGGRR)

    Returns:
        pysubs2.Color object
    """
    # Remove &H prefix if present
    if ass_color.startswith("&H"):
        ass_color = ass_color[2:]

    # Pad to 8 characters (AABBGGRR)
    ass_color = ass_color.zfill(8)

    a = int(ass_color[0:2], 16)
    b = int(ass_color[2:4], 16)
    g = int(ass_color[4:6], 16)
    r = int(ass_color[6:8], 16)

    return pysubs2.Color(r=r, g=g, b=b, a=a)


def create_ass_style(name: str, config: ASSStyleConfig) -> SSAStyle:
    """Create a pysubs2 SSAStyle from config."""
    style = SSAStyle()
    style.fontname = config.fontname
    style.fontsize = config.fontsize
    style.primarycolor = parse_ass_color(config.primary_color)
    style.secondarycolor = parse_ass_color(config.secondary_color)
    style.outlinecolor = parse_ass_color(config.outline_color)
    style.backcolor = parse_ass_color(config.back_color)
    style.bold = config.bold
    style.italic = config.italic
    style.outline = config.outline
    style.shadow = config.shadow
    style.alignment = config.alignment
    style.marginl = config.margin_l
    style.marginr = config.margin_r
    style.marginv = config.margin_v
    style.scalex = config.scale_x
    style.scaley = config.scale_y
    style.spacing = config.spacing
    style.borderstyle = config.border_style
    return style


class ASSGenerator:
    """Generate ASS subtitle files for video captions."""

    def __init__(
        self,
        video_width: int = 1080,
        video_height: int = 1920,
        preset: CaptionPreset = CaptionPreset.CAPCUT_BOLD,
    ):
        """Initialize ASS generator.

        Args:
            video_width: Output video width
            video_height: Output video height
            preset: Style preset to use
        """
        self.video_width = video_width
        self.video_height = video_height
        self.preset = preset

        # Create the subtitle file
        self.subs = SSAFile()
        self.subs.info["PlayResX"] = str(video_width)
        self.subs.info["PlayResY"] = str(video_height)

        # Add styles
        self._setup_styles()

    def _setup_styles(self) -> None:
        """Set up ASS styles based on preset."""
        # Main style from preset
        config = ASS_PRESETS.get(self.preset, ASS_PRESETS[CaptionPreset.CAPCUT_BOLD])
        self.subs.styles["Default"] = create_ass_style("Default", config)

        # Hook style (for top-positioned text)
        # Positioned at 18% from top
        hook_config = ASSStyleConfig(
            fontname=config.fontname,
            fontsize=int(config.fontsize * 0.85),
            primary_color=config.primary_color,
            outline_color=config.outline_color,
            back_color=config.back_color,
            outline=config.outline * 0.8,
            shadow=config.shadow,
            bold=config.bold,
            alignment=8,  # Top center
            margin_v=int(self.video_height * 0.18),
        )
        self.subs.styles["Hook"] = create_ass_style("Hook", hook_config)

        # Center style (for impact words at boom)
        center_config = ASSStyleConfig(
            fontname=config.fontname,
            fontsize=int(config.fontsize * 1.3),
            primary_color=config.primary_color,
            outline_color=config.outline_color,
            back_color=config.back_color,
            outline=config.outline * 1.2,
            shadow=config.shadow * 1.5,
            bold=True,
            alignment=5,  # Middle center
            margin_v=config.margin_v,
        )
        self.subs.styles["Center"] = create_ass_style("Center", center_config)

        # Countdown style (30% from top, for small countdown numbers)
        countdown_config = ASSStyleConfig(
            fontname=config.fontname,
            fontsize=int(config.fontsize * 1.3),
            primary_color=config.primary_color,
            outline_color=config.outline_color,
            back_color=config.back_color,
            outline=config.outline * 1.2,
            shadow=config.shadow * 1.5,
            bold=True,
            alignment=8,  # Top center
            margin_v=int(self.video_height * 0.30),
        )
        self.subs.styles["Countdown"] = create_ass_style("Countdown", countdown_config)

        # Main style (bottom position, for build-up text)
        # Positioned towards center (25% from bottom)
        main_config = ASSStyleConfig(
            fontname=config.fontname,
            fontsize=config.fontsize,
            primary_color=config.primary_color,
            outline_color=config.outline_color,
            back_color=config.back_color,
            outline=config.outline,
            shadow=config.shadow,
            bold=config.bold,
            alignment=2,  # Bottom center
            margin_v=int(self.video_height * 0.25),  # Closer to center
        )
        self.subs.styles["Main"] = create_ass_style("Main", main_config)

    def add_event(
        self,
        text: str,
        start_ms: int,
        end_ms: int,
        style: str = "Default",
        fade_in_ms: int = 0,
        fade_out_ms: int = 0,
        pop_in: bool = False,
    ) -> None:
        """Add a caption event to the subtitle file.

        Args:
            text: Caption text
            start_ms: Start time in milliseconds
            end_ms: End time in milliseconds
            style: ASS style name (Default, Hook, Center, Main)
            fade_in_ms: Fade in duration in milliseconds
            fade_out_ms: Fade out duration in milliseconds
            pop_in: Whether to apply pop-in scale animation
        """
        line = SSAEvent()
        line.start = start_ms
        line.end = end_ms
        line.style = style

        # Build the text with effects
        effect_tags = ""

        # Add fade effect
        if fade_in_ms > 0 or fade_out_ms > 0:
            effect_tags += f"\\fad({fade_in_ms},{fade_out_ms})"

        # Add pop-in animation (scale from 0 to 100)
        if pop_in:
            duration = min(200, (end_ms - start_ms) // 4)
            effect_tags += f"\\fscx0\\fscy0\\t(0,{duration},\\fscx100\\fscy100)"

        if effect_tags:
            line.text = "{" + effect_tags + "}" + text
        else:
            line.text = text

        self.subs.append(line)

    def save(self, output_path: Path) -> Path:
        """Save the ASS file.

        Args:
            output_path: Path to save the .ass file

        Returns:
            Path to the saved file
        """
        output_path = Path(output_path)
        self.subs.save(str(output_path))
        return output_path


# Position to ASS style mapping
POSITION_TO_STYLE = {
    "hook": "Hook",
    "main": "Main",
    "center": "Center",
    "countdown": "Countdown",
}


def generate_ass_from_resolved(
    output_path: Path,
    captions: list["ResolvedCaption"],
    video_width: int = 1080,
    video_height: int = 1920,
    preset: CaptionPreset = CaptionPreset.CAPCUT_BOLD,
) -> Path:
    """Generate an ASS subtitle file from resolved template captions.

    Args:
        output_path: Path to save the .ass file
        captions: List of ResolvedCaption from template resolution
        video_width: Output video width
        video_height: Output video height
        preset: Visual style preset

    Returns:
        Path to the generated .ass file
    """
    generator = ASSGenerator(
        video_width=video_width,
        video_height=video_height,
        preset=preset,
    )

    for caption in captions:
        # Map position to ASS style
        style = POSITION_TO_STYLE.get(caption.position, "Default")

        generator.add_event(
            text=caption.text,
            start_ms=caption.start_ms,
            end_ms=caption.end_ms,
            style=style,
            fade_in_ms=caption.effects.fade_in,
            fade_out_ms=caption.effects.fade_out,
            pop_in=caption.effects.pop_in,
        )

    return generator.save(output_path)
