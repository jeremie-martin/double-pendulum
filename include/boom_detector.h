#pragma once

#include "config.h"
#include <optional>
#include <vector>

struct BoomResult {
    int frame;              // Frame where boom was detected
    double variance;        // Angle variance at detection
    double max_spread;      // Max angle difference between pendulums
};

struct WhiteResult {
    int frame;              // Frame where white (plateau) was detected
    double variance;        // Variance at plateau
};

class BoomDetector {
public:
    explicit BoomDetector(BoomDetectionParams const& params);

    // Update with angles from all pendulums for current frame
    // Returns boom info if detected this frame
    std::optional<BoomResult> update(std::vector<double> const& angles, int frame);

    // Detect white frame (variance plateau) from history
    std::optional<WhiteResult> detectWhiteFrame() const;

    // Reset detector state for new simulation
    void reset();

    // Getters
    bool shouldEarlyStopAfterWhite() const { return early_stop_after_white_; }
    double getCurrentVariance() const { return current_variance_; }
    bool hasBoomOccurred() const { return boom_frame_.has_value(); }
    std::optional<int> getBoomFrame() const { return boom_frame_; }
    double getBoomVariance() const { return boom_variance_; }
    bool hasWhiteOccurred() const { return white_frame_.has_value(); }
    std::optional<int> getWhiteFrame() const { return white_frame_; }
    double getWhiteVariance() const { return white_variance_; }
    std::vector<double> const& getVarianceHistory() const { return variance_history_; }

private:
    // Boom detection params
    double variance_threshold_;
    int confirmation_frames_;

    // White detection params
    double white_tolerance_;
    int white_plateau_frames_;
    bool early_stop_after_white_;

    // State
    std::vector<double> variance_history_;
    std::optional<int> boom_frame_;
    double boom_variance_ = 0.0;
    std::optional<int> white_frame_;
    double white_variance_ = 0.0;
    int frames_above_threshold_ = 0;
    double current_variance_ = 0.0;
};
