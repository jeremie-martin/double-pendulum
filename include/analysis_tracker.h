#pragma once

#include "pendulum.h"
#include "variance_tracker.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

// Per-frame analysis data
struct FrameAnalysis {
    double variance = 0.0;      // Angle2 variance (existing metric)
    float max_value = 0.0f;     // Max accumulated pixel value (from GPU)
    float brightness = 0.0f;    // Mean pixel intensity (from GPU)
    double total_energy = 0.0;  // Sum of all pendulum energies
};

// Extended tracker for analysis mode
// Collects multiple statistics per frame
class AnalysisTracker {
public:
    // Update with pendulum data and GPU statistics
    FrameAnalysis update(std::vector<Pendulum> const& pendulums, float max_val, float brightness) {
        FrameAnalysis analysis;

        if (pendulums.empty()) {
            history_.push_back(analysis);
            return analysis;
        }

        // Compute variance of angle2 (same as VarianceTracker)
        std::vector<double> angles;
        angles.reserve(pendulums.size());
        for (auto const& p : pendulums) {
            angles.push_back(p.getTheta2());
        }

        double sum = std::accumulate(angles.begin(), angles.end(), 0.0);
        double mean = sum / angles.size();

        double var_sum = 0.0;
        for (double a : angles) {
            double diff = a - mean;
            var_sum += diff * diff;
        }
        analysis.variance = var_sum / angles.size();

        // GPU statistics
        analysis.max_value = max_val;
        analysis.brightness = brightness;

        // Compute total energy
        double energy_sum = 0.0;
        for (auto const& p : pendulums) {
            energy_sum += p.totalEnergy();
        }
        analysis.total_energy = energy_sum;

        history_.push_back(analysis);
        current_ = analysis;

        return analysis;
    }

    // Simple update (no GPU stats yet)
    FrameAnalysis updateAnglesOnly(std::vector<Pendulum> const& pendulums) {
        return update(pendulums, 0.0f, 0.0f);
    }

    // Update GPU stats for the last frame
    void updateGPUStats(float max_val, float brightness) {
        if (!history_.empty()) {
            history_.back().max_value = max_val;
            history_.back().brightness = brightness;
            current_.max_value = max_val;
            current_.brightness = brightness;
        }
    }

    // Reset tracker state
    void reset() {
        history_.clear();
        current_ = {};
    }

    // Getters
    FrameAnalysis const& getCurrent() const { return current_; }
    std::vector<FrameAnalysis> const& getHistory() const { return history_; }
    size_t frameCount() const { return history_.size(); }

    // Extract variance history for compatibility with VarianceUtils
    std::vector<double> getVarianceHistory() const {
        std::vector<double> variances;
        variances.reserve(history_.size());
        for (auto const& frame : history_) {
            variances.push_back(frame.variance);
        }
        return variances;
    }

    // Get specific metric at frame
    double getVarianceAt(size_t frame) const {
        if (frame < history_.size()) return history_[frame].variance;
        return 0.0;
    }

    float getMaxValueAt(size_t frame) const {
        if (frame < history_.size()) return history_[frame].max_value;
        return 0.0f;
    }

    float getBrightnessAt(size_t frame) const {
        if (frame < history_.size()) return history_[frame].brightness;
        return 0.0f;
    }

    double getEnergyAt(size_t frame) const {
        if (frame < history_.size()) return history_[frame].total_energy;
        return 0.0;
    }

private:
    std::vector<FrameAnalysis> history_;
    FrameAnalysis current_;
};
