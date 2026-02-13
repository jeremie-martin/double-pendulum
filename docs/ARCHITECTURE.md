# Architecture

## Data Flow

```
Config (TOML)
     │
     ▼
┌─────────────────────────────────────────────────────────────┐
│                    SIMULATION LOOP                          │
│                                                             │
│  Physics Thread      Main Thread          I/O Thread        │
│  ┌────────────┐     ┌────────────┐       ┌────────────┐    │
│  │ RK4 step   │────▶│ Metrics    │       │            │    │
│  │ N pendula  │     │ GPU render │──────▶│ Video      │    │
│  │ substeps   │     │ post-proc  │       │ write      │    │
│  └────────────┘     └────────────┘       └────────────┘    │
│       N+1               frame N            frame N-1        │
└─────────────────────────────────────────────────────────────┘
     │
     ▼
Post-simulation: target evaluation, quality scoring, metadata
```

**Pipeline parallelism:** Physics (CPU), rendering (GPU), and I/O run concurrently using double-buffered state and pixel data.

## Core Components

| Component | File | Responsibility |
|-----------|------|----------------|
| `Simulation` | `simulation.cpp` | Orchestrates loop, threading, output |
| `Pendulum` | `pendulum.h` | RK4 physics integration |
| `GLRenderer` | `gl_renderer.cpp` | GPU line rendering, post-processing |
| `MetricsCollector` | `metrics_collector.h` | Time-series tracking |
| `TargetEvaluator` | `target_evaluator.h` | Frame detection, quality scoring |

## Physics

4th-order Runge-Kutta integration of Lagrangian equations of motion. Quality controlled via `max_dt`:

| Quality | max_dt | Steps/Period | Use Case |
|---------|--------|--------------|----------|
| low | 20ms | ~100 | Fast probes |
| medium | 12ms | ~167 | Acceptable |
| high | 7ms | ~286 | Default |
| ultra | 3ms | ~667 | Perfect accuracy |

Substeps computed as `ceil(frame_dt / max_dt)`.

## GPU Rendering

Two-pass approach:

1. **Accumulation:** Lines rendered with additive blending into `GL_RGBA32F` texture (floating-point to handle millions of overlaps)
2. **Post-processing:** Fullscreen shader applies normalize → exposure → tone map → contrast → gamma

Compute shader (OpenGL 4.3+) optionally accelerates max-value reduction for normalization.

## Metrics

### Physics Metrics (21 total)

**Distribution:**
- `variance` - Angular variance (stable, CV <1%)
- `circular_spread` - Distribution uniformity (0=concentrated, 1=uniform)
- `angular_range` - Normalized angular coverage

**Caustic Detection:**
- `angular_causticness` - coverage × Gini on angle1+angle2
- `tip_causticness` - Causticness using geometric tip angle
- `local_coherence` - Neighbor distance vs random distance

**Velocity:**
- `velocity_dispersion` - Spread of velocity directions
- `velocity_bimodality` - Detects opposing motion groups

**GPU:**
- `brightness` - Mean pixel intensity
- `coverage` - Fraction of non-zero pixels
- `max_value` - Peak intensity before post-processing

### Metric Series

Each metric tracked as time series with derivative computation, smoothing, and peak/threshold detection.

## Target System

### Frame Detection

Find a specific frame based on metric behavior:

| Method | Description |
|--------|-------------|
| `max_value` | Frame with maximum metric value |
| `first_peak_percent` | First peak ≥X% of max |
| `derivative_peak` | When d(metric)/dt is maximum |
| `threshold_crossing` | First sustained crossing |

### Score Methods

Compute quality score:

**Boom-dependent:** `peak_clarity`, `post_boom_sustain`, `composite`

**Boom-independent:** `dynamic_range`, `rise_time`, `smoothness`, `buildup_gradient`, `peak_dominance`

### Usage

```toml
[targets.boom]
type = "frame"
metric = "angular_causticness"
method = "max_value"

[targets.boom_quality]
type = "score"
metric = "angular_causticness"
method = "peak_clarity"
```

Evaluated post-simulation to produce `boom_frame` and quality scores in metadata.

## Batch Generation

Two-phase workflow filters poor configurations before expensive rendering:

**Phase 1 - Probe:** 1000 pendulums, reduced frames, physics-only. Fast rejection of configs that won't produce good booms.

**Phase 2 - Render:** Full simulation with GPU if probe passes.

```toml
[batch]
count = 100
base_config = "config/default.toml"

[probe]
enabled = true
pendulum_count = 1000

[filter]
min_uniformity = 0.9

[filter.targets.boom]
min_seconds = 8.0
max_seconds = 15.0
required = true
```

## Extending

### Add Metric

1. Add to `METRIC_REGISTRY` in `metric_registry.h`
2. Add constant to `MetricNames` in `metrics_collector.h`
3. Compute in `MetricsCollector::updateFromStates()`

### Add Post-Process Effect

1. Add enum in `config.h` → `ToneMapOperator`
2. Parse in `config.cpp`
3. Implement in `gl_renderer.cpp` shader

### Add Detection Method

1. Add enum to `prediction_target.h`
2. Parse in `parseFrameDetectionMethod()`
3. Implement in `frame_detector.h`
