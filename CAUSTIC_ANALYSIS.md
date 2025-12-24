# Caustic Metric Analysis

## What We Want to Capture

A good caustic metric must have this temporal profile:
```
       BOOM
        ↓
       /\
      /  \
     /    \____
____/          \____
START          CHAOS
```

**Key insight**: Caustics are about **LOCAL STRUCTURE within GLOBAL SPREAD**.

- At START: No spread, no structure to see
- At CAUSTIC (boom): Global spread + local coherence (points cluster along curves)
- At CHAOS: Global spread + no local coherence (points randomly distributed)

## Why Current Metrics Fail

| Metric | Problem | Why it fails |
|--------|---------|--------------|
| `organization` | Converges to 1 | Measures "spread" not "structure". Chaos is maximally spread. |
| `spatial_concentration` | Delayed, slow decay | 2D histogram too coarse. Caustic lines smear across cells. |
| `fold` | Stays high in chaos | CV of distances doesn't distinguish structured vs random variability |
| `gini × coverage` | Works OK but noisy | Gini on angular sectors misses spatial structure |

## The Fundamental Problem

All current metrics measure **global statistics** (mean, variance, Gini of the whole distribution).

Caustics are a **local phenomenon**: neighboring trajectories (in parameter space) cluster together (in physical space) along curves.

## New Paradigm: Local Coherence

The key distinguishing property:

- **Caustic**: Index neighbors (i, i+1) → spatial neighbors (close in x,y)
- **Chaos**: Index neighbors (i, i+1) → random positions (not correlated)

This is testable! We can measure how "predictable" position[i+1] is from position[i].

## Proposed New Metrics

### 1. Trajectory Smoothness (prediction error)

```cpp
// For each triplet (i-1, i, i+1):
// Predict position[i+1] by extrapolating from (i-1, i)
// Measure prediction error

predicted = 2 * pos[i] - pos[i-1]  // Linear extrapolation
error = |pos[i+1] - predicted|

smoothness = 1 / (1 + mean_error)  // High when curves are smooth
```

- START: Very high (all same position, zero error)
- CAUSTIC: High (smooth curves with folds)
- CHAOS: Low (can't predict next position)

**Problem**: Also high at start. Need to combine with spread.

**Solution**: `smoothness × spread × (1 - smoothness_at_start)`

### 2. Local Neighbor Correlation

```cpp
// Measure spatial correlation between index-neighbors
// Compare distance(i, i+1) to distance(i, random_j)

local_dist = mean(|pos[i+1] - pos[i]|)
random_dist = mean(|pos[i] - pos[random]|)

correlation = 1 - local_dist / random_dist
```

- START: 0 (all same position, both distances are 0)
- CAUSTIC: Positive (neighbors closer than random)
- CHAOS: ~0 (neighbors same as random)

### 3. Fold Detection (crossing trajectories)

A true fold = adjacent pendulums at same position but different angles.

```cpp
// Count "fold events":
// pos[i] ≈ pos[i+k] for small k, but angle[i] ≠ angle[i+k]

fold_count = 0
for each pair (i, i+k) where k in [2, 10]:
    if |pos[i] - pos[i+k]| < threshold:
        fold_count++

fold_density = fold_count / N
```

This directly detects where the parameter→position curve crosses itself!

### 4. Entropy Rate (temporal derivative)

Instead of entropy itself, measure how fast entropy is changing:

```cpp
entropy[t] = -Σ p_i * log(p_i)  // Shannon entropy of position distribution
entropy_rate = d(entropy)/dt

// Boom = maximum entropy rate (fastest increase in disorder)
```

The RATE peaks at the boom because:
- Before boom: slow spread
- At boom: rapid divergence
- After boom: already spread, slowing down

### 5. Multi-Scale Variance Ratio

Caustics have structure at fine scales. Chaos is uniform at all scales.

```cpp
// Compute position density at two grid resolutions
fine_var = variance(fine_grid_counts)
coarse_var = variance(coarse_grid_counts)

// Normalize by expected variance for uniform distribution
fine_ratio = fine_var / expected_fine_var
coarse_ratio = coarse_var / expected_coarse_var

structure = fine_ratio / coarse_ratio
```

High ratio = more fine-scale structure than coarse = caustic pattern.

### 6. Curvature-Based (the "folding" manifold)

The mapping θ → (x,y) forms a parametric curve. Caustics = high curvature points.

```cpp
// For triplet (i-1, i, i+1), compute curvature
v1 = pos[i] - pos[i-1]
v2 = pos[i+1] - pos[i]

// Curvature ∝ |v1 × v2| / |v1||v2|
curvature[i] = cross(v1, v2) / (|v1| * |v2|)

// High curvature = sharp turn = fold region
mean_curvature = mean(|curvature|)
max_curvature = max(|curvature|)
```

## Recommended Implementation Priority

1. **Trajectory smoothness + spread** - Simple, directly tests local coherence
2. **Entropy rate** - Temporal derivative naturally peaks at boom
3. **Fold detection** - Directly captures the physics of caustics
4. **Curvature** - Elegant but may be noisy

## Beyond Frame-by-Frame

Current metrics compute one value per frame. But we could:

1. **Windowed metrics**: Compare frame t to frame t-k
2. **Temporal smoothing**: Moving average to reduce noise
3. **Rate-of-change metrics**: Peak of derivative more reliable than peak of value
4. **Memory metrics**: Track cumulative properties

## The "Perfect" Caustic Metric (Theoretical)

If we had unlimited compute, the ideal metric would be:

```
causticness = (local_density_variance / global_density) × spread × temporal_surprise
```

Where:
- `local_density_variance` = how clustered are points locally (along curves)
- `global_density` = how spread out overall
- `spread` = coverage of space (0 at start, 1 when spread)
- `temporal_surprise` = how much did the configuration change from previous frame

This captures:
- Local clustering (the bright lines)
- Global spread (exploring space)
- Dynamic moment (the "boom" is a rapid change)
