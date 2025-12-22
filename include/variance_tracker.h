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

// Simple variance tracker - computes and stores variance history
// All detection logic (boom, white, thresholds) is external
class VarianceTracker {
public:
    // Update with angles from all pendulums for current frame
    // Returns the computed variance for this frame
    double update(std::vector<double> const& angles) {
        current_variance_ = computeVariance(angles);
        variance_history_.push_back(current_variance_);
        return current_variance_;
    }

    // Reset tracker state
    void reset() {
        variance_history_.clear();
        current_variance_ = 0.0;
    }

    // Getters
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

private:
    std::vector<double> variance_history_;
    double current_variance_ = 0.0;
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
                            double boom_threshold, int boom_confirmation,
                            double white_threshold, int white_confirmation) {
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
