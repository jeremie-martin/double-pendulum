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
    // Call after simulation completes
    std::optional<WhiteResult> detectWhiteFrame(
        double plateau_tolerance = 0.05,
        int plateau_frames = 30
    ) const;

    // Reset detector state for new simulation
    void reset();

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
