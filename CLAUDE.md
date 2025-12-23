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
┌─────────────┐
│  Analyzers  │
│ (Boom,etc.) │
└─────────────┘
```

## Key Files

| File | Purpose |
|------|---------|
| `src/simulation.cpp` | Main simulation loop, coordinates physics + rendering |
| `src/batch_generator.cpp` | Batch video generation with probe filtering |
| `src/gl_renderer.cpp` | GPU line rendering with GLSL shaders |
| `src/headless_gl.cpp` | EGL context for headless GPU rendering |
| `include/pendulum.h` | RK4 physics integration (Lagrangian mechanics) |
| `include/config.h` | TOML config parsing, all parameters |
| `include/batch_generator.h` | Batch config, filter criteria, probe filter |
| `include/metrics/` | Unified metrics system (see Metrics System below) |

## Rendering Pipeline

1. **Line Drawing**: Pendulum arms → GPU quads with smoothstep AA
2. **Accumulation**: Additive blending to RGBA32F texture
3. **Post-Processing** (GPU shader):
   - Normalize by max value
   - Apply exposure (2^stops)
   - Tone mapping (Reinhard/ACES/Logarithmic)
   - Contrast adjustment
   - Gamma correction
4. **Output**: Read back RGB8 → PNG or FFmpeg

## Metrics System

The unified metrics system in `include/metrics/` replaces the legacy variance/analysis trackers:

| File | Purpose |
|------|---------|
| `metric_series.h` | Generic time series with derivative tracking, smoothing |
| `metrics_collector.h` | Central hub for all metrics (physics + GPU) |
| `event_detector.h` | Configurable event detection (boom, chaos) |
| `analyzer.h` | Base class for pluggable quality analyzers |
| `boom_analyzer.h` | Boom quality scoring (sharpness, type) |
| `causticness_analyzer.h` | Causticness evolution scoring |
| `probe_filter.h` | Pass/fail decision logic for filtering |
| `probe_pipeline.h` | Multi-phase probe system |

### Key Concepts

- **Metric**: Raw time-series measurement (variance, brightness, edge_energy)
- **Event**: Detected threshold crossing with confirmation (boom, chaos)
- **Analyzer**: Component that computes quality scores from metrics
- **Score**: Quality assessment for ranking/filtering (SimulationScore)

### Usage Example

```cpp
metrics::MetricsCollector collector;
collector.registerStandardMetrics();

metrics::EventDetector detector;
detector.addBoomCriteria(0.1, 10, metrics::MetricNames::Variance);

// In simulation loop:
collector.beginFrame(frame);
collector.updateFromPendulums(pendulums);
collector.setGPUMetrics(gpu_bundle);
collector.endFrame();
detector.update(collector, frame_duration);
```

## Common Modifications

### Add new tone mapping operator
1. Add enum value in `include/config.h` → `ToneMapOperator`
2. Add parsing in `src/config.cpp` → `parseToneMapOperator()`
3. Add GLSL implementation in `src/gl_renderer.cpp` → `pp_fragment_shader_src`
4. Add CPU reference in `include/post_process.h` → `toneMap()`

### Add new color scheme
1. Add enum in `include/config.h` → `ColorScheme`
2. Add parsing in `src/config.cpp`
3. Add implementation in `include/color_scheme.h` → `ColorSchemeGenerator`

### Modify physics
- All physics in `include/pendulum.h` → `Pendulum::step()`
- Uses RK4 integration with Lagrangian equations of motion
- Thread-safe (each pendulum is independent)

### Add new filter criterion for batch probing
1. Add field to `FilterCriteria` struct in `include/batch_generator.h`
2. Add criterion to `FilterCriteria::toProbeFilter()` method
3. Add check in `metrics::ProbeFilter::passes()` method in `include/metrics/probe_filter.h`
4. Add TOML parsing in `BatchConfig::load()` in `src/batch_generator.cpp`

### Add new analysis metric
1. Add field to `SpreadMetrics` struct in `include/metrics/metrics_collector.h`
2. Compute in `MetricsCollector::computeSpread()` method
3. Register metric in `MetricsCollector::registerStandardMetrics()`
4. Access via `MetricsCollector::getMetric()` or `getSpreadHistory()`

### Add new analyzer for quality scoring
1. Create new analyzer class inheriting from `metrics::Analyzer` in `include/metrics/`
2. Implement `name()`, `analyze()`, `score()`, `toJSON()`, `reset()`, `hasResults()`
3. Add to simulation or batch generator as needed

### Physics Quality System
The simulation uses a `physics_quality` setting that maps to maximum timestep (`max_dt`):

| Quality | max_dt | Steps/period | Description |
|---------|--------|--------------|-------------|
| low     | 20ms   | ~100         | Visible artifacts, fast |
| medium  | 12ms   | ~167         | Acceptable quality |
| high    | 7ms    | ~286         | Gold standard (default) |
| ultra   | 3ms    | ~667         | Overkill, perfect accuracy |

The system automatically computes substeps per frame to maintain constant quality:
```cpp
substeps = max(1, ceil(frame_dt / max_dt))
```

This ensures consistent simulation quality regardless of frame count or duration. You can also specify `max_dt` directly in the config for fine control.

### Batch Generation with Probe Filtering

The batch system supports a two-phase workflow for generating videos with quality filtering:

**Phase 1 - Probe**: Run a fast physics-only simulation (no GPU, no rendering) with fewer pendulums (e.g., 1000) to evaluate if parameters produce a good animation.

**Phase 2 - Render**: Only if the probe passes all filter criteria, run the full simulation with GPU rendering.

#### Filter Criteria

| Criterion | Purpose |
|-----------|---------|
| `min_boom_seconds` | Boom must happen after this time |
| `max_boom_seconds` | Boom must happen before this time |
| `min_spread_ratio` | Minimum fraction of pendulums above horizontal |
| `require_boom` | Reject simulations with no detectable boom |
| `require_valid_music` | Fail if no music track has drop > boom time |

#### Spread Metrics

The `MetricsCollector` computes spread metrics every frame:
- `spread_ratio`: Fraction of pendulums with angle1 in [-π/2, π/2] (above horizontal)
- `circular_spread`: 1 - mean resultant length (0=concentrated, 1=uniform)
- `angle1_mean`, `angle1_variance`: For debugging/analysis

Spread is tracked and output in multiple places:
- **variance.csv**: `frame,variance,max_value,spread_ratio` columns
- **metadata.json**: `final_spread_ratio` in results section
- **stdout**: Printed at end of simulation ("Spread: X% above horizontal")
- **GUI**: Real-time display in Analysis panel, graphable in metric selector
- **Batch summary**: Spread column in completion table

#### Color Presets

Define curated color combinations in batch configs:
```toml
[[color_presets]]
scheme = "spectrum"
start = 0.2
end = 0.8

[[color_presets]]
scheme = "heat"
start = 0.0
end = 0.7
```

In random batch mode, one preset is randomly selected per video.

#### Example Batch Config with Probing

```toml
[batch]
count = 100
base_config = "config/default.toml"

[probe]
enabled = true
pendulum_count = 1000    # Fast probing
max_retries = 10         # Retry with new params if rejected

[filter]
min_boom_seconds = 8.0   # Boom between 8-15 seconds
max_boom_seconds = 15.0
min_spread_ratio = 0.3   # At least 30% above horizontal

[physics_ranges]
initial_angle1_deg = [140.0, 200.0]
initial_angle2_deg = [140.0, 200.0]
```

## Build Commands

```bash
# CLI only
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# With GUI
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON && cmake --build build -j
```

## Dependencies

Required: OpenGL 3.3+, GLEW, EGL, libpng, pthreads
Optional: SDL2 (GUI), FFmpeg (video output)

## Code Style

- C++20 with `auto`, structured bindings, `std::optional`
- Header-only where sensible (pendulum.h, post_process.h)
- TOML for config, JSON for metadata/progress
- No exceptions in hot paths, use return values

## Performance Notes

- Physics is CPU-bound (RK4 per pendulum per substep)
- Rendering is GPU-bound (millions of line quads)
- I/O can dominate for PNG output (use video format)
- 1M pendulums @ 4K: ~5s/frame (RTX 4090)

## YouTube Uploader (Python)

A standalone Python tool for uploading videos to YouTube with auto-generated metadata.

### Location
`youtube-uploader/` - Python project managed with `uv`

### Setup
```bash
cd youtube-uploader
uv sync
# Place OAuth credentials in credentials/client_secrets.json
```

### Commands
```bash
# Preview generated metadata
uv run pendulum-upload preview /path/to/video_dir

# Upload single video
uv run pendulum-upload upload /path/to/video_dir --privacy unlisted

# Upload entire batch
uv run pendulum-upload batch /path/to/batch_output --limit 10

# Dry run (no upload)
uv run pendulum-upload upload /path/to/video_dir --dry-run
```

### Key Files
| File | Purpose |
|------|---------|
| `src/pendulum_uploader/cli.py` | Click CLI commands |
| `src/pendulum_uploader/uploader.py` | YouTube API integration |
| `src/pendulum_uploader/templates.py` | Title/description/tag generation |
| `src/pendulum_uploader/models.py` | Pydantic models for metadata.json |

### Music Selection with Boom Timing

The batch generator now ensures that music drops happen AFTER the visual boom:

1. After rendering, `boom_seconds` is calculated
2. `pickMusicTrackForBoom(boom_seconds)` filters tracks where `drop_time > boom_seconds`
3. If no valid track and `require_valid_music = true`, video fails and retries with new parameters

This ensures the visual climax always syncs with (or leads) the music drop.
