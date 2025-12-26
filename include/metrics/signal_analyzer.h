#pragma once

// Signal Analyzer - Generic time series analysis for any metric
//
// This class was formerly CausticnessAnalyzer but has been generalized to work
// with any metric series. The analysis (peak detection, clarity scoring,
// post-reference area) is not causticness-specific and works on any metric.
//
// Usage:
//   SignalAnalyzer analyzer;
//   analyzer.setMetricName("angular_causticness");  // REQUIRED
//   analyzer.setFrameDuration(1.0 / 60.0);
//   analyzer.analyze(collector, events);
//   double score = analyzer.score();

#include "metrics/analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

#include <json.hpp>
#include <string>
#include <vector>

namespace metrics {

// A detected peak in the signal curve
struct SignalPeak {
    int frame = -1;
    double value = 0.0;
    double seconds = 0.0;
    double prominence = 0.0;  // Height above surrounding terrain
};

// Signal evolution metrics
struct SignalMetrics {
    double peak_value = 0.0;            // Maximum value
    int peak_frame = -1;                // Frame of peak
    double peak_seconds = 0.0;          // Time of peak
    double average_value = 0.0;         // Average over analysis window
    double time_above_threshold = 0.0;  // Seconds above quality threshold
    int frames_above_threshold = 0;     // Frames above quality threshold
    double total_value = 0.0;           // Sum (area under curve)

    // Post-reference analysis (computed relative to reference frame)
    double post_ref_average = 0.0;      // Average after reference frame
    double post_ref_peak = 0.0;         // Peak after reference frame
    int post_ref_peak_frame = -1;       // Frame of post-reference peak

    // Peak clarity analysis
    double peak_clarity_score = 1.0;    // main / (main + max_competitor), 1.0 = no competition
    int competing_peaks_count = 0;      // Number of peaks before main peak
    double max_competitor_ratio = 0.0;  // Highest competitor / main_peak
    double nearest_competitor_seconds = 0.0;  // Time distance to nearest competitor

    // Post-reference sustain
    double post_ref_area = 0.0;         // Area under curve after reference
    double post_ref_area_normalized = 0.0;  // Normalized 0-1
    double post_ref_duration = 0.0;     // Window duration used

    // Normalized quality score (0-1)
    double qualityScore() const {
        // Peak value (0-1 range, saturates at 1.0)
        double peak_score = std::min(1.0, peak_value);

        // Post-reference sustain shows visual interest continues
        double sustain_score = post_ref_area_normalized;

        // Peak clarity penalizes competing peaks before main
        double clarity_score = peak_clarity_score;

        // Weight: clarity most important, then peak, then sustain
        return clarity_score * 0.4 + peak_score * 0.35 + sustain_score * 0.25;
    }

    // Legacy accessors for backward compatibility
    double peak_causticness() const { return peak_value; }
    double post_boom_area_normalized() const { return post_ref_area_normalized; }
};

// Signal evolution analyzer - works with any metric
class SignalAnalyzer : public Analyzer {
public:
    SignalAnalyzer() = default;
    ~SignalAnalyzer() override = default;

    // REQUIRED: Set which metric to analyze
    // Must be called before analyze()
    void setMetricName(std::string const& name) { metric_name_ = name; }
    std::string metricName() const { return metric_name_; }

    // Set reference frame for post-reference analysis (optional)
    // If not set, uses the detected peak frame
    void setReferenceFrame(int frame) { reference_frame_ = frame; }
    int referenceFrame() const { return reference_frame_; }

    // Configure analysis parameters
    void setThreshold(double threshold) { quality_threshold_ = threshold; }
    void setPostReferenceWindow(double seconds) { post_ref_window_seconds_ = seconds; }
    void setSamplingInterval(double seconds) { sampling_interval_ = seconds; }
    void setMinPeakSeparation(double seconds) { min_peak_separation_ = seconds; }
    void setMinPeakHeightFraction(double fraction) { min_peak_height_fraction_ = fraction; }
    void setMinProminenceFraction(double fraction) { min_prominence_fraction_ = fraction; }

    // Set frame duration for time-based calculations.
    // Must be positive. Zero or negative values will trigger a warning on analyze().
    void setFrameDuration(double seconds) {
        if (seconds > 0.0) {
            frame_duration_ = seconds;
        }
        // Invalid values ignored - analyze() will use fallback with warning
    }

    // Analyzer interface
    std::string name() const override {
        return metric_name_.empty() ? "signal" : metric_name_;
    }

    void analyze(MetricsCollector const& collector,
                 EventDetector const& events) override;

    double score() const override {
        return has_results_ ? metrics_.qualityScore() : 0.0;
    }

    nlohmann::json toJSON() const override;

    void reset() override {
        has_results_ = false;
        metrics_ = SignalMetrics{};
        samples_.clear();
        sample_times_.clear();
        detected_peaks_.clear();
        total_frames_ = 0;
        // Don't reset metric_name_, reference_frame_, or frame_duration_ so user-set values persist
        // Reset warning flag so it can warn again if frame_duration becomes invalid
        warned_frame_duration_fallback_ = false;
    }

    bool hasResults() const override { return has_results_; }

    // Signal-specific accessors
    SignalMetrics const& getMetrics() const { return metrics_; }
    std::vector<double> const& getSamples() const { return samples_; }

    // Get values at specific intervals after reference frame
    std::vector<std::pair<double, double>> getSampleTimeline() const;

    // Peak detection (public for testing/debugging)
    std::vector<SignalPeak> const& getDetectedPeaks() const { return detected_peaks_; }

    // Get peak clarity score directly (for filtering)
    double peakClarityScore() const { return metrics_.peak_clarity_score; }

    // Get post-reference area normalized (for filtering)
    double postReferenceAreaNormalized() const { return metrics_.post_ref_area_normalized; }

    // Legacy aliases for backward compatibility
    double postBoomAreaNormalized() const { return postReferenceAreaNormalized(); }

private:
    // Peak detection
    double computeProminence(std::vector<double> const& values, size_t peak_idx) const;
    std::vector<SignalPeak> findPeaks(std::vector<double> const& values) const;
    void computePeakClarity(std::vector<double> const& values);
    void computePostReferenceArea(std::vector<double> const& values);

    // Configuration
    std::string metric_name_;                     // REQUIRED: Which metric to analyze
    int reference_frame_ = -1;                    // Reference frame for post-ref analysis (-1 = use peak)
    double quality_threshold_ = 0.25;             // Minimum value to count
    double post_ref_window_seconds_ = 10.0;       // Post-reference area window
    double sampling_interval_ = 0.5;              // Sample every N seconds
    double min_peak_separation_ = 0.3;            // Min seconds between peaks
    double min_peak_height_fraction_ = 0.1;       // Min peak height as fraction of max
    double min_prominence_fraction_ = 0.05;       // Min prominence as fraction of max

    // State
    bool has_results_ = false;
    SignalMetrics metrics_;
    std::vector<double> samples_;         // Sampled values
    std::vector<double> sample_times_;    // Times of samples
    std::vector<SignalPeak> detected_peaks_;  // All detected peaks

    int actual_reference_frame_ = -1;     // Resolved reference frame (peak or user-set)
    double frame_duration_ = 0.0;         // 0 = auto-detect
    size_t total_frames_ = 0;
    mutable bool warned_frame_duration_fallback_ = false;  // One-time warning flag
};

// ============================================================================
// BACKWARD COMPATIBILITY ALIASES (deprecated)
// ============================================================================

// Legacy type aliases - prefer SignalAnalyzer and SignalMetrics
using CausticnessAnalyzer = SignalAnalyzer;
using CausticnessMetrics = SignalMetrics;
using CausticnessPeak = SignalPeak;

} // namespace metrics
