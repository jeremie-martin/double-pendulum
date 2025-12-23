#pragma once

#include "metrics/analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

#include <json.hpp>
#include <optional>
#include <string>

namespace metrics {

// Boom quality classification
enum class BoomType {
    Unknown,      // Not analyzed or no boom
    Sharp,        // High derivative spike, quick transition
    Gradual,      // Slow build-up, lower derivative
    Oscillating   // Multiple threshold crossings before settling
};

inline std::string boomTypeToString(BoomType type) {
    switch (type) {
    case BoomType::Sharp:
        return "sharp";
    case BoomType::Gradual:
        return "gradual";
    case BoomType::Oscillating:
        return "oscillating";
    default:
        return "unknown";
    }
}

// Detailed boom quality metrics
struct BoomQuality {
    double sharpness_ratio = 0.0;     // derivative / threshold (>1 = sharp)
    double peak_derivative = 0.0;      // Max d(variance)/dt near boom
    int frames_to_peak = 0;            // Frames from crossing to peak derivative
    double initial_acceleration = 0.0; // Second derivative at boom
    double pre_boom_variance_mean = 0.0;   // Mean variance before boom
    double post_boom_variance_max = 0.0;   // Peak variance after boom
    double variance_at_boom = 0.0;         // Variance when boom detected

    BoomType type = BoomType::Unknown;

    // Normalized quality score (0-1)
    double qualityScore() const {
        // Higher sharpness = better (up to a point)
        double sharpness_score = std::min(1.0, sharpness_ratio / 2.0);

        // Quick peak is better
        double peak_score = frames_to_peak > 0
                                ? std::max(0.0, 1.0 - frames_to_peak / 30.0)
                                : 0.5;

        // Good contrast between pre and post variance
        double contrast_score = 0.0;
        if (pre_boom_variance_mean > 0.001) {
            double contrast = post_boom_variance_max / pre_boom_variance_mean;
            contrast_score = std::min(1.0, contrast / 1000.0);
        }

        // Weight the components
        return sharpness_score * 0.5 + peak_score * 0.3 + contrast_score * 0.2;
    }
};

// Boom-specific analyzer with quality characterization
class BoomAnalyzer : public Analyzer {
public:
    BoomAnalyzer() = default;
    ~BoomAnalyzer() override = default;

    // Configure analysis parameters
    void setMetric(std::string const& metric_name) { metric_name_ = metric_name; }
    void setAnalysisWindow(int frames_before, int frames_after) {
        frames_before_ = frames_before;
        frames_after_ = frames_after;
    }
    void setSharpnessThreshold(double threshold) {
        sharpness_threshold_ = threshold;
    }

    // Analyzer interface
    std::string name() const override { return ScoreNames::Boom; }

    void analyze(MetricsCollector const& collector,
                 EventDetector const& events) override;

    double score() const override {
        return has_results_ ? quality_.qualityScore() : 0.0;
    }

    nlohmann::json toJSON() const override;

    void reset() override {
        has_results_ = false;
        quality_ = BoomQuality{};
        boom_frame_ = -1;
        boom_seconds_ = 0.0;
    }

    bool hasResults() const override { return has_results_; }

    // Boom-specific accessors
    BoomQuality const& getQuality() const { return quality_; }
    int getBoomFrame() const { return boom_frame_; }
    double getBoomSeconds() const { return boom_seconds_; }
    BoomType getBoomType() const { return quality_.type; }

    // Check if boom meets quality threshold
    bool meetsQualityThreshold(double min_sharpness = 0.0) const {
        if (!has_results_)
            return false;
        return quality_.sharpness_ratio >= min_sharpness;
    }

private:
    std::string metric_name_ = MetricNames::Variance;
    int frames_before_ = 30;   // Frames to analyze before boom
    int frames_after_ = 60;    // Frames to analyze after boom
    double sharpness_threshold_ = 0.5;  // Threshold for "sharp" classification

    bool has_results_ = false;
    BoomQuality quality_;
    int boom_frame_ = -1;
    double boom_seconds_ = 0.0;

    BoomType classifyBoomType() const;
};

} // namespace metrics
