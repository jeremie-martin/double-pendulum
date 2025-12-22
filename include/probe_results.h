#pragma once

#include <optional>

// Results from a probe simulation (physics-only, no rendering)
// Used for quick parameter evaluation before full rendering
struct ProbeResults {
    bool success = false;

    // Boom detection
    std::optional<int> boom_frame;
    double boom_seconds = 0.0;
    double boom_variance = 0.0;

    // Spread metrics (how well pendulums cover the circle)
    double final_spread_ratio = 0.0; // Fraction with angle1 above horizontal
    double angle1_mean = 0.0;        // Mean of angle1 at end (for debugging)
    double angle1_variance = 0.0;    // Variance of angle1 at end (for debugging)

    // Simulation info
    int frames_completed = 0;
    double final_variance = 0.0; // Final angle2 variance
};
