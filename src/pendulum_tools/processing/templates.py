"""Template system for video post-processing.

Loads templates and text pools from TOML config files.
Templates define motion effects and caption timing.
Text pools provide randomizable text content.
"""

from __future__ import annotations

import random
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from .motion import BoomPunchConfig, MotionConfig, ShakeConfig, SlowZoomConfig


def get_config_dir() -> Path:
    """Get the config directory path."""
    # Look relative to this module
    module_dir = Path(__file__).parent.parent.parent.parent
    config_dir = module_dir / "config"

    if config_dir.exists():
        return config_dir

    # Fallback: check current working directory
    cwd_config = Path.cwd() / "config"
    if cwd_config.exists():
        return cwd_config

    raise FileNotFoundError(
        f"Config directory not found. Looked in: {config_dir}, {cwd_config}"
    )


@dataclass
class CaptionEffects:
    """Effects to apply to a caption."""

    fade_in: int = 0  # Fade in duration in ms
    fade_out: int = 0  # Fade out duration in ms
    pop_in: bool = False  # Scale animation on appear


@dataclass
class CaptionConfig:
    """Configuration for a single caption in a template."""

    pool: str  # Text pool key (e.g., "hooks.science")
    position: str  # Position: "hook" (top), "main" (bottom), "center"
    start: float | str  # Start time or expression
    end: float | str  # End time or expression
    effects: CaptionEffects = field(default_factory=CaptionEffects)


@dataclass
class TemplateConfig:
    """Full template configuration."""

    name: str
    description: str
    motion: MotionConfig
    captions: list[CaptionConfig]


@dataclass
class ResolvedCaption:
    """A caption with resolved text and timing."""

    text: str
    position: str
    start_ms: int
    end_ms: int
    effects: CaptionEffects


class TextPoolLibrary:
    """Library of text pools loaded from TOML."""

    def __init__(self, config_path: Optional[Path] = None):
        """Load text pools from TOML file.

        Args:
            config_path: Path to text_pools.toml (or auto-detect)
        """
        if config_path is None:
            config_path = get_config_dir() / "text_pools.toml"

        if not config_path.exists():
            raise FileNotFoundError(f"Text pools config not found: {config_path}")

        with open(config_path, "rb") as f:
            self._pools = tomllib.load(f)

    def get_pool(self, key: str) -> list[str]:
        """Get a text pool by key (e.g., 'hooks.science').

        Args:
            key: Dot-separated pool key

        Returns:
            List of text variations

        Raises:
            KeyError: If pool not found
        """
        parts = key.split(".")
        current = self._pools

        for part in parts:
            if part not in current:
                raise KeyError(f"Text pool not found: {key}")
            current = current[part]

        if isinstance(current, dict) and "texts" in current:
            return current["texts"]
        elif isinstance(current, list):
            return current
        else:
            raise KeyError(f"Invalid text pool format: {key}")

    def pick(self, key: str, seed: Optional[int] = None) -> str:
        """Randomly pick a text from a pool.

        Args:
            key: Pool key
            seed: Optional random seed for reproducibility

        Returns:
            Selected text
        """
        pool = self.get_pool(key)
        if seed is not None:
            rng = random.Random(seed)
            return rng.choice(pool)
        return random.choice(pool)

    def list_pools(self) -> list[str]:
        """List all available pool keys."""
        keys = []
        self._collect_keys(self._pools, "", keys)
        return keys

    def _collect_keys(self, obj: dict, prefix: str, keys: list[str]) -> None:
        """Recursively collect pool keys."""
        for key, value in obj.items():
            full_key = f"{prefix}.{key}" if prefix else key
            if isinstance(value, dict):
                if "texts" in value:
                    keys.append(full_key)
                else:
                    self._collect_keys(value, full_key, keys)


class TemplateLibrary:
    """Library of templates loaded from TOML."""

    def __init__(self, config_path: Optional[Path] = None):
        """Load templates from TOML file.

        Args:
            config_path: Path to templates.toml (or auto-detect)
        """
        if config_path is None:
            config_path = get_config_dir() / "templates.toml"

        if not config_path.exists():
            raise FileNotFoundError(f"Templates config not found: {config_path}")

        with open(config_path, "rb") as f:
            data = tomllib.load(f)

        self._templates: dict[str, TemplateConfig] = {}
        self._parse_templates(data.get("template", {}))

    def _parse_templates(self, templates_data: dict) -> None:
        """Parse template definitions from TOML data."""
        for name, data in templates_data.items():
            motion = self._parse_motion(data.get("motion", {}))
            captions = self._parse_captions(data.get("captions", []))

            self._templates[name] = TemplateConfig(
                name=name,
                description=data.get("description", ""),
                motion=motion,
                captions=captions,
            )

    def _parse_motion(self, motion_data: dict) -> MotionConfig:
        """Parse motion config from TOML data."""
        slow_zoom = None
        boom_punch = None
        shake = None

        if "slow_zoom" in motion_data:
            sz = motion_data["slow_zoom"]
            slow_zoom = SlowZoomConfig(
                start=sz.get("start", 1.0),
                end=sz.get("end", 1.1),
            )

        if "boom_punch" in motion_data:
            bp = motion_data["boom_punch"]
            boom_punch = BoomPunchConfig(
                factor=bp.get("factor", 1.3),
                ramp_in=bp.get("ramp_in", 0.4),
                hold=bp.get("hold", 0.4),
                ramp_out=bp.get("ramp_out", 0.4),
            )

        if "shake" in motion_data:
            sh = motion_data["shake"]
            shake = ShakeConfig(
                frames=sh.get("frames", 4),
                intensity=sh.get("intensity", 8),
            )

        return MotionConfig(
            slow_zoom=slow_zoom,
            boom_punch=boom_punch,
            shake=shake,
        )

    def _parse_captions(self, captions_data: list) -> list[CaptionConfig]:
        """Parse caption configs from TOML data."""
        captions = []

        for cap in captions_data:
            effects_data = cap.get("effects", {})
            effects = CaptionEffects(
                fade_in=effects_data.get("fade_in", 0),
                fade_out=effects_data.get("fade_out", 0),
                pop_in=effects_data.get("pop_in", False),
            )

            captions.append(
                CaptionConfig(
                    pool=cap["pool"],
                    position=cap.get("position", "main"),
                    start=cap["start"],
                    end=cap["end"],
                    effects=effects,
                )
            )

        return captions

    def get(self, name: str) -> TemplateConfig:
        """Get a template by name.

        Args:
            name: Template name

        Returns:
            TemplateConfig

        Raises:
            KeyError: If template not found
        """
        if name not in self._templates:
            raise KeyError(f"Template not found: {name}. Available: {list(self._templates.keys())}")
        return self._templates[name]

    def list_templates(self) -> list[str]:
        """List all available template names."""
        return list(self._templates.keys())

    def pick_random(self, seed: Optional[int] = None) -> TemplateConfig:
        """Pick a random template.

        Args:
            seed: Optional random seed

        Returns:
            Random TemplateConfig
        """
        names = self.list_templates()
        if seed is not None:
            rng = random.Random(seed)
            name = rng.choice(names)
        else:
            name = random.choice(names)
        return self.get(name)


def resolve_timing_ms(
    time_expr: float | str,
    boom_seconds: float,
    video_duration: float,
) -> int:
    """Resolve a timing expression to milliseconds.

    Args:
        time_expr: Time expression (float seconds, or "boom", "boom+1", "boom-0.5")
        boom_seconds: Boom time in seconds
        video_duration: Video duration in seconds

    Returns:
        Time in milliseconds
    """
    if isinstance(time_expr, (int, float)):
        return int(float(time_expr) * 1000)

    expr = str(time_expr).strip().lower()

    if expr == "end":
        return int(video_duration * 1000)

    if expr == "boom":
        return int(boom_seconds * 1000)

    # Parse boom+N or boom-N
    match = re.match(r"boom\s*([+-])\s*(\d+\.?\d*)", expr)
    if match:
        op, value = match.groups()
        offset = float(value)
        if op == "-":
            offset = -offset
        return int((boom_seconds + offset) * 1000)

    raise ValueError(f"Unknown timing expression: {time_expr}")


def format_text(
    text: str,
    pendulum_count: int,
    boom_seconds: float,
) -> str:
    """Format text with placeholders.

    Supported placeholders:
    - {count}: Formatted pendulum count (e.g., "1M", "500K")
    - {boom_time}: Boom time (e.g., "8.4s")

    Args:
        text: Text with placeholders
        pendulum_count: Number of pendulums
        boom_seconds: Boom time in seconds

    Returns:
        Formatted text
    """
    # Format count
    if pendulum_count >= 1_000_000:
        count_str = f"{pendulum_count / 1_000_000:.0f}M"
    elif pendulum_count >= 1_000:
        count_str = f"{pendulum_count / 1_000:.0f}K"
    else:
        count_str = str(pendulum_count)

    # Format boom time
    boom_time_str = f"{boom_seconds:.1f}s"

    return text.replace("{count}", count_str).replace("{boom_time}", boom_time_str)


def resolve_template(
    template: TemplateConfig,
    text_pools: TextPoolLibrary,
    boom_seconds: float,
    video_duration: float,
    pendulum_count: int,
    seed: Optional[int] = None,
) -> list[ResolvedCaption]:
    """Resolve a template to concrete captions with text and timing.

    Args:
        template: Template configuration
        text_pools: Text pool library
        boom_seconds: Boom time in seconds
        video_duration: Video duration in seconds
        pendulum_count: Number of pendulums
        seed: Optional random seed for text selection

    Returns:
        List of resolved captions
    """
    resolved = []

    # Use seed offset for each caption to get different picks
    for i, cap in enumerate(template.captions):
        # Pick text from pool
        cap_seed = None if seed is None else seed + i
        try:
            text = text_pools.pick(cap.pool, seed=cap_seed)
        except KeyError:
            print(f"Warning: Text pool not found: {cap.pool}")
            continue

        # Format text with placeholders
        text = format_text(text, pendulum_count, boom_seconds)

        # Resolve timing
        try:
            start_ms = resolve_timing_ms(cap.start, boom_seconds, video_duration)
            end_ms = resolve_timing_ms(cap.end, boom_seconds, video_duration)
        except ValueError as e:
            print(f"Warning: Invalid timing: {e}")
            continue

        # Clamp to valid range
        start_ms = max(0, start_ms)
        end_ms = min(end_ms, int(video_duration * 1000))

        if start_ms >= end_ms:
            continue

        resolved.append(
            ResolvedCaption(
                text=text,
                position=cap.position,
                start_ms=start_ms,
                end_ms=end_ms,
                effects=cap.effects,
            )
        )

    return resolved


# Convenience function to load both libraries
def load_template_system(
    templates_path: Optional[Path] = None,
    pools_path: Optional[Path] = None,
) -> tuple[TemplateLibrary, TextPoolLibrary]:
    """Load both template and text pool libraries.

    Args:
        templates_path: Optional path to templates.toml
        pools_path: Optional path to text_pools.toml

    Returns:
        Tuple of (TemplateLibrary, TextPoolLibrary)
    """
    templates = TemplateLibrary(templates_path)
    pools = TextPoolLibrary(pools_path)
    return templates, pools
