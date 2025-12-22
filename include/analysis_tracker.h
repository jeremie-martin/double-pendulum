#pragma once

#include "pendulum.h"
#include "variance_tracker.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

// Per-frame analysis data
struct FrameAnalysis {
    double variance = 0.0;        // Angle2 variance (existing metric)
    float max_value = 0.0f;       // Max accumulated pixel value (from GPU)
    float brightness = 0.0f;      // Mean pixel intensity (from GPU)
    double total_energy = 0.0;    // Sum of all pendulum energies
    float contrast_stddev = 0.0f; // Luminance standard deviation
    float contrast_range = 0.0f;  // p95 - p5 luminance spread

    // New metrics for causticness detection
    float edge_energy = 0.0f;       // Gradient magnitude (sharp filaments = high)
    float color_variance = 0.0f;    // Color diversity across RGB channels
    float coverage = 0.0f;          // Fraction of non-black pixels
    float peak_median_ratio = 0.0f; // p99/p50 brightness ratio (bright focal points)

    // Causticness score: rewards sharp edges, color diversity, moderate coverage
    // Penalizes uniform blobs (low edge energy), compact shapes (low coverage),
    // and washed-out images (high coverage/brightness)
    double causticness() const {
        // Coverage factor: peaks around 0.35, penalizes both extremes
        // Low coverage (<0.15): too sparse/compact
        // High coverage (>0.55): washed out/white
        double coverage_factor = 0.0;
        if (coverage > 0.1 && coverage < 0.7) {
            // Peak at 0.35, with asymmetric falloff (harsher for high coverage)
            if (coverage <= 0.35) {
                coverage_factor = (coverage - 0.1) / 0.25; // Linear rise from 0.1 to 0.35
            } else {
                // Steeper falloff for high coverage (penalize white more)
                coverage_factor = 1.0 - std::pow((coverage - 0.35) / 0.35, 1.5);
            }
            coverage_factor = std::max(0.0, coverage_factor);
        }

        // Brightness penalty: high brightness means washed out
        // Caustics look best at moderate brightness (0.05-0.15)
        double brightness_factor = 1.0;
        if (brightness > 0.15) {
            brightness_factor = std::max(0.0, 1.0 - (brightness - 0.15) * 4.0);
        }

        // Contrast factor: good caustics have high contrast_range
        // After white, contrast_range drops because everything is uniformly bright
        double contrast_factor = std::min(1.0, contrast_range * 2.0);

        // Base score from edge energy and color variance
        double score = edge_energy * (1.0 + color_variance * 2.0);

        // Apply all factors
        score *= coverage_factor * brightness_factor * (0.5 + contrast_factor * 0.5);

        return score;
    }
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

        // Compute variance of angle2 using shared function
        std::vector<double> angles;
        angles.reserve(pendulums.size());
        for (auto const& p : pendulums) {
            angles.push_back(p.getTheta2());
        }
        analysis.variance = computeVariance(angles);

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

    // GPU metrics bundle for cleaner parameter passing
    struct GPUMetrics {
        float max_value = 0.0f;
        float brightness = 0.0f;
        float contrast_stddev = 0.0f;
        float contrast_range = 0.0f;
        float edge_energy = 0.0f;
        float color_variance = 0.0f;
        float coverage = 0.0f;
        float peak_median_ratio = 0.0f;
    };

    // Update GPU stats for the last frame
    void updateGPUStats(GPUMetrics const& m) {
        if (!history_.empty()) {
            history_.back().max_value = m.max_value;
            history_.back().brightness = m.brightness;
            history_.back().contrast_stddev = m.contrast_stddev;
            history_.back().contrast_range = m.contrast_range;
            history_.back().edge_energy = m.edge_energy;
            history_.back().color_variance = m.color_variance;
            history_.back().coverage = m.coverage;
            history_.back().peak_median_ratio = m.peak_median_ratio;
            current_ = history_.back();
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
        if (frame < history_.size())
            return history_[frame].variance;
        return 0.0;
    }

    float getMaxValueAt(size_t frame) const {
        if (frame < history_.size())
            return history_[frame].max_value;
        return 0.0f;
    }

    float getBrightnessAt(size_t frame) const {
        if (frame < history_.size())
            return history_[frame].brightness;
        return 0.0f;
    }

    double getEnergyAt(size_t frame) const {
        if (frame < history_.size())
            return history_[frame].total_energy;
        return 0.0;
    }

    float getContrastStddevAt(size_t frame) const {
        if (frame < history_.size())
            return history_[frame].contrast_stddev;
        return 0.0f;
    }

    float getContrastRangeAt(size_t frame) const {
        if (frame < history_.size())
            return history_[frame].contrast_range;
        return 0.0f;
    }

private:
    std::vector<FrameAnalysis> history_;
    FrameAnalysis current_;
};
