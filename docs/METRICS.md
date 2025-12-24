# Metrics and Boom Detection System

This document describes the metrics system used for analyzing double pendulum simulations, including boom detection, parameter tuning, and optimization.

## Overview

The system computes various metrics from pendulum states to detect "boom" events (the moment when chaotic behavior produces visually interesting caustic patterns) and measure simulation quality.

## Available Metrics

### Causticness Metrics

These metrics measure how concentrated or structured the pendulum distribution is, peaking when caustic patterns form.

| Metric | Description | Best For |
|--------|-------------|----------|
| `angular_causticness` | Coverage × Gini on angle1+angle2 sectors | General purpose |
| `tip_causticness` | Same formula using atan2(x2, y2) tip angle | Position-aware detection |
| `cv_causticness` | Coefficient of variation on sector counts | **Best for boom detection** |
| `spatial_concentration` | 2D grid coverage × Gini on tip positions | Spatial clustering |
| `fold_causticness` | CV of adjacent-pair distances × spread | Fold/crossing detection |
| `local_coherence` | Neighbor distance vs random distance ratio | Structure detection |

### Other Metrics

| Metric | Description |
|--------|-------------|
| `variance` | Angular variance of pendulum states |
| `circular_spread` | 1 - mean resultant length (0=concentrated, 1=uniform) |
| `spread_ratio` | Fraction of pendulums above horizontal |
| `brightness` | Mean pixel intensity (GPU metric) |
| `coverage` | Fraction of non-zero pixels (GPU metric) |

## Boom Detection Methods

Three methods are available to detect the boom frame from a chosen metric's time series:

### 1. MaxCausticness (Recommended)

Finds the frame with maximum metric value, then applies an offset for visual alignment.

```toml
[boom_detection]
method = "max_causticness"
offset_seconds = 0.2    # Subtract this from peak time
```

**Pros:** Simple, robust, finds peak visual richness
**Cons:** Peak may be after the visual "boom" moment

### 2. FirstPeakPercent

Finds the first local peak that reaches a threshold percentage of the maximum.

```toml
[boom_detection]
method = "first_peak_percent"
peak_percent_threshold = 0.6    # 60% of max
```

**Pros:** Finds onset rather than peak
**Cons:** Sensitive to noise, may trigger too early

### 3. DerivativePeak

Finds the frame with maximum rate of change (steepest increase).

```toml
[boom_detection]
method = "derivative_peak"
smoothing_window = 5    # Frames to smooth before derivative
```

**Pros:** Finds transition moment
**Cons:** Requires smoothing, can be noisy

## Configuration

### TOML Config Structure

```toml
# Metric computation parameters
[metrics]
min_sectors = 8              # Min angular bins
max_sectors = 72             # Max angular bins
target_per_sector = 40       # Target pendulums per bin (N-scaling)
min_grid = 4                 # Min spatial grid size
max_grid = 32                # Max spatial grid size
target_per_cell = 40         # Target pendulums per cell
max_radius = 2.0             # Max tip radius (L1+L2)
cv_normalization = 1.5       # CV divisor for normalization
min_spread_threshold = 0.05  # Min spread for coherence metrics
gini_chaos_baseline = 0.35   # Gini noise floor in chaos
gini_baseline_divisor = 0.65 # Normalized Gini divisor

# Boom detection parameters
[boom_detection]
metric_name = "cv_causticness"    # Which metric to use
method = "max_causticness"        # Detection method
offset_seconds = 0.2              # Offset from peak (MaxCausticness)
peak_percent_threshold = 0.6      # Threshold (FirstPeakPercent)
smoothing_window = 5              # Window size (DerivativePeak)
min_peak_prominence = 0.05        # Min peak height vs neighbors
```

### Config File Includes

Configs can import other configs to share common parameters:

```toml
# my_simulation.toml
include = ["best_params.toml"]

[physics]
initial_angle1_deg = 175.0

[color]
scheme = "plasma"
```

Included files are loaded first; the main file's values override them.

## GUI Configuration

The GUI provides a "Metric Parameters" window (accessible via the gear icon in the Analysis panel) with controls for:

- **Sector Algorithm**: min/max sectors, target per sector
- **Grid Algorithm**: min/max grid, target per cell
- **Normalization**: max radius, CV norm, log ratio norm, min spread
- **Gini Baseline**: chaos baseline, divisor
- **Local Coherence**: log baseline, divisor
- **Boom Detection**: metric selector, method, method-specific parameters

Changes take effect after clicking "Recompute Now" or are applied automatically when running a new simulation.

## Optimization Tool

The `pendulum-optimize` tool finds optimal parameters for boom detection by comparing against ground-truth annotations.

### Usage

```bash
# Create annotations file with ground truth boom frames
cat > annotations.json << 'EOF'
{
  "version": 1,
  "annotations": [
    {"id": "run_001", "data_path": "output/run_001/simulation_data.bin", "boom_frame": 450},
    {"id": "run_002", "data_path": "output/run_002/simulation_data.bin", "boom_frame": 523}
  ]
}
EOF

# Run optimization
./pendulum-optimize annotations.json
```

### Two-Phase Architecture

The optimizer uses a two-phase approach for efficiency:

**Phase 1 - Compute Metrics (expensive, parallelized)**
- Loads all simulation data into memory
- Computes all causticness metrics for each (metric_config, simulation) pair
- Fully parallelized across all CPU cores
- Example: 336 metric configs × 30 sims = 10,080 work items

**Phase 2 - Search Boom Methods (cheap, instant)**
- Evaluates different boom detection configurations on pre-computed metrics
- Time: <0.01s for 96 configurations per metric config

This architecture means:
- Adding more boom detection methods is essentially free
- Adding more metric configs scales linearly with Phase 1 time
- All CPU cores stay fully utilized

### What It Searches

| Phase | Dimension | Values |
|-------|-----------|--------|
| Phase 1 | min_sectors | 2, 3, 4, 5, 6, 7, 8, 9 |
| Phase 1 | max_sectors | 16, 32, 48, 64, 80, 96 |
| Phase 1 | target_per_sector | 20, 30, 40, 50, 60, 70, 80 |
| Phase 2 | Metric | angular, tip, spatial, cv, fold, local_coherence |
| Phase 2 | Method | max, first_peak, derivative |
| Phase 2 | Method params | offset, threshold, smoothing |

### Output

```
=== Two-Phase Optimization ===
Phase 1: 336 metric configs × 30 simulations (expensive)
Phase 2: 96 boom detection methods (fast)
Total: 32256 parameter combinations
Threads: 32

Phase 1: Computing metrics (10080 work items)...
  Progress: 5000/10080 (49.6%) | 45.2s | 111 items/s | ETA: 46s
Phase 1 complete: 10080 items in 91.23s (110 items/s)

Phase 2: Evaluating 96 boom detection methods...
Phase 2 complete: 0.12s

Top 10 parameter combinations:
------------------------------------------------------------------------------------------
Rank    Boom MAE    Peak MAE     Score  Parameters
------------------------------------------------------------------------------------------
   1        28.5         N/A      28.5  cv first@80%
   2        29.1         N/A      29.1  cv max off=0.2
...
```

Best parameters are saved to `best_params.toml`.

### Current Best Configuration

Based on optimization against 30 annotated simulations:

```toml
[boom_detection]
metric_name = "cv_causticness"
method = "first_peak_percent"
peak_percent_threshold = 0.80
```

## Metric Computation Details

### N-Scaling

Metrics are designed to be independent of pendulum count (N). The system automatically adjusts:

- **Sector count**: `clamp(N / target_per_sector, min_sectors, max_sectors)`
- **Grid size**: `clamp(sqrt(N / target_per_cell), min_grid, max_grid)`

This ensures consistent behavior whether simulating 1,000 or 1,000,000 pendulums.

### Causticness Formula

The general causticness formula is:

```
causticness = coverage × adjusted_gini
```

Where:
- `coverage` = fraction of bins/sectors with at least one pendulum
- `adjusted_gini` = `(gini - chaos_baseline) / baseline_divisor`

This produces:
- Low values at start (all pendulums bunched together)
- Peak at caustic formation (spread out but structured)
- Low values in chaos (evenly distributed, low Gini)

### CV Causticness

The `cv_causticness` metric uses coefficient of variation instead of Gini:

```
cv = std(sector_counts) / mean(sector_counts)
cv_causticness = coverage × (cv / cv_normalization)
```

This is more sensitive to concentration differences and performs better for boom detection.

## Performance Considerations

### Metric Computation Speed

- Physics metrics: ~1ms per frame for 10,000 pendulums
- GPU metrics: Computed as part of rendering (essentially free)

### Optimization Speed

With pre-loaded simulation data:
- ~2-3 evaluations/second per thread
- 42 parameter combinations × 3 simulations ≈ 20 seconds on 32 cores

### Memory Usage

Each loaded simulation requires:
- Header: 144 bytes
- Per-pendulum-per-frame: 24 bytes (6 floats: x1, y1, x2, y2, th1, th2)
- Example: 1440 frames × 7500 pendulums = ~260MB compressed

## Files Reference

| File | Purpose |
|------|---------|
| `include/metrics/metrics_collector.h` | MetricsCollector class, metric computation |
| `include/metrics/boom_detection.h` | BoomDetector, findBoomFrame() |
| `include/config.h` | MetricParams, BoomDetectionParams structs |
| `src/config.cpp` | TOML parsing for metrics config |
| `src/main_optimize.cpp` | Parameter optimization tool |
| `config/best_params.toml` | Current best parameters |
| `config/gui.toml` | Default GUI config (includes best_params) |
