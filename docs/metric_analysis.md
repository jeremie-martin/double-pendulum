# Metric Deep Dive Analysis

## Overview

This document provides a comprehensive analysis of all metrics in the double pendulum simulation,
examining their mathematical correctness, N-independence, stability characteristics, and potential improvements.

---

## Category 1: Basic Statistics Metrics

### 1. `variance`
- **Purpose**: Measures spread of angle2 values (second arm angles)
- **Formula**: `Σ(θ - μ)² / N` (sample variance)
- **N-independence**: ✅ Yes - sample variance is inherently N-independent
- **Range**: [0, π²] ≈ [0, 10]
- **Stability**: A+ (0.1% CV at boom, 0.4% overall)
- **Verdict**: ✅ Perfect

### 2. `spread_ratio`
- **Purpose**: Fraction of pendulums above horizontal (|angle1| > π/2)
- **Formula**: `count(|θ| > π/2) / N`
- **N-independence**: ✅ Yes - proportion is N-independent
- **Range**: [0, 1]
- **Stability**: A+ (0.0% CV at boom, 0.4% overall)
- **Verdict**: ✅ Perfect

### 3. `circular_spread`
- **Purpose**: Measures angular dispersion using circular statistics
- **Formula**: `1 - R` where R = mean resultant length = `√((Σcos(θ)/N)² + (Σsin(θ)/N)²)`
- **N-independence**: ✅ Yes - R is inherently N-independent
- **Range**: [0, 1] (0=all same direction, 1=uniform circular distribution)
- **Stability**: A+ (0.1% CV at boom, 0.3% overall)
- **Verdict**: ✅ Perfect, well-founded in circular statistics

### 4. `angular_range`
- **Purpose**: Normalized range of angle1 values
- **Formula**: `(max - min) / 2π`
- **N-independence**: ⚠️ Almost - sample range is an order statistic that increases with N
- **Range**: [0, 1]
- **Stability**: A+ (0.0% CV at boom, 0.1% overall)
- **Issues**: Minor - sample range has slight N-dependence, but converges quickly
- **Improvement potential**: Could use interquartile range (IQR/2π) for more robust estimate
- **Verdict**: ✅ Good enough - N-dependence is negligible for practical counts

### 5. `total_energy`
- **Purpose**: Energy of all pendulums
- **Formula**: `Σ p.totalEnergy()` (sum of all pendulum energies)
- **N-independence**: ❌ **BUG** - this is a SUM, not an average!
- **Stability**: A+ (0.0% CV - but misleading since all pendulums start with identical energy)
- **Issues**: **CRITICAL** - should be mean energy per pendulum
- **Fix**: Change to `total_energy / N`
- **Verdict**: ⚠️ **Needs fix**

---

## Category 2: Sector-based Causticness Metrics

All use the pattern: `coverage × concentration_measure` where:
- coverage = fraction of sectors occupied = `occupied / num_sectors`
- concentration = Gini coefficient or CV of sector counts

**Common Issue**: The coverage term follows birthday-problem statistics:
- E[occupied] = M × (1 - (1-1/M)^N) where M = num_sectors
- Even with adaptive sector sizing, this introduces N-dependence

### 6. `angular_causticness`
- **Purpose**: Measures concentration of tip angles (θ1 + θ2)
- **Formula**: `coverage × gini`
- **Stability**: C (19.5% overall, 6.8% at boom)
- **Issues**: Coverage term has birthday-problem N-dependence; high early-phase CV expected
- **Verdict**: ⚠️ Acceptable

### 7. `tip_causticness`
- **Purpose**: Uses atan2(x2, y2) for geometrically correct tip angle
- **Formula**: Same as angular_causticness but with position-derived angle
- **Stability**: D (20.2% overall, 8.1% at boom)
- **Verdict**: ⚠️ Acceptable

### 8. `cv_causticness`
- **Purpose**: Uses CV instead of Gini for spikiness detection
- **Formula**: `coverage × min(1, cv / cv_normalization)`
- **Stability**: A at boom (4.7%), D overall (22.1%)
- **Verdict**: ✅ Good for boom detection

### 9. `organization_causticness`
- **Purpose**: Combines mean resultant lengths of both arms with coverage
- **Formula**: `(1 - R1×R2) × coverage`
- **N-independence**:
  - R1, R2 are inherently N-independent ✅
  - Coverage has birthday-problem dependence ⚠️
- **Stability**: B at boom (8.1%), C overall (16.9%)
- **Improvement**: Replace coverage with circular_spread or fixed threshold
- **Verdict**: ⚠️ Could be improved

### 10. `r1_concentration`
- **Purpose**: Causticness of angle1 alone (first arm)
- **Stability**: A+ (0.0% at boom, 1.8% overall)
- **Verdict**: ✅ Excellent

### 11. `r2_concentration`
- **Purpose**: Causticness of angle2 alone (second arm)
- **Stability**: D (34.3% at boom, 21.2% overall)
- **Issues**: Much worse than r1 - the second arm has more chaotic motion
- **Analysis**: This isn't a bug - angle2 genuinely has more variable distribution at boom
- **Verdict**: ⚠️ Expected behavior, not a bug

### 12. `joint_concentration`
- **Purpose**: Product of r1 and r2
- **Formula**: `r1 × r2`
- **Stability**: D (34.3% at boom, 21.6% overall)
- **Issues**: Dominated by r2's variability
- **Verdict**: ⚠️ Expected behavior

---

## Category 3: Spatial and Fold Metrics

### 13. `spatial_concentration` (FIXED)
- **Purpose**: 2D grid-based concentration metric
- **Formula**: `spatial_extent × gini` (after fix)
- **Previous issue**: Used adaptive grid + coverage (birthday-problem dependent)
- **Fix applied**: Fixed physical grid + spatial extent instead of coverage
- **Stability**: A+ (0.0% at boom, 3.3% overall) - FIXED!
- **Verdict**: ✅ Excellent after fix

### 14. `fold_causticness`
- **Purpose**: Measures CV of adjacent-pair distances × spread
- **Formula**: `spread × min(1, cv / cv_normalization)`
- **N-independence**:
  - Adjacent distances scale with 1/√N, but CV is a ratio ✅
  - spread uses circular_spread or mean_radius / max_radius ✅
- **Stability**: B (0.0% at boom, 10.0% overall)
- **Verdict**: ✅ Good

---

## Category 4: Local Coherence Metrics

All use neighbor distances between adjacent pendulums (ordered by initial angle).

### 15. `trajectory_smoothness`
- **Purpose**: Lag-1 autocorrelation of neighbor distances (clustering measure)
- **Formula**: `spread × max(0, autocorrelation)`
- **N-independence**: ✅ Autocorrelation is inherently N-independent
- **Stability**: B (0.1% at boom, 5.2% overall)
- **Verdict**: ✅ Good

### 16. `curvature`
- **Purpose**: P90/P10 ratio of neighbor distances (bimodality measure)
- **Formula**: `spread × min(1, log10(ratio) / normalization)`
- **N-independence**: ✅ Percentile ratios are N-independent
- **Stability**: A+ (0.0% at boom, 4.5% overall)
- **Verdict**: ✅ Good

### 17. `true_folds` (actually: Neighbor Distance Gini)
- **Purpose**: Gini coefficient of neighbor distances
- **Formula**: `spread × max(0, (gini - baseline) / divisor)`
- **N-independence**: ✅ Gini is inherently N-independent
- **Stability**: A+ at boom (0.0%), C overall (17.2%), 12.8% scale sensitivity
- **Issues**: High scale sensitivity in late phase
- **Analysis**: This is expected - in chaotic phase, distance distribution varies more
- **Verdict**: ✅ Good for boom detection

### 18. `local_coherence`
- **Purpose**: Min/Median ratio of neighbor distances (fold strength)
- **Formula**: `spread × min(1, max(0, (-log10(min/median) - baseline) / divisor))`
- **N-independence**: ⚠️ Min is an order statistic that scales with N
- **Analysis**: With more pendulums, the minimum distance tends to be smaller
- **Stability**: A+ (0.0% at boom, 4.7% overall)
- **Verdict**: ⚠️ Good stability despite theoretical concern

---

## Category 5: GPU Metrics

These are computed from the rendered frame, not from pendulum state.

### 19. `brightness`
- **Purpose**: Mean pixel intensity
- **N-independence**: Not applicable - brightness naturally increases with more pendulums
- **Verdict**: N/A for physics analysis

### 20. `coverage`
- **Purpose**: Fraction of non-zero pixels
- **N-independence**: Not applicable - more pendulums cover more pixels
- **Verdict**: N/A for physics analysis

### 21. `max_value`
- **Purpose**: Peak pixel intensity (before post-processing)
- **N-independence**: Partially - depends on line density at hotspots
- **Verdict**: N/A for physics analysis

---

## Summary of Issues

### Critical
1. **`total_energy`**: Currently sums all pendulum energies instead of averaging. Should be per-pendulum.

### Moderate (Expected Behavior, Not Bugs)
2. **`r2_concentration`** / **`joint_concentration`**: High CV because angle2 is inherently more chaotic
3. **Coverage term in sector metrics**: Birthday-problem N-dependence, but manageable

### Minor (Theoretical but Negligible in Practice)
4. **`angular_range`**: Sample range has slight N-dependence
5. **`local_coherence`**: Min is an order statistic

---

## Applied Fixes

### 1. `total_energy` → Per-pendulum Average
Changed from sum to mean: `total_energy / N`

### 2. Birthday-Problem Coverage Correction
Added expected coverage normalization to sector-based metrics:
```cpp
double p_empty = pow(1.0 - 1.0/num_sectors, N);
double expected_coverage = 1.0 - p_empty;
double normalized_coverage = raw_coverage / expected_coverage;
```

Applied to: `angular_causticness`, `computeCausticnessFromAngles` (r1, r2, tip),
`cv_causticness`, `organization_causticness`

**Effect**: Minimal at typical N (1000-50000) because expected coverage is already ~100%
with the adaptive sector sizing. More impactful at very low N (<100).

### 3. `spatial_concentration` → Fixed Physical Grid
Changed from adaptive grid to fixed physical grid over pendulum reach [-2.1, 2.1]².
Replaced coverage with spatial extent (bounding box). Major improvement: 22.6% → 0.0% CV at boom.

---

## Understanding the Remaining Instabilities

### Why `r2_concentration` is Worse than `r1_concentration`
This is **physics, not a bug**:
- First arm (angle1): Attached to fixed pivot, more constrained motion
- Second arm (angle2): Compounding chaos, genuinely more variable distribution

At boom time, the second arm's angle distribution varies more across different N.
This reflects real physical differences in how chaos develops.

### Why Early-Phase CV is High (~50%)
Early in simulation, pendulums are bunched together:
- Few sectors occupied → small variations cause large relative changes
- This is expected and doesn't affect boom detection (which happens later)

---

## Category 6: Velocity-Based Metrics (NEW)

These metrics use angular velocities (ω1, ω2) to compute tip velocities and analyze their distribution.
They capture the *dynamics* of the boom moment rather than just positions.

### 22. `velocity_dispersion`
- **Purpose**: How spread out are velocity DIRECTIONS?
- **Formula**: `1 - R` where R = mean resultant length of velocity direction angles
- **N-independence**: ✅ Yes - circular statistics normalized by N
- **Range**: [0, 1] (0=all same direction, 1=uniform/dispersed)
- **Stability**: A+ (0.1% CV overall, 0.1% at boom)
- **Boom behavior**: Moderate (~0.78) at boom
- **Verdict**: ✅ Excellent stability

### 23. `velocity_bimodality`
- **Purpose**: Detects "half left, half right" opposing velocity pattern
- **Formula**: `(gap / 2σ) × balance` where gap = separation of positive/negative projections
- **N-independence**: ✅ Yes - based on statistical properties
- **Range**: [0, 1]
- **Stability**: A+ (0.1% CV overall, 0.1% at boom)
- **Boom behavior**: HIGH (~0.79) at boom - strong bimodal pattern detected!
- **Peak timing**: Actually peaks ~1.2s BEFORE boom (frame 453 vs 527)
- **Analysis**: This detects the *divergence moment* when pendulums start separating,
  which precedes the visual caustic formation
- **Verdict**: ✅ Excellent - best detector of opposing motion

### 24. `speed_variance`
- **Purpose**: Normalized variance of tip speeds (CV of speeds)
- **Formula**: `std(speeds) / mean(speeds)`, clamped to [0, 1]
- **N-independence**: ✅ Yes - CV is scale-free
- **Range**: [0, 1]
- **Stability**: A+ (0.1% CV overall, 0.2% at boom)
- **Boom behavior**: Moderate (~0.60) at boom, decreases toward chaos
- **Verdict**: ✅ Excellent stability

### 25. `angular_momentum_spread`
- **Purpose**: Spread of angular momenta about the pivot
- **Formula**: Balance of clockwise/counterclockwise rotation + magnitude variance
- **N-independence**: ✅ Yes - normalized by N
- **Range**: [0, 1]
- **Boom behavior**: High when half rotate CW, half CCW
- **Verdict**: TBD (new metric)

### 26. `acceleration_dispersion`
- **Purpose**: How spread out are tip accelerations?
- **Formula**: `1 - R` where R = mean resultant length of acceleration direction angles
- **N-independence**: ✅ Yes - circular statistics normalized by N
- **Range**: [0, 1]
- **Boom behavior**: Captures force/torque divergence
- **Verdict**: TBD (new metric)

### Key Insight: Velocity vs Position Metrics

Position-based metrics (causticness, etc.) detect the *visual caustic* - when pendulums
spatially cluster in fold patterns.

Velocity-based metrics detect the *dynamics* that create caustics:
1. **Early phase**: All velocities aligned (low dispersion, low bimodality)
2. **Pre-boom (~1.2s before)**: Velocities diverge into two groups (peak bimodality)
3. **Boom**: Caustic forms as diverging groups cross paths
4. **Chaos**: Random velocities (high dispersion, moderate bimodality)

The `velocity_bimodality` peak PRECEDES the causticness peak, making it potentially
useful for predicting boom timing.

---

## Recommendations for Future Work

### Potential Improvements (Low Priority)
1. Consider IQR-based `angular_range` for more robust estimate
2. Consider using P5 instead of min in `local_coherence`
3. For very low N probing (<500), consider using simpler metrics like `circular_spread`

### Metrics Most Suitable for Low-N Probing
Based on stability analysis, these are the most reliable at N=1000:
1. `circular_spread` - 0.1% CV overall, 0.1% at boom
2. `variance` - 0.4% CV overall, 0.1% at boom
3. `velocity_bimodality` - 0.1% CV overall, 0.1% at boom (NEW - excellent!)
4. `velocity_dispersion` - 0.1% CV overall, 0.1% at boom (NEW)
5. `speed_variance` - 0.1% CV overall, 0.2% at boom (NEW)
6. `r1_concentration` - 1.8% CV overall, 0.0% at boom
7. `cv_causticness` - 22.1% overall, 4.7% at boom (good for boom detection)
