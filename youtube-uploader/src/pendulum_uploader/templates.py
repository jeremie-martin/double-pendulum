"""Templates for YouTube video metadata generation."""

from __future__ import annotations

import random
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .models import VideoMetadata

# Title Templates - randomly selected for variety
TITLE_TEMPLATES = [
    "{count_formatted} Pendulums Diverge into Chaos",
    "Double Pendulum Chaos - {count_formatted} Simulations",
    "When {count_formatted} Pendulums Go Chaotic",
    "Mesmerizing Chaos: {count_formatted} Double Pendulums",
    "The Butterfly Effect with {count_formatted} Pendulums",
]

# Description Templates
DESCRIPTION_TEMPLATES = [
    """\
Watch {count_formatted} double pendulums start nearly identical, then diverge into beautiful chaos.

This simulation demonstrates the butterfly effect - tiny differences in initial conditions lead to completely different outcomes. Each pendulum started with angles differing by a fraction of a degree.

Simulation Details:
- Pendulum count: {pendulum_count:,}
- Physics duration: {duration:.1f} seconds
- Playback speed: {speed:.1f}x
- Resolution: {width}x{height}
- Chaos begins at: {boom_time}

Created with custom C++/OpenGL simulation software.

#doublependulum #chaos #physics #simulation #satisfying #butterflyeffect""",
    """\
A mesmerizing visualization of chaos theory in action.

{count_formatted} double pendulums, each starting from almost identical positions, quickly diverge into a stunning display of chaotic motion. The "boom" - when chaos visibly emerges - happens around {boom_time}.

Technical specs:
- {pendulum_count:,} individual pendulum simulations
- {duration:.1f}s of physics at {speed:.1f}x speed
- High-precision RK4 integration ({substeps} substeps/frame)
- {width}x{height} resolution

The colors represent each pendulum's position in the initial lineup, creating a rainbow of chaos.

#physics #simulation #doublependulum #chaostheory #satisfying #hypnotic #mesmerizing""",
]

# Base tags (always included)
BASE_TAGS = [
    "double pendulum",
    "chaos theory",
    "physics simulation",
    "butterfly effect",
    "satisfying video",
    "mesmerizing",
    "hypnotic",
    "physics",
    "simulation",
    "chaos",
    "pendulum",
    "science",
    "math",
    "visualization",
    "oddly satisfying",
    "relaxing",
]


def format_count(count: int) -> str:
    """Format pendulum count for titles (e.g., '1 Million', '100K')."""
    if count >= 1_000_000:
        millions = count / 1_000_000
        if millions == int(millions):
            return f"{int(millions)} Million"
        return f"{millions:.1f} Million"
    elif count >= 1_000:
        thousands = count / 1_000
        if thousands == int(thousands):
            return f"{int(thousands)}K"
        return f"{thousands:.1f}K"
    return str(count)


def get_count_tags(count: int) -> list[str]:
    """Get dynamic tags based on pendulum count."""
    tags = []
    if count >= 1_000_000:
        tags.extend(["million pendulums", "1 million", "massive simulation"])
    elif count >= 500_000:
        tags.extend(["500k pendulums", "half million"])
    elif count >= 100_000:
        tags.extend(["100k pendulums", "hundred thousand"])
    elif count >= 10_000:
        tags.extend(["10k pendulums", "ten thousand"])
    return tags


def generate_title(metadata: VideoMetadata) -> str:
    """Generate a random title from templates."""
    template = random.choice(TITLE_TEMPLATES)
    return template.format(
        count_formatted=format_count(metadata.config.pendulum_count),
        pendulum_count=metadata.config.pendulum_count,
    )


def generate_description(metadata: VideoMetadata) -> str:
    """Generate a random description from templates."""
    template = random.choice(DESCRIPTION_TEMPLATES)

    boom_time = "N/A"
    if metadata.boom_seconds is not None:
        boom_time = f"{metadata.boom_seconds:.1f}s"

    return template.format(
        count_formatted=format_count(metadata.config.pendulum_count),
        pendulum_count=metadata.config.pendulum_count,
        duration=metadata.config.duration_seconds,
        speed=metadata.simulation_speed,
        width=metadata.config.width,
        height=metadata.config.height,
        boom_time=boom_time,
        substeps=metadata.config.substeps,
    )


def generate_tags(metadata: VideoMetadata) -> list[str]:
    """Generate tags for the video."""
    tags = BASE_TAGS.copy()
    tags.extend(get_count_tags(metadata.config.pendulum_count))

    # YouTube has a 500 character limit for tags
    # Keep the most important ones
    return tags[:30]


def generate_all_titles(metadata: VideoMetadata) -> list[str]:
    """Generate all possible titles (for preview mode)."""
    return [
        template.format(
            count_formatted=format_count(metadata.config.pendulum_count),
            pendulum_count=metadata.config.pendulum_count,
        )
        for template in TITLE_TEMPLATES
    ]
