#include "boom_detector.h"
#include <cmath>
#include <algorithm>
#include <numeric>

BoomDetector::BoomDetector(BoomDetectionParams const& params)
    : enabled_(params.enabled),
      variance_threshold_(params.variance_threshold),
      confirmation_frames_(params.confirmation_frames),
      early_stop_(params.early_stop) {}

std::optional<BoomResult> BoomDetector::update(std::vector<double> const& angles, int frame) {
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

std::optional<WhiteResult> BoomDetector::detectWhiteFrame(
    double plateau_tolerance,
    int plateau_frames
) const {
    if (variance_history_.size() < static_cast<size_t>(plateau_frames + 1)) {
        return std::nullopt;
    }

    // Find the maximum variance to use as reference for plateau detection
    double max_variance = *std::max_element(variance_history_.begin(), variance_history_.end());
    if (max_variance < 0.001) {
        return std::nullopt;  // No significant variance recorded
    }

    // Look for plateau: consecutive frames where variance changes by less than tolerance
    int consecutive_stable = 0;
    int plateau_start = -1;

    for (size_t i = 1; i < variance_history_.size(); ++i) {
        double prev = variance_history_[i - 1];
        double curr = variance_history_[i];

        // Check if variance is relatively stable (not increasing significantly)
        // Use relative change based on current variance level
        double relative_change = std::abs(curr - prev) / std::max(prev, 0.001);

        if (relative_change < plateau_tolerance) {
            if (consecutive_stable == 0) {
                plateau_start = static_cast<int>(i - 1);
            }
            consecutive_stable++;

            if (consecutive_stable >= plateau_frames) {
                // Plateau detected
                return WhiteResult{plateau_start, variance_history_[plateau_start]};
            }
        } else {
            consecutive_stable = 0;
            plateau_start = -1;
        }
    }

    return std::nullopt;
}

void BoomDetector::reset() {
    variance_history_.clear();
    boom_frame_.reset();
    frames_above_threshold_ = 0;
    current_variance_ = 0.0;
}
