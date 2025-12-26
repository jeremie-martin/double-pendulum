# Claude Context for Double Pendulum

## Project Overview

C++20 double pendulum physics simulation with GPU-accelerated rendering. Simulates millions of pendulums with slightly different initial conditions to visualize chaotic divergence.

## Architecture

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│   Physics   │────▶│  GLRenderer  │────▶│ VideoWriter │
│  (CPU, MT)  │     │ (GPU, GLSL)  │     │  (FFmpeg)   │
└─────────────┘     └──────────────┘     └─────────────┘
      │                    │
      ▼                    ▼
┌─────────────┐     ┌──────────────┐
│  Metrics    │◀────│    Post-     │
│  Collector  │     │  Processing  │
└─────────────┘     │   (Shader)   │
      │             └──────────────┘
      ▼
┌─────────────┐     ┌──────────────┐
│   Target    │────▶│   Analyzers  │
│  Evaluator  │     │ (Signal,etc) │
└─────────────┘     └──────────────┘
```

## Key Files

| File | Purpose |
|------|---------|
| `src/simulation.cpp` | Main simulation loop, coordinates physics + rendering |
| `src/batch_generator.cpp` | Batch video generation with probe filtering |
| `src/main_gui.cpp` | GUI application with real-time preview and analysis |
| `src/main_metrics.cpp` | Metric iteration tool for recomputing metrics |
| `src/main_optimize.cpp` | Metric parameter optimization via grid search |
| `src/main_stability.cpp` | Metric stability analysis across pendulum counts |
| `src/simulation_data.cpp` | ZSTD-compressed simulation data I/O |
| `src/gl_renderer.cpp` | GPU line rendering with GLSL shaders |
| `include/pendulum.h` | RK4 physics integration (Lagrangian mechanics) |
| `include/config.h` | TOML config parsing, all parameters |
| `include/simulation.h` | Simulation class with run() and runProbe() |
| `include/batch_generator.h` | Batch config, filter criteria, target constraints |

## Metrics System

The metrics system in `include/metrics/` provides time-series tracking and analysis:

| File | Purpose |
|------|---------|
| `metrics_collector.h` | Central hub for all metrics (physics + GPU) |
| `metric_series.h` | Generic time series with derivative tracking |
| `signal_analyzer.h` | Generic signal analysis (peak clarity, post-reference area) |
| `event_detector.h` | Threshold-based event detection (chaos only) |
| `boom_detection.h` | Wrapper for finding boom frame via FrameDetector |
| `probe_filter.h` | Pass/fail decision logic for filtering |
| `probe_pipeline.h` | Multi-phase probe system |
| `analyzer.h` | Base class for quality analyzers |

### Available Metrics

**Physics metrics** (computed from pendulum state):
- `variance` - Angular variance (stable, CV <1%)
- `circular_spread` - Distribution uniformity 0=concentrated, 1=uniform (stable)
- `spread_ratio` - Fraction above horizontal (legacy)
- `angular_range` - Normalized angular coverage
- `total_energy` - Total mechanical energy

**Causticness metrics** (sector-based, less stable but good for detection):
- `angular_causticness` - coverage × density concentration on angle1+angle2
- `tip_causticness` - causticness using geometric tip angle atan2(x2,y2)
- `cv_causticness` - CV-based instead of Gini
- `organization_causticness` - (1-R1*R2) × coverage
- `local_coherence` - neighbor distance vs random distance
- `r1_concentration`, `r2_concentration`, `joint_concentration` - per-arm metrics

**Velocity metrics** (for boom detection via opposing motion):
- `velocity_dispersion` - spread of velocity directions (0=same, 1=uniform)
- `speed_variance` - normalized variance of tip speeds
- `velocity_bimodality` - detects two groups moving opposite directions
- `angular_momentum_spread` - spread of angular momenta
- `acceleration_dispersion` - spread of tip accelerations

**GPU metrics** (from rendered frame):
- `brightness` - mean pixel intensity (0-1)
- `coverage` - fraction of non-zero pixels (0-1)
- `max_value` - peak pixel intensity before post-processing

## Prediction System

The multi-target prediction system in `include/optimize/` provides flexible frame detection and quality scoring:

| File | Purpose |
|------|---------|
| `prediction_target.h` | Target definitions, detection methods, score methods |
| `frame_detector.h` | Generic frame detection from any metric |
| `score_predictor.h` | Score prediction with multiple methods |
| `target_evaluator.h` | Multi-target orchestration |

### Frame Detection Methods

| Method | Description |
|--------|-------------|
| `max_value` | Frame with maximum metric value (default for boom) |
| `first_peak_percent` | First peak >= X% of max |
| `derivative_peak` | When d(metric)/dt is maximum |
| `threshold_crossing` | First sustained crossing of threshold |
| `second_derivative_peak` | When acceleration is maximum |
| `constant_frame` | Always returns configured frame (testing) |

### Score Methods

**Boom-dependent** (require reference frame):
- `peak_clarity` - main / (main + max_competitor)
- `post_boom_sustain` - normalized area after reference
- `composite` - weighted combination

**Boom-independent** (analyze full signal):
- `dynamic_range` - (max - min) / max
- `rise_time` - peak_frame / total (inverted: early = high)
- `smoothness` - 1 / (1 + mean_abs_second_deriv)
- `buildup_gradient` - average slope to peak
- `peak_dominance` - peak / mean ratio
- `decay_rate` - how quickly signal drops after peak
- `median_dominance` - peak / median ratio
- `tail_weight` - mean / median ratio (skewness)

**Boom-relative** (properties around reference frame):
- `pre_boom_contrast` - 1 - (avg_before / peak)
- `boom_steepness` - derivative_at_boom / max_derivative

### Usage Example

Targets are configured in TOML and evaluated after simulation:

```cpp
#include "optimize/target_evaluator.h"
#include "optimize/prediction_target.h"

// Build targets from config
std::vector<optimize::PredictionTarget> targets;
for (auto const& tc : config.targets) {
    targets.push_back(optimize::targetConfigToPredictionTarget(
        tc.name, tc.type, tc.metric, tc.method,
        tc.offset_seconds, tc.peak_percent_threshold,
        tc.min_peak_prominence, tc.smoothing_window,
        tc.crossing_threshold, tc.crossing_confirmation,
        tc.weights));
}

// Evaluate all targets
optimize::TargetEvaluator evaluator;
evaluator.setTargets(targets);
auto predictions = evaluator.evaluate(collector, frame_duration);

// Access results
auto boom_frame = optimize::TargetEvaluator::getBoomFrame(predictions);
auto boom_quality = optimize::TargetEvaluator::getBoomQuality(predictions);
```

## Configuration

### Target Configuration (TOML)

Define prediction targets in config files:

```toml
# Frame target for boom detection
[targets.boom]
type = "frame"
metric = "angular_causticness"
method = "max_value"
offset_seconds = 0.0

# Frame target for chaos detection
[targets.chaos]
type = "frame"
metric = "variance"
method = "threshold_crossing"
crossing_threshold = 0.8
crossing_confirmation = 10

# Score target for quality
[targets.boom_quality]
type = "score"
metric = "angular_causticness"
method = "peak_clarity"
```

### Per-Metric Configuration

Metrics with configurable parameters can be tuned:

```toml
# Sector-based metrics (angular_causticness, tip_causticness, etc.)
[metrics.angular_causticness]
min_sectors = 8
max_sectors = 72
target_per_sector = 40

# CV-based sector metrics
[metrics.cv_causticness]
min_sectors = 8
max_sectors = 72
target_per_sector = 40
cv_normalization = 1.5

# Local coherence
[metrics.local_coherence]
max_radius = 2.0
min_spread_threshold = 0.05
log_inverse_baseline = 1.0
log_inverse_divisor = 2.5
```

### Physics Quality

| Quality | max_dt | Steps/period | Description |
|---------|--------|--------------|-------------|
| low     | 20ms   | ~100         | Visible artifacts, fast |
| medium  | 12ms   | ~167         | Acceptable quality |
| high    | 7ms    | ~286         | Gold standard (default) |
| ultra   | 3ms    | ~667         | Perfect accuracy |

Substeps computed automatically: `substeps = max(1, ceil(frame_dt / max_dt))`

## Batch Generation

### Two-Phase Workflow

**Phase 1 - Probe**: Fast physics-only simulation with fewer pendulums to evaluate parameters.

**Phase 2 - Render**: Full simulation with GPU rendering if probe passes.

### Filter Criteria

```toml
[filter]
min_uniformity = 0.9     # Minimum distribution uniformity

# Target-based constraints
[filter.targets.boom]
min_seconds = 8.0        # Boom must happen after this time
max_seconds = 15.0       # Boom must happen before this time
required = true          # Reject if no boom detected

[filter.targets.boom_quality]
min_score = 0.6          # Minimum quality score
```

### Example Batch Config

```toml
[batch]
count = 100
base_config = "config/default.toml"

[probe]
enabled = true
pendulum_count = 1000
max_retries = 10

[filter]
min_uniformity = 0.9

[filter.targets.boom]
min_seconds = 8.0
max_seconds = 15.0
required = true

[physics_ranges]
initial_angle1_deg = [140.0, 200.0]
initial_angle2_deg = [140.0, 200.0]
```

## Common Modifications

### Add new tone mapping operator
1. Add enum in `include/config.h` → `ToneMapOperator`
2. Add parsing in `src/config.cpp`
3. Add GLSL implementation in `src/gl_renderer.cpp` → `pp_fragment_shader_src`
4. Add CPU reference in `include/post_process.h` → `toneMap()`

### Add new color scheme
1. Add enum in `include/config.h` → `ColorScheme`
2. Add parsing in `src/config.cpp`
3. Add implementation in `include/color_scheme.h` → `ColorSchemeGenerator`

### Add new metric
1. Add constant in `metrics/metrics_collector.h` → `MetricNames`
2. Register in `MetricsCollector::registerStandardMetrics()`
3. Compute in appropriate update method (`updateFromStates()`, etc.)

### Add new score method
1. Add enum in `optimize/prediction_target.h` → `ScoreMethod`
2. Add parsing in `parseScoreMethod()`
3. Implement in `optimize/score_predictor.h` → `ScorePredictor`

### Add new filter criterion
**For target-based constraints:**
1. Add field to `TargetConstraint` in `include/batch_generator.h`
2. Add TOML parsing in `BatchConfig::load()`
3. Add evaluation in `metrics::ProbeFilter::evaluate()`

**For general filters:**
1. Add field to `FilterCriteria` in `include/batch_generator.h`
2. Add to `FilterCriteria::toProbeFilter()`
3. Add TOML parsing in `BatchConfig::load()`

## Build Commands

```bash
# CLI only
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# With GUI
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON && cmake --build build -j
```

## Metric Iteration Workflow

Save raw simulation data and recompute metrics without re-running physics:

```bash
# Save data during simulation
./pendulum config.toml --save-data

# Recompute physics metrics only
./pendulum-metrics output/run_xxx/simulation_data.bin

# With GPU re-rendering
./pendulum-metrics output/run_xxx/simulation_data.bin --render
```

Data format: 144-byte header + ZSTD-compressed payload (24 bytes per pendulum per frame).

## Metric Stability

Stability analysis validates that low-N probes can predict full simulation results:

**Stable (CV <1%):** `variance`, `circular_spread`, `angular_range`, `spread_ratio`

**Unstable absolute values (CV 20-50%):** All causticness metrics

**Key insight:** Boom detection remains stable (stddev <3 frames) because detection analyzes curve shape, not absolute values.

## GUI Application

The GUI (`pendulum-gui`) provides:
- Real-time preview with adjustable physics quality
- Multi-axis metric plotting with derivatives
- Quality scores from SignalAnalyzer

## Dependencies

Required: OpenGL 3.3+, GLEW, EGL, libpng, pthreads, zstd
Optional: SDL2 (GUI), FFmpeg (video output)

## Pendulum Tools (Python)

Post-processing and uploading tools managed with `uv`:

```bash
uv sync
```

### Commands
```bash
pendulum-tools music add /path/to/video     # Add music → video.mp4
pendulum-tools process /path/to/video       # Apply effects → video_processed.mp4
pendulum-tools upload /path/to/video        # Upload to YouTube
```

### Key Files
| File | Purpose |
|------|---------|
| `src/pendulum_tools/cli.py` | Click CLI commands |
| `src/pendulum_tools/models.py` | Pydantic models for metadata.json |
| `src/pendulum_tools/config.py` | User config file support |
| `src/pendulum_tools/constants.py` | Centralized magic numbers |
| `src/pendulum_tools/exceptions.py` | Custom exceptions with FFmpeg error extraction |
| `src/pendulum_tools/music/manager.py` | Music selection and FFmpeg muxing |
| `src/pendulum_tools/processing/pipeline.py` | Video effects pipeline |

### User Config

Optional config file at `~/.config/pendulum-tools/config.toml`:
```toml
music_dir = "/path/to/music"
credentials_dir = "/path/to/credentials"
use_nvenc = true
nvenc_cq = 23
```

CLI flags override config file values.

### Music Sync

Music drops are aligned with visual booms:
1. Simulation outputs `boom_seconds` in metadata.json
2. `pendulum-tools music add` selects tracks where `drop_time > boom_seconds`
3. Audio is offset so drop aligns with boom frame
