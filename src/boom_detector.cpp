#include "boom_detector.h"
#include <cmath>
#include <algorithm>
#include <numeric>

BoomDetector::BoomDetector(BoomDetectionParams const& params)
    : variance_threshold_(params.variance_threshold),
      confirmation_frames_(params.confirmation_frames),
      white_tolerance_(params.white_tolerance),
      white_plateau_frames_(params.white_plateau_frames),
      early_stop_after_white_(params.early_stop_after_white) {}

std::optional<BoomResult> BoomDetector::update(std::vector<double> const& angles, int frame) {
    if (angles.empty()) return std::nullopt;

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

    std::optional<BoomResult> boom_result;

    // Check for boom (variance exceeds threshold for N consecutive frames)
    if (!boom_frame_.has_value()) {
        if (current_variance_ > variance_threshold_) {
            if (frames_above_threshold_ == 0) {
                // First frame above threshold - store this variance
                boom_variance_ = current_variance_;
            }
            frames_above_threshold_++;
            if (frames_above_threshold_ >= confirmation_frames_) {
                // Boom confirmed - report frame where it started
                boom_frame_ = frame - confirmation_frames_ + 1;
                boom_result = BoomResult{*boom_frame_, boom_variance_, spread};
            }
        } else {
            frames_above_threshold_ = 0;
            boom_variance_ = 0.0;  // Reset if we drop below threshold
        }
    }

    // Check for white (variance plateau) - only after boom occurred
    if (boom_frame_.has_value() && !white_frame_.has_value()) {
        size_t history_size = variance_history_.size();
        if (history_size >= static_cast<size_t>(white_plateau_frames_ + 1)) {
            // Check if last N frames are stable
            int consecutive_stable = 0;
            for (size_t i = history_size - white_plateau_frames_; i < history_size; ++i) {
                double prev = variance_history_[i - 1];
                double curr = variance_history_[i];
                double relative_change = std::abs(curr - prev) / std::max(prev, 0.001);

                if (relative_change < white_tolerance_) {
                    consecutive_stable++;
                } else {
                    consecutive_stable = 0;
                }
            }

            if (consecutive_stable >= white_plateau_frames_) {
                white_frame_ = frame - white_plateau_frames_ + 1;
                white_variance_ = current_variance_;
            }
        }
    }

    return boom_result;
}

std::optional<WhiteResult> BoomDetector::detectWhiteFrame() const {
    if (white_frame_.has_value()) {
        return WhiteResult{*white_frame_, white_variance_};
    }
    return std::nullopt;
}

void BoomDetector::reset() {
    variance_history_.clear();
    boom_frame_.reset();
    boom_variance_ = 0.0;
    white_frame_.reset();
    white_variance_ = 0.0;
    frames_above_threshold_ = 0;
    current_variance_ = 0.0;
}
