"""Pydantic models for simulation metadata."""

from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path
from typing import Optional

from pydantic import BaseModel


# =============================================================================
# New metadata structure models (matching C++ output)
# =============================================================================


class PhysicsConfig(BaseModel):
    """Physics parameters for the simulation."""

    gravity: float
    length1: float
    length2: float
    mass1: float
    mass2: float
    initial_angle1_deg: float
    initial_angle2_deg: float
    initial_velocity1: float
    initial_velocity2: float


class SimulationSection(BaseModel):
    """Simulation parameters."""

    pendulum_count: int
    angle_variation_deg: float
    duration_seconds: float
    total_frames: int
    physics_quality: str
    max_dt: float
    substeps: int
    dt: float


class RenderConfig(BaseModel):
    """Render settings."""

    width: int
    height: int
    thread_count: int = 0


class ColorConfig(BaseModel):
    """Color scheme settings."""

    scheme: str
    start: float
    end: float


class PostProcessConfig(BaseModel):
    """Post-processing settings."""

    tone_map: str
    exposure: float
    contrast: float
    gamma: float
    normalization: str
    reinhard_white_point: float = 1.0


class OutputConfig(BaseModel):
    """Output settings."""

    video_fps: int
    video_duration: float
    simulation_speed: float
    video_codec: str
    video_crf: int


class SimulationResults(BaseModel):
    """Results from the simulation run."""

    frames_completed: int
    boom_frame: Optional[int] = None
    boom_seconds: Optional[float] = None
    boom_causticness: Optional[float] = None
    chaos_frame: Optional[int] = None
    chaos_variance: Optional[float] = None
    final_uniformity: float = 0.0


class SimulationTiming(BaseModel):
    """Timing statistics from the simulation."""

    total_seconds: float
    physics_seconds: float
    render_seconds: float
    io_seconds: float


class TargetConfig(BaseModel):
    """Target detection configuration."""

    name: str
    type: str
    metric: str
    method: str
    offset_seconds: Optional[float] = None


class PredictionResult(BaseModel):
    """Prediction result from a target."""

    type: str
    frame: Optional[int] = None
    seconds: Optional[float] = None
    score: Optional[float] = None


class ScoreData(BaseModel):
    """Quality scores from simulation analyzers."""

    # Core quality scores (0-1 normalized)
    boom: Optional[float] = None
    causticness: Optional[float] = None
    peak_clarity: Optional[float] = None
    post_boom_sustain: Optional[float] = None

    @property
    def quality_score(self) -> float:
        """Combined quality score (average of available scores)."""
        scores = [s for s in [self.causticness, self.peak_clarity, self.post_boom_sustain] if s is not None]
        return sum(scores) / len(scores) if scores else 0.0


# =============================================================================
# Compatibility layer for legacy code accessing metadata.config.X
# =============================================================================


class SimulationConfig(BaseModel):
    """Compatibility class that aggregates config fields for legacy access.

    This allows code using metadata.config.X to continue working.
    """

    duration_seconds: float
    total_frames: int
    video_fps: int
    pendulum_count: int
    width: int
    height: int
    physics_quality: str
    max_dt: float
    substeps: int
    dt: float

    @property
    def video_duration(self) -> float:
        """Calculate video duration in seconds."""
        return self.total_frames / self.video_fps

    @property
    def simulation_speed(self) -> float:
        """Calculate simulation speed multiplier (physics time / video time)."""
        return self.duration_seconds / self.video_duration


# =============================================================================
# Main VideoMetadata model
# =============================================================================


class VideoMetadata(BaseModel):
    """Complete metadata for a simulation video.

    Supports the new C++ metadata format with separate sections.
    """

    version: str
    created_at: datetime

    # New structure sections
    physics: PhysicsConfig
    simulation: SimulationSection
    render: RenderConfig
    color: ColorConfig
    post_process: PostProcessConfig
    output: OutputConfig
    results: SimulationResults
    timing: SimulationTiming

    # Optional sections
    targets: Optional[list[TargetConfig]] = None
    scores: Optional[ScoreData] = None
    predictions: Optional[dict[str, PredictionResult]] = None

    @classmethod
    def from_file(cls, path: str | Path) -> VideoMetadata:
        """Load metadata from a JSON file."""
        with open(path) as f:
            data = json.load(f)
        return cls(**data)

    @property
    def config(self) -> SimulationConfig:
        """Compatibility property for legacy code accessing metadata.config.X"""
        return SimulationConfig(
            duration_seconds=self.simulation.duration_seconds,
            total_frames=self.simulation.total_frames,
            video_fps=self.output.video_fps,
            pendulum_count=self.simulation.pendulum_count,
            width=self.render.width,
            height=self.render.height,
            physics_quality=self.simulation.physics_quality,
            max_dt=self.simulation.max_dt,
            substeps=self.simulation.substeps,
            dt=self.simulation.dt,
        )

    @property
    def boom_seconds(self) -> Optional[float]:
        """Convenience property for boom time in seconds."""
        return self.results.boom_seconds

    @property
    def video_duration(self) -> float:
        """Convenience property for video duration."""
        return self.output.video_duration

    @property
    def simulation_speed(self) -> float:
        """Convenience property for simulation speed."""
        return self.output.simulation_speed

    @property
    def best_frame_seconds(self) -> Optional[float]:
        """Time of best visual quality frame in seconds.

        Returns the boom time as the "best" frame for thumbnails/effects.
        """
        return self.boom_seconds
