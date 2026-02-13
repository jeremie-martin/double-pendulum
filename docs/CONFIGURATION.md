# Configuration Reference

## Config File Structure

```toml
include = "other.toml"        # Include and override

[physics]
gravity = 9.81
length1 = 1.0
length2 = 1.0
mass1 = 1.0
mass2 = 1.0
initial_angle1_deg = 152.2
initial_angle2_deg = 152.0
angle_variation_deg = 0.1

[simulation]
pendulum_count = 100000
duration = 30.0
frames = 900
physics_quality = "high"

[render]
width = 1920
height = 1080

[post_process]
tone_map = "ACES"
exposure = 0.0
contrast = 1.0
gamma = 1.0

[color]
scheme = "plasma"

[output]
directory = "output"
format = "mp4"
```

## Key Parameters

### Physics

| Parameter | Range | Effect |
|-----------|-------|--------|
| `initial_angle1/2_deg` | [-180, 180] | Starting angles |
| `angle_variation_deg` | [0.001, 0.2] | Spread across pendulums |
| `initial_velocity1/2` | Any | Angular velocity (rad/s) |

### Simulation

| Parameter | Description |
|-----------|-------------|
| `pendulum_count` | Number of pendulums (1K-1M+) |
| `duration` | Simulation time in seconds |
| `frames` | Total frames to render |
| `physics_quality` | low/medium/high/ultra |

### Post-Processing

| Parameter | Description |
|-----------|-------------|
| `tone_map` | none, reinhard, ACES, filmic, etc. |
| `exposure` | Stops adjustment |
| `contrast` | Mid-tone contrast (0-2+) |
| `gamma` | Display gamma |

### Color Schemes

30+ schemes: `plasma`, `viridis`, `inferno`, `magma`, `cividis`, `turbo`, `jet`, `rainbow`, etc.

## CLI Usage

```bash
# Basic
./pendulum config.toml

# Override parameters
./pendulum config.toml --set simulation.pendulum_count=50000

# Multiple overrides
./pendulum config.toml \
  --set simulation.pendulum_count=100000 \
  --set color.scheme=inferno \
  --set post_process.exposure=0.5

# Save raw data for later analysis
./pendulum config.toml --save-data

# Extended statistics
./pendulum config.toml --analysis
```

## Targets

Define frame detection and quality scoring:

```toml
[targets.boom]
type = "frame"
metric = "angular_causticness"
method = "max_value"
offset_seconds = 0.0

[targets.chaos]
type = "frame"
metric = "variance"
method = "threshold_crossing"
crossing_threshold = 0.8
crossing_confirmation = 10

[targets.boom_quality]
type = "score"
metric = "angular_causticness"
method = "peak_clarity"
```

## Batch Configuration

```toml
[batch]
count = 100                    # Videos to generate
base_config = "config/default.toml"

[probe]
enabled = true
pendulum_count = 1000          # Fast probe with fewer pendulums
max_retries = 10

[filter]
min_uniformity = 0.9

[filter.targets.boom]
min_seconds = 8.0              # Boom must be after 8s
max_seconds = 15.0             # Boom must be before 15s
required = true                # Reject if no boom detected

[filter.targets.boom_quality]
min_score = 0.6

[physics_ranges]
initial_angle1_deg = [140.0, 200.0]  # Random range
initial_angle2_deg = [140.0, 200.0]
```

## Metric Parameters

Per-metric tuning:

```toml
[metrics.angular_causticness]
min_sectors = 8
max_sectors = 72
target_per_sector = 40

[metrics.local_coherence]
max_radius = 2.0
```

## Presets

Bundle color + post_process:

```toml
[preset.theme]
name = "ember_cinematic"
# Or define inline:
color = {scheme = "inferno"}
post_process = {tone_map = "filmic", exposure = 0.3}
```

---

# CLI Tools Reference

## pendulum-metrics

Recompute metrics from saved data:

```bash
./pendulum-metrics output/run_xxx/simulation_data.bin

# With GPU re-rendering
./pendulum-metrics output/run_xxx/simulation_data.bin --render

# Validate against saved metrics
./pendulum-metrics output/run_xxx/simulation_data.bin --validate
```

## pendulum-stability

Analyze metric stability across pendulum counts:

```bash
./pendulum-stability --counts 500,1000,2000,5000
```

---

# Python Tools

```bash
uv sync  # Install
```

## Commands

```bash
# Add music synced to boom
pendulum-tools music add /path/to/video

# Process with effects
pendulum-tools process /path/to/video --shorts --blur-bg

# Upload to YouTube
pendulum-tools upload /path/to/video --privacy unlisted

# Batch operations
pendulum-tools batch /path/to/output --limit 10
```

## User Config

`~/.config/pendulum-tools/config.toml`:

```toml
music_dir = "/path/to/music"
credentials_dir = "/path/to/credentials"
use_nvenc = true
nvenc_cq = 23
```
