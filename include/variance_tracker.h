#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

// Simple variance tracker - computes and stores variance history
// All detection logic (boom, white, thresholds) is external
class VarianceTracker {
public:
    // Update with angles from all pendulums for current frame
    // Returns the computed variance for this frame
    double update(std::vector<double> const& angles) {
        if (angles.empty()) {
            variance_history_.push_back(0.0);
            current_variance_ = 0.0;
            return 0.0;
        }

        // Calculate mean
        double sum = std::accumulate(angles.begin(), angles.end(), 0.0);
        double mean = sum / angles.size();

        // Calculate variance
        double var_sum = 0.0;
        for (double a : angles) {
            double diff = a - mean;
            var_sum += diff * diff;
        }

        current_variance_ = var_sum / angles.size();
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

} // namespace VarianceUtils
