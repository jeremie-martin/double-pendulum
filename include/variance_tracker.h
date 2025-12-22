#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <vector>

// Standalone variance computation - shared by VarianceTracker and AnalysisTracker
inline double computeVariance(std::vector<double> const& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    double mean = sum / values.size();
    double var_sum = 0.0;
    for (double v : values) {
        double diff = v - mean;
        var_sum += diff * diff;
    }
    return var_sum / values.size();
}

// Spread metrics - how well pendulums cover the visual space
struct SpreadMetrics {
    double spread_ratio = 0.0;    // Fraction of pendulums above horizontal
    double angle1_mean = 0.0;     // Mean of angle1 (for debugging)
    double angle1_variance = 0.0; // Variance of angle1 (for debugging)
};

// Compute spread metrics from angle1 values
// "Above horizontal" = |angle1| > pi/2 (pendulum tip higher than pivot)
// angle1 is measured from vertical down, so:
//   angle1 = 0      -> hanging straight down (below horizontal)
//   angle1 = ±pi/2  -> horizontal
//   angle1 = ±pi    -> pointing straight up (above horizontal)
inline SpreadMetrics computeSpread(std::vector<double> const& angle1_values) {
    SpreadMetrics metrics;
    if (angle1_values.empty()) {
        return metrics;
    }

    constexpr double PI = 3.14159265358979323846;
    constexpr double HALF_PI = PI / 2.0;

    // Count pendulums above horizontal
    int above_count = 0;
    double sum = 0.0;

    for (double angle1 : angle1_values) {
        // Normalize angle to [-pi, pi]
        double normalized = std::fmod(angle1, 2.0 * PI);
        if (normalized > PI)
            normalized -= 2.0 * PI;
        if (normalized < -PI)
            normalized += 2.0 * PI;

        // Check if above horizontal (|angle1| > pi/2)
        if (std::abs(normalized) > HALF_PI) {
            above_count++;
        }

        sum += normalized;
    }

    // Compute spread ratio
    metrics.spread_ratio =
        static_cast<double>(above_count) / static_cast<double>(angle1_values.size());

    // Compute mean
    metrics.angle1_mean = sum / static_cast<double>(angle1_values.size());

    // Compute variance
    double var_sum = 0.0;
    for (double angle1 : angle1_values) {
        double normalized = std::fmod(angle1, 2.0 * PI);
        if (normalized > PI)
            normalized -= 2.0 * PI;
        if (normalized < -PI)
            normalized += 2.0 * PI;

        double diff = normalized - metrics.angle1_mean;
        var_sum += diff * diff;
    }
    metrics.angle1_variance = var_sum / static_cast<double>(angle1_values.size());

    return metrics;
}

// Simple variance tracker - computes and stores variance history
// Also tracks spread metrics (how well pendulums cover the visual space)
// All detection logic (boom, white, thresholds) is external
class VarianceTracker {
public:
    // Update with angle2 values only (original behavior)
    // Returns the computed variance for this frame
    double update(std::vector<double> const& angles) {
        current_variance_ = computeVariance(angles);
        variance_history_.push_back(current_variance_);
        return current_variance_;
    }

    // Update with both angle2 (for variance) and angle1 (for spread)
    // Returns the computed variance for this frame
    double updateWithSpread(std::vector<double> const& angle2s,
                            std::vector<double> const& angle1s) {
        current_variance_ = computeVariance(angle2s);
        variance_history_.push_back(current_variance_);

        current_spread_ = computeSpread(angle1s);
        spread_history_.push_back(current_spread_);

        return current_variance_;
    }

    // Reset tracker state
    void reset() {
        variance_history_.clear();
        spread_history_.clear();
        current_variance_ = 0.0;
        current_spread_ = SpreadMetrics{};
    }

    // Variance getters
    double getCurrentVariance() const { return current_variance_; }
    std::vector<double> const& getHistory() const { return variance_history_; }
    size_t frameCount() const { return variance_history_.size(); }

    // Utility: get variance at specific frame (0-indexed)
    double getVarianceAt(size_t frame) const {
        if (frame < variance_history_.size()) {
            return variance_history_[frame];
        }
        return 0.0;
    }

    // Spread getters
    SpreadMetrics const& getCurrentSpread() const { return current_spread_; }
    std::vector<SpreadMetrics> const& getSpreadHistory() const { return spread_history_; }

    // Get spread at specific frame
    SpreadMetrics getSpreadAt(size_t frame) const {
        if (frame < spread_history_.size()) {
            return spread_history_[frame];
        }
        return SpreadMetrics{};
    }

    // Get final spread (last frame)
    SpreadMetrics getFinalSpread() const {
        if (!spread_history_.empty()) {
            return spread_history_.back();
        }
        return SpreadMetrics{};
    }

private:
    std::vector<double> variance_history_;
    std::vector<SpreadMetrics> spread_history_;
    double current_variance_ = 0.0;
    SpreadMetrics current_spread_;
};

// Helper functions for threshold detection (can be used externally)
namespace VarianceUtils {

// Check if variance has been above threshold for N consecutive frames
// Returns the frame where threshold was first crossed, or -1 if not met
inline int checkThresholdCrossing(std::vector<double> const& history, double threshold,
                                  int confirmation_frames) {
    if (history.size() < static_cast<size_t>(confirmation_frames)) {
        return -1;
    }

    int consecutive = 0;
    int first_frame = -1;

    for (size_t i = 0; i < history.size(); ++i) {
        if (history[i] > threshold) {
            if (consecutive == 0) {
                first_frame = static_cast<int>(i);
            }
            consecutive++;
            if (consecutive >= confirmation_frames) {
                return first_frame;
            }
        } else {
            consecutive = 0;
            first_frame = -1;
        }
    }

    return -1;
}

// Results structure for boom/white detection
struct ThresholdResults {
    std::optional<int> boom_frame;
    double boom_variance = 0.0;
    std::optional<int> white_frame;
    double white_variance = 0.0;
};

// Update threshold detection results given current state
// This eliminates duplicate detection logic in simulation.cpp and main_gui.cpp
inline void updateDetection(ThresholdResults& results, VarianceTracker const& tracker,
                            double boom_threshold, int boom_confirmation, double white_threshold,
                            int white_confirmation) {
    auto const& history = tracker.getHistory();

    // Check for boom detection
    if (!results.boom_frame.has_value()) {
        int boom = checkThresholdCrossing(history, boom_threshold, boom_confirmation);
        if (boom >= 0) {
            results.boom_frame = boom;
            results.boom_variance = tracker.getVarianceAt(boom);
        }
    }

    // Check for white detection (only after boom)
    if (results.boom_frame.has_value() && !results.white_frame.has_value()) {
        int white = checkThresholdCrossing(history, white_threshold, white_confirmation);
        if (white >= 0) {
            results.white_frame = white;
            results.white_variance = tracker.getVarianceAt(white);
        }
    }
}

} // namespace VarianceUtils
