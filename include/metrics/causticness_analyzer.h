#pragma once

#include "metrics/analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

#include <json.hpp>
#include <string>
#include <vector>

namespace metrics {

// A detected peak in the causticness curve
struct CausticnessPeak {
    int frame = -1;
    double value = 0.0;
    double seconds = 0.0;
};

// Causticness evolution metrics
struct CausticnessMetrics {
    double peak_causticness = 0.0;      // Maximum causticness value
    int peak_frame = -1;                // Frame of peak causticness
    double peak_seconds = 0.0;          // Time of peak causticness
    double average_causticness = 0.0;   // Average over analysis window
    double time_above_threshold = 0.0;  // Seconds above quality threshold
    int frames_above_threshold = 0;     // Frames above quality threshold
    double total_causticness = 0.0;     // Sum (area under curve)

    // Post-boom analysis (most relevant for quality)
    double post_boom_average = 0.0;     // Average causticness after boom
    double post_boom_peak = 0.0;        // Peak causticness after boom
    int post_boom_peak_frame = -1;      // Frame of post-boom peak

    // Peak clarity analysis (new)
    double peak_clarity_score = 1.0;    // main / (main + max_competitor), 1.0 = no competition
    int competing_peaks_count = 0;      // Number of peaks before main peak
    double max_competitor_ratio = 0.0;  // Highest competitor / main_peak
    double nearest_competitor_seconds = 0.0;  // Time distance to nearest competitor

    // Post-boom sustain (new)
    double post_boom_area = 0.0;        // Area under curve after boom
    double post_boom_area_normalized = 0.0;  // Normalized 0-1
    double post_boom_duration = 0.0;    // Window duration used

    // Normalized quality score (0-1)
    double qualityScore() const {
        // Peak is most important, but sustained quality matters too
        double peak_score = std::min(1.0, peak_causticness / 100.0);

        // Average contributes to overall quality
        double avg_score = std::min(1.0, average_causticness / 50.0);

        // Time above threshold shows sustained quality
        double duration_score = std::min(1.0, time_above_threshold / 5.0);

        return peak_score * 0.4 + avg_score * 0.35 + duration_score * 0.25;
    }
};

// Causticness evolution analyzer
class CausticnessAnalyzer : public Analyzer {
public:
    CausticnessAnalyzer() = default;
    ~CausticnessAnalyzer() override = default;

    // Configure analysis parameters
    void setThreshold(double threshold) { quality_threshold_ = threshold; }
    void setPostBoomWindow(double seconds) { post_boom_window_seconds_ = seconds; }
    void setSamplingInterval(double seconds) { sampling_interval_ = seconds; }
    void setMinPeakSeparation(double seconds) { min_peak_separation_ = seconds; }
    void setMinPeakHeightFraction(double fraction) { min_peak_height_fraction_ = fraction; }
    void setFrameDuration(double seconds) { frame_duration_ = seconds; }

    // Analyzer interface
    std::string name() const override { return ScoreNames::Causticness; }

    void analyze(MetricsCollector const& collector,
                 EventDetector const& events) override;

    double score() const override {
        return has_results_ ? metrics_.qualityScore() : 0.0;
    }

    nlohmann::json toJSON() const override;

    void reset() override {
        has_results_ = false;
        metrics_ = CausticnessMetrics{};
        samples_.clear();
        sample_times_.clear();
        detected_peaks_.clear();
        total_frames_ = 0;
        // Don't reset frame_duration_ so user-set value persists
    }

    bool hasResults() const override { return has_results_; }

    // Causticness-specific accessors
    CausticnessMetrics const& getMetrics() const { return metrics_; }
    std::vector<double> const& getSamples() const { return samples_; }

    // Get causticness at specific intervals after boom
    std::vector<std::pair<double, double>> getSampleTimeline() const;

    // Peak detection (public for testing/debugging)
    std::vector<CausticnessPeak> const& getDetectedPeaks() const { return detected_peaks_; }

    // Get peak clarity score directly (for filtering)
    double peakClarityScore() const { return metrics_.peak_clarity_score; }

    // Get post-boom area normalized (for filtering)
    double postBoomAreaNormalized() const { return metrics_.post_boom_area_normalized; }

private:
    // Peak detection
    std::vector<CausticnessPeak> findPeaks(std::vector<double> const& values) const;
    void computePeakClarity(std::vector<double> const& values);
    void computePostBoomArea(std::vector<double> const& values);
    double quality_threshold_ = 20.0;          // Minimum causticness to count
    double post_boom_window_seconds_ = 10.0;   // Post-boom area window (user requested)
    double sampling_interval_ = 0.5;           // Sample every N seconds
    double min_peak_separation_ = 0.3;         // Min seconds between peaks (user: 0.3s)
    double min_peak_height_fraction_ = 0.1;    // Min peak height as fraction of max

    bool has_results_ = false;
    CausticnessMetrics metrics_;
    std::vector<double> samples_;       // Sampled causticness values
    std::vector<double> sample_times_;  // Times of samples
    std::vector<CausticnessPeak> detected_peaks_;  // All detected peaks

    int boom_frame_ = -1;
    double frame_duration_ = 0.0;  // 0 = auto-detect (was 1/60)
    size_t total_frames_ = 0;
};

} // namespace metrics
