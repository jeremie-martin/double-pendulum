"""Upload archive for tracking all YouTube uploads with full context.

Maintains a JSON Lines file at ~/.local/share/pendulum-tools/uploads.jsonl
Each line is a complete record of an upload for future analysis.

Concurrent safety: Uses file locking (fcntl.flock) to allow multiple
watchers to safely append to the same archive file.
"""

from __future__ import annotations

import fcntl
import json
import os
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Optional


def get_archive_path() -> Path:
    """Get the path to the uploads archive file."""
    data_dir = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    archive_dir = data_dir / "pendulum-tools"
    archive_dir.mkdir(parents=True, exist_ok=True)
    return archive_dir / "uploads.jsonl"


@dataclass
class UploadRecord:
    """Complete record of an upload for archival.

    Captures everything needed to analyze what works and what doesn't:
    - YouTube identifiers (video_id, url)
    - Content used (title, description, tags)
    - Simulation parameters (physics, render settings)
    - Visual settings (theme, color preset + params, post-processing preset + params)
    - Key moments (boom_frame, boom_seconds, chaos_frame)
    - Quality scores
    - Music used
    - Processing template used
    """

    # YouTube identifiers
    video_id: str
    url: str
    privacy: str
    uploaded_at: str

    # Content used (what was actually uploaded)
    title: str
    description: str
    tags: list[str]

    # Simulation metadata
    pendulum_count: int
    duration_seconds: float
    video_fps: int
    physics_quality: str

    # Physics parameters
    gravity: float
    length1: float
    length2: float
    mass1: float
    mass2: float
    initial_angle1_deg: float
    initial_angle2_deg: float

    # Theme (top-level combined theme name)
    theme: Optional[str] = None

    # Color settings (full)
    color_preset: Optional[str] = None  # color.preset_name
    color_scheme: Optional[str] = None  # color.scheme
    color_start: Optional[float] = None  # color.start
    color_end: Optional[float] = None  # color.end

    # Post-processing settings (full)
    postprocess_preset: Optional[str] = None  # post_process.preset_name
    tone_map: Optional[str] = None
    exposure: Optional[float] = None
    contrast: Optional[float] = None
    gamma: Optional[float] = None
    normalization: Optional[str] = None
    reinhard_white_point: Optional[float] = None

    # Key moments (in video time, not simulation time)
    boom_frame: Optional[int] = None
    boom_seconds: Optional[float] = None
    chaos_frame: Optional[int] = None

    # Quality scores
    boom_quality: Optional[float] = None
    peak_clarity: Optional[float] = None
    post_boom_sustain: Optional[float] = None
    causticness: Optional[float] = None
    final_uniformity: Optional[float] = None

    # Music
    music_id: Optional[str] = None
    music_title: Optional[str] = None
    music_drop_time_ms: Optional[int] = None

    # Processing
    processing_template: Optional[str] = None

    # Source tracking
    source_dir: Optional[str] = None  # Original video directory name

    # Arbitrary extra fields for future-proofing
    extra: dict[str, Any] = field(default_factory=dict)

    def to_json_line(self) -> str:
        """Serialize to a single JSON line."""
        data = asdict(self)
        # Remove None values and empty extra dict to save space
        data = {k: v for k, v in data.items() if v is not None and v != {}}
        return json.dumps(data, separators=(",", ":"))

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> UploadRecord:
        """Create from a dictionary (for loading from JSON)."""
        # Handle extra fields that aren't in the dataclass
        known_fields = {f for f in cls.__dataclass_fields__.keys()}
        extra = {k: v for k, v in data.items() if k not in known_fields}
        data = {k: v for k, v in data.items() if k in known_fields}
        data["extra"] = {**data.get("extra", {}), **extra}
        return cls(**data)

    @classmethod
    def from_metadata(
        cls,
        metadata: dict[str, Any],
        video_id: str,
        title: str,
        description: str,
        tags: list[str],
        privacy: str,
        source_dir: Optional[str] = None,
    ) -> UploadRecord:
        """Create an UploadRecord from raw metadata dict and upload info.

        Args:
            metadata: The full metadata.json contents
            video_id: YouTube video ID
            title: Title used for upload
            description: Description used for upload
            tags: Tags used for upload
            privacy: Privacy status
            source_dir: Original directory name (for tracking)
        """
        physics = metadata.get("physics", {})
        simulation = metadata.get("simulation", {})
        output = metadata.get("output", {})
        color = metadata.get("color", {})
        post_process = metadata.get("post_process", {})
        results = metadata.get("results", {})
        scores = metadata.get("scores", {})
        music = metadata.get("music", {})
        processing = metadata.get("processing", {})

        return cls(
            # YouTube identifiers
            video_id=video_id,
            url=f"https://youtu.be/{video_id}",
            privacy=privacy,
            uploaded_at=datetime.now().isoformat(),
            # Content used
            title=title,
            description=description,
            tags=tags,
            # Simulation metadata
            pendulum_count=simulation.get("pendulum_count", 0),
            duration_seconds=simulation.get("duration_seconds", 0),
            video_fps=output.get("video_fps", 30),
            physics_quality=simulation.get("physics_quality", "unknown"),
            # Physics parameters
            gravity=physics.get("gravity", 9.81),
            length1=physics.get("length1", 1.0),
            length2=physics.get("length2", 1.0),
            mass1=physics.get("mass1", 1.0),
            mass2=physics.get("mass2", 1.0),
            initial_angle1_deg=physics.get("initial_angle1_deg", 0.0),
            initial_angle2_deg=physics.get("initial_angle2_deg", 0.0),
            # Theme (top-level)
            theme=metadata.get("theme"),
            # Color settings (full)
            color_preset=color.get("preset_name"),
            color_scheme=color.get("scheme"),
            color_start=color.get("start"),
            color_end=color.get("end"),
            # Post-processing settings (full)
            postprocess_preset=post_process.get("preset_name"),
            tone_map=post_process.get("tone_map"),
            exposure=post_process.get("exposure"),
            contrast=post_process.get("contrast"),
            gamma=post_process.get("gamma"),
            normalization=post_process.get("normalization"),
            reinhard_white_point=post_process.get("reinhard_white_point"),
            # Key moments
            boom_frame=results.get("boom_frame"),
            boom_seconds=results.get("boom_seconds"),
            chaos_frame=results.get("chaos_frame"),
            # Quality scores
            boom_quality=scores.get("boom"),
            peak_clarity=scores.get("peak_clarity"),
            post_boom_sustain=scores.get("post_boom_sustain"),
            causticness=scores.get("causticness"),
            final_uniformity=results.get("final_uniformity"),
            # Music
            music_id=music.get("id"),
            music_title=music.get("title"),
            music_drop_time_ms=music.get("drop_time_ms"),
            # Processing
            processing_template=processing.get("template"),
            # Source tracking
            source_dir=source_dir,
        )


def append_upload_record(record: UploadRecord) -> None:
    """Append an upload record to the archive file.

    Uses file locking to ensure safe concurrent access from multiple
    watcher processes.
    """
    archive_path = get_archive_path()
    line = record.to_json_line() + "\n"

    # Open in append mode with exclusive lock
    with open(archive_path, "a") as f:
        # Acquire exclusive lock (blocks until available)
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
        try:
            f.write(line)
            f.flush()  # Ensure data is written before releasing lock
        finally:
            # Release lock
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)


def load_upload_records() -> list[UploadRecord]:
    """Load all upload records from the archive file."""
    archive_path = get_archive_path()
    if not archive_path.exists():
        return []

    records = []
    with open(archive_path) as f:
        # Shared lock for reading
        fcntl.flock(f.fileno(), fcntl.LOCK_SH)
        try:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        data = json.loads(line)
                        records.append(UploadRecord.from_dict(data))
                    except json.JSONDecodeError:
                        continue  # Skip malformed lines
        finally:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
    return records


def get_upload_stats() -> dict[str, Any]:
    """Get summary statistics from the archive."""
    records = load_upload_records()
    if not records:
        return {"total_uploads": 0}

    # Basic counts
    total = len(records)

    # Theme distribution (top-level theme)
    themes: dict[str, int] = {}
    for r in records:
        if r.theme:
            themes[r.theme] = themes.get(r.theme, 0) + 1

    # Color scheme distribution
    color_schemes: dict[str, int] = {}
    for r in records:
        if r.color_scheme:
            color_schemes[r.color_scheme] = color_schemes.get(r.color_scheme, 0) + 1

    # Color preset distribution
    color_presets: dict[str, int] = {}
    for r in records:
        if r.color_preset:
            color_presets[r.color_preset] = color_presets.get(r.color_preset, 0) + 1

    # Post-process preset distribution
    postprocess_presets: dict[str, int] = {}
    for r in records:
        if r.postprocess_preset:
            postprocess_presets[r.postprocess_preset] = (
                postprocess_presets.get(r.postprocess_preset, 0) + 1
            )

    # Boom time distribution
    boom_times = [r.boom_seconds for r in records if r.boom_seconds is not None]
    avg_boom = sum(boom_times) / len(boom_times) if boom_times else None

    # Processing template distribution
    templates: dict[str, int] = {}
    for r in records:
        if r.processing_template:
            templates[r.processing_template] = templates.get(r.processing_template, 0) + 1

    # Music distribution
    music_tracks: dict[str, int] = {}
    for r in records:
        if r.music_title:
            music_tracks[r.music_title] = music_tracks.get(r.music_title, 0) + 1

    return {
        "total_uploads": total,
        "themes": themes,
        "color_schemes": color_schemes,
        "color_presets": color_presets,
        "postprocess_presets": postprocess_presets,
        "average_boom_seconds": avg_boom,
        "processing_templates": templates,
        "music_tracks": music_tracks,
    }
