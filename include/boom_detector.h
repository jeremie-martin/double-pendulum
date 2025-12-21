#pragma once

#include "config.h"
#include <optional>
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

struct BoomResult {
    int frame;              // Frame where boom was detected
    double variance;        // Angle variance at detection
    double max_spread;      // Max angle difference between pendulums
};

class BoomDetector {
public:
    explicit BoomDetector(BoomDetectionParams const& params)
        : enabled_(params.enabled),
          variance_threshold_(params.variance_threshold),
          confirmation_frames_(params.confirmation_frames),
          early_stop_(params.early_stop) {}

    // Update with angles from all pendulums for current frame
    // Returns boom info if detected this frame
    std::optional<BoomResult> update(std::vector<double> const& angles, int frame) {
        if (!enabled_ || angles.empty()) return std::nullopt;

        // Calculate mean angle
        double sum = std::accumulate(angles.begin(), angles.end(), 0.0);
        double mean = sum / angles.size();

        // Calculate variance
        double var_sum = 0.0;
        double min_angle = angles[0];
        double max_angle = angles[0];

        for (double a : angles) {
            double diff = a - mean;
            var_sum += diff * diff;
            min_angle = std::min(min_angle, a);
            max_angle = std::max(max_angle, a);
        }

        current_variance_ = var_sum / angles.size();
        double spread = max_angle - min_angle;

        // Track variance history
        variance_history_.push_back(current_variance_);

        // Check for boom (variance exceeds threshold for N consecutive frames)
        if (!boom_frame_.has_value()) {
            if (current_variance_ > variance_threshold_) {
                frames_above_threshold_++;
                if (frames_above_threshold_ >= confirmation_frames_) {
                    // Boom confirmed - report frame where it started
                    boom_frame_ = frame - confirmation_frames_ + 1;
                    return BoomResult{*boom_frame_, current_variance_, spread};
                }
            } else {
                frames_above_threshold_ = 0;
            }
        }

        return std::nullopt;
    }

    // Reset detector state for new simulation
    void reset() {
        variance_history_.clear();
        boom_frame_.reset();
        frames_above_threshold_ = 0;
        current_variance_ = 0.0;
    }

    // Getters
    bool isEnabled() const { return enabled_; }
    bool shouldEarlyStop() const { return early_stop_; }
    double getCurrentVariance() const { return current_variance_; }
    bool hasBoomOccurred() const { return boom_frame_.has_value(); }
    std::optional<int> getBoomFrame() const { return boom_frame_; }
    std::vector<double> const& getVarianceHistory() const { return variance_history_; }

private:
    bool enabled_;
    double variance_threshold_;
    int confirmation_frames_;
    bool early_stop_;

    std::vector<double> variance_history_;
    std::optional<int> boom_frame_;
    int frames_above_threshold_ = 0;
    double current_variance_ = 0.0;
};
