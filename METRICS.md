# Metrics System Documentation

This document explains the metrics and analysis system used to evaluate double pendulum simulations.

## Overview

The simulation tracks several metrics over time and uses analyzers to compute quality scores for filtering and ranking simulations.

## Key Metrics

### Angular Causticness
**Primary metric for boom detection and quality scoring.**

Measures how "bunched up" the pendulum angles are - high causticness indicates pendulums are converging to similar angles, creating visual caustic patterns (bright concentrated regions).

- Range: 0-1 (typically peaks around 0.3-0.8 for good simulations)
- Computed from: Angular distribution concentration
- Used by: Boom detection, CausticnessAnalyzer

### Variance
**Legacy metric, still tracked but less central.**

Measures spread of pendulum positions. Originally used for boom detection via threshold crossing.

- Range: 0+ (unbounded, depends on pendulum count)
- Computed from: Position variance across all pendulums
- Used by: BoomAnalyzer (legacy), chaos detection

### Uniformity (Circular Spread)
**Distribution quality metric.**

Measures how uniformly distributed the pendulums are around the circle. Value of 1 means perfectly uniform distribution on disk.

- Range: 0-1 (1 = uniform, 0 = concentrated)
- Target: ≥0.9 for good simulations
- Used by: Filter criteria in batch generation

## Boom Detection

**Current method**: Find the frame with maximum angular causticness, offset by 0.3 seconds for better visual alignment.

```cpp
auto boom = metrics::findBoomFrame(collector, frame_duration);
// boom.frame, boom.seconds, boom.causticness
```

**Legacy method** (deprecated): Variance threshold crossing with derivative confirmation. No longer used for boom detection but the EventDetector still supports it for chaos detection.

## Analyzers

### CausticnessAnalyzer
**Primary quality analyzer.** Computes:

| Metric | Description |
|--------|-------------|
| `peak_causticness` | Maximum causticness value (0-1) |
| `peak_frame/seconds` | When the peak occurred |
| `peak_clarity_score` | 0.5-1.0, higher means no competing peaks before main |
| `post_boom_area_normalized` | 0-1, how well causticness sustains after peak |
| `competing_peaks_count` | Number of peaks before the main peak |

**Quality Score Formula:**
```
score = clarity * 0.4 + peak * 0.35 + sustain * 0.25
```

### Peak Detection (findPeaks)

Uses prominence-based filtering to ignore noise:

1. Find local maxima (higher than both neighbors)
2. Filter by minimum height (≥10% of global max)
3. Filter by minimum prominence (≥5% of global max)
4. Enforce minimum separation (0.3s between peaks)

**Prominence** = height above surrounding terrain. A peak on a declining slope has low prominence and gets filtered out.

### BoomAnalyzer (Legacy)
**Variance-based analysis around boom frame.** Currently produces low/meaningless scores because:

- Analyzes variance derivatives, not causticness
- Relies on threshold crossing detection (not used for causticness-based boom)
- Kept for backward compatibility but output removed from CLI

## Filter Criteria

For batch generation, simulations can be filtered by:

| Criterion | Description |
|-----------|-------------|
| `min_boom_seconds` | Boom must occur after this time |
| `max_boom_seconds` | Boom must occur before this time |
| `min_uniformity` | Minimum final uniformity (0.9 recommended) |
| `min_peak_clarity` | Minimum peak clarity (0.75 recommended) |
| `require_boom` | Reject if no boom detected |

## Output Fields

### Console Output
```
Boom:        6.65s (frame 266, causticness=0.5776)
Uniformity:  0.99 (target: 0.9)
Causticness: 0.55 (peak=0.58, avg=0.16, clarity=0.65)
```

### metadata.json
```json
{
  "results": {
    "boom_frame": 266,
    "boom_seconds": 6.65,
    "boom_causticness": 0.5776,
    "final_uniformity": 0.99
  },
  "scores": {
    "causticness": 0.55,
    "peak_clarity": 0.65,
    "post_boom_sustain": 0.36
  }
}
```

### metrics.csv
Frame-by-frame metrics:
```
frame,variance,circular_spread,spread_ratio,angular_range,angular_causticness,brightness,coverage,total_energy
```

Note: `circular_spread` is the uniformity metric (0=concentrated, 1=uniform).

## Notes

- **BoomAnalyzer class files**: Remain in codebase but are no longer used. Can be deleted.
- **EventDetector threshold detection**: Still used for chaos detection, not boom.
- **variance_at_boom in forceBoomEvent**: Passed for event logging compatibility.
