"""Pydantic models for simulation metadata."""

from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path
from typing import Optional

from pydantic import BaseModel, Field


class SimulationConfig(BaseModel):
    """Configuration used for the simulation."""

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


class SimulationResults(BaseModel):
    """Results from the simulation run."""

    frames_completed: int
    boom_frame: Optional[int] = None
    boom_variance: Optional[float] = None
    chaos_frame: Optional[int] = None
    chaos_variance: Optional[float] = None
    final_uniformity: float = 0.0  # Distribution uniformity on disk (0=concentrated, 1=uniform)

    def boom_seconds(self, config: SimulationConfig) -> Optional[float]:
        """Calculate boom time in seconds."""
        if self.boom_frame is None:
            return None
        frame_duration = config.duration_seconds / config.total_frames
        return self.boom_frame * frame_duration


class SimulationTiming(BaseModel):
    """Timing statistics from the simulation."""

    total_seconds: float
    physics_seconds: float
    render_seconds: float
    io_seconds: float


class ScoreData(BaseModel):
    """Quality scores from simulation analyzers.

    These are the normalized 0-1 scores computed by BoomAnalyzer and CausticnessAnalyzer.
    The scores are used for filtering and ranking simulations.
    """

    # Core quality scores (0-1 normalized)
    boom: Optional[float] = None              # Boom analyzer score
    causticness: Optional[float] = None       # Causticness analyzer score

    # Peak clarity: main_peak / (main_peak + max_competitor)
    # 1.0 = no competing peaks, 0.5 = equal height peaks
    peak_clarity: Optional[float] = None

    # Post-boom sustain: normalized area under causticness curve after boom
    post_boom_sustain: Optional[float] = None

    @property
    def quality_score(self) -> float:
        """Combined quality score (average of available scores)."""
        scores = [s for s in [self.causticness, self.peak_clarity, self.post_boom_sustain] if s is not None]
        return sum(scores) / len(scores) if scores else 0.0


class VideoMetadata(BaseModel):
    """Complete metadata for a simulation video."""

    version: str
    created_at: datetime
    config: SimulationConfig
    results: SimulationResults
    timing: SimulationTiming
    scores: Optional[ScoreData] = None  # Quality scores from analyzers

    @classmethod
    def from_file(cls, path: str | Path) -> VideoMetadata:
        """Load metadata from a JSON file."""
        with open(path) as f:
            data = json.load(f)
        return cls(**data)

    @property
    def boom_seconds(self) -> Optional[float]:
        """Convenience property for boom time in seconds."""
        return self.results.boom_seconds(self.config)

    @property
    def video_duration(self) -> float:
        """Convenience property for video duration."""
        return self.config.video_duration

    @property
    def simulation_speed(self) -> float:
        """Convenience property for simulation speed."""
        return self.config.simulation_speed

    @property
    def best_frame_seconds(self) -> Optional[float]:
        """Time of best visual quality frame in seconds.

        Returns the boom time as the "best" frame for thumbnails/effects.
        """
        return self.boom_seconds
