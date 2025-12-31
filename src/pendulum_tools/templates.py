"""Templates for YouTube video metadata generation."""

from __future__ import annotations

import random
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .models import VideoMetadata

# Title Templates - randomly selected for variety
# Philosophy: Not always "double pendulum", mix themes (chaos, butterfly effect, satisfying)
# Keep titles clear, searchable, and scroll-stopping
TITLE_TEMPLATES = [
    # Chaos-focused - strong performers
    "Chaos Theory Visualized",
    "From Order to Chaos",
    "The Moment Chaos Begins",
    "When Order Breaks Down",
    "Pure Chaos",
    "Controlled Chaos",
    "Chaos Unfolds",
    "Beautiful Chaos",
    "Chaos in Motion",
    # Butterfly effect themed - highly searchable
    "The Butterfly Effect",
    "The Butterfly Effect Visualized",
    "Tiny Change, Massive Chaos",
    "Small Differences, Big Chaos",
    "One Tiny Difference",
    "0.00001° Makes All The Difference",
    # Satisfying/hypnotic vibe
    "Oddly Satisfying Chaos",
    "Hypnotic",
    "Mesmerizing Physics",
    "Strangely Satisfying",
    "So Satisfying",
    # Count-focused (impressive numbers)
    "{count_formatted} Simulations, One Tiny Difference",
    "{count_formatted} Identical Starts, Total Chaos",
    "{count_formatted} Start Together, Then This Happens",
    "{count_formatted} Pendulums, One Outcome",
    "{count_formatted} Paths Diverge",
    # Curiosity-driven (scroll stoppers)
    "Watch Until The End",
    "You Won't See It Coming",
    "This Is Why Prediction Is Impossible",
    "They All Start The Same",
    "Same Start, Different End",
    "Wait For It",
    "Keep Watching",
    # Physics/science
    "Physics Is Beautiful",
    "Nature Is Chaotic",
    "Why We Can't Predict The Future",
    "Deterministic Chaos",
    # Clean/minimal
    "Chaos",
    "Order to Chaos",
    "Divergence",
]

# Hashtags to randomly append to titles (0-3 selected)
TITLE_HASHTAGS = [
    "#chaos",
    "#physics",
    "#satisfying",
    "#simulation",
    "#science",
    "#hypnotic",
    "#mesmerizing",
    "#butterflyeffect",
    "#math",
    "#pendulum",
]

# Description Templates - mix of minimal and detailed
# Note: {boom_time} is VIDEO time (when it appears on screen), not simulation time
# Philosophy: Not always mention boom time, keep it clean, avoid being too dry/educational
DESCRIPTION_TEMPLATES = [
    # Ultra minimal
    """\
{count_formatted} identical starts. Complete chaos.

#chaos #physics #satisfying #butterflyeffect""",

    # Butterfly effect focus
    """\
The butterfly effect: tiny differences create massive outcomes.

{count_formatted} simulations prove it.

#butterflyeffect #chaos #physics #satisfying""",

    # Minimal with boom time
    """\
{count_formatted} pendulums. One tiny difference.

Watch for {boom_time}.

#chaos #physics #satisfying #simulation""",

    # Mysterious/intriguing
    """\
They all start the same way.

They don't end the same way.

#chaos #physics #satisfying #butterflyeffect""",

    # Short punchy with boom time
    """\
Order becomes chaos at {boom_time}.

{count_formatted} simulations.

#chaos #physics #satisfying #simulation""",

    # Clean statement
    """\
{count_formatted} pendulums. Tiny differences. Total chaos.

#chaos #physics #satisfying #butterflyeffect""",

    # Curiosity
    """\
Same start. Different end.

#chaos #physics #satisfying #simulation""",

    # Question style
    """\
What happens when {count_formatted} start almost identical?

Chaos.

#chaos #physics #satisfying #butterflyeffect""",

    # Pure minimal - just hashtags
    """\
#chaos #physics #simulation #satisfying #pendulum""",

    # Science-focused
    """\
Deterministic chaos: the outcome is predetermined, but impossible to predict.

{count_formatted} pendulums demonstrate why.

#chaostheory #physics #science #simulation""",

    # One-liner poetic
    """\
Where physics meets art.

#physics #simulation #chaos #satisfying""",

    # Story hint
    """\
{count_formatted} begin together. None finish together.

#chaos #butterflyeffect #physics #simulation""",

    # Math angle
    """\
0.00001° difference. Completely different outcomes.

#chaos #math #physics #simulation""",

    # Hypnotic vibe
    """\
Just watch.

#satisfying #hypnotic #chaos #physics""",

    # Double pendulum explicit
    """\
Double pendulum chaos.

{count_formatted} simulations.

#pendulum #chaos #physics #simulation""",
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
    """Generate a random title from templates with 0-3 hashtags."""
    template = random.choice(TITLE_TEMPLATES)
    title = template.format(
        count_formatted=format_count(metadata.config.pendulum_count),
        pendulum_count=metadata.config.pendulum_count,
    )

    # Add 0-3 random hashtags
    num_hashtags = random.randint(0, 3)
    if num_hashtags > 0:
        hashtags = random.sample(TITLE_HASHTAGS, num_hashtags)
        title = f"{title} {' '.join(hashtags)}"

    return title


def generate_description(metadata: VideoMetadata) -> str:
    """Generate a random description from templates."""
    template = random.choice(DESCRIPTION_TEMPLATES)

    # Use VIDEO time (boom_frame / fps), not simulation time
    boom_time = "N/A"
    if metadata.results and metadata.results.boom_frame:
        video_boom_seconds = metadata.results.boom_frame / metadata.config.video_fps
        boom_time = f"{video_boom_seconds:.0f}s"
    elif metadata.boom_seconds is not None:
        # Fallback to simulation time if no boom_frame
        boom_time = f"{metadata.boom_seconds:.0f}s"

    return template.format(
        count_formatted=format_count(metadata.config.pendulum_count),
        pendulum_count=metadata.config.pendulum_count,
        boom_time=boom_time,
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


# Caption Styles for Video Overlays
# Each style is a dict mapping phase names to caption definitions.
# Caption definition: (text, start_time, end_time)
# Times can be:
#   - float: absolute seconds
#   - "boom": boom time
#   - "boom+N" or "boom-N": relative to boom
#   - "end": video end time
#
# Text can contain {count} placeholder for pendulum count.

CaptionDefinition = tuple[str, float | str, float | str]

CAPTION_STYLES: dict[str, dict[str, CaptionDefinition]] = {
    # No text overlays
    "minimal": {},

    # Simple hook - just "Wait for it..."
    "wait_for_it": {
        "pre_boom": ("Wait for it...", 0, "boom-1"),
    },

    # Science/educational framing
    "science": {
        "intro": ("Same starting position.", 0, 3),
        "mid": ("Tiny differences...", 3, "boom-0.3"),
        "boom": ("CHAOS.", "boom-0.3", "boom+2"),
    },

    # Viewer challenge/engagement
    "viewer_challenge": {
        "challenge": ("Can you spot when chaos begins?", 0, "boom-2"),
        "reveal": ("Did you catch it?", "boom+1", "boom+3"),
    },

    # Micro-story narrative
    "micro_story": {
        "intro": ("{count} pendulums start together.", 0, 2),
        "tension": ("Just 0.00001 degrees apart.", 2, "boom-1"),
        "climax": ("Then THIS happens.", "boom", "boom+2"),
    },

    # Dramatic countdown feel
    "dramatic": {
        "setup": ("Order.", 0, 2),
        "tension": ("...", 2, "boom-0.5"),
        "release": ("Chaos.", "boom", "boom+2"),
    },

    # Minimal end card
    "end_card": {
        "end": ("{count} Double Pendulums", "boom+2", "end"),
    },
}


def get_caption_style(style_name: str) -> dict[str, CaptionDefinition]:
    """Get a caption style by name.

    Args:
        style_name: Style name or "random" for random selection

    Returns:
        Caption style dictionary
    """
    if style_name == "random":
        # Exclude minimal from random selection
        choices = [k for k in CAPTION_STYLES.keys() if k != "minimal"]
        style_name = random.choice(choices)

    return CAPTION_STYLES.get(style_name, {})
