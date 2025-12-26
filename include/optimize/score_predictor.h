#pragma once

#include "optimize/prediction_target.h"
#include "metrics/signal_analyzer.h"
#include "metrics/metrics_collector.h"
#include "metrics/event_detector.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace optimize {

// Result of score prediction
struct ScorePrediction {
    double score = 0.0;
    ScoreMethod method_used = ScoreMethod::PeakClarity;

    bool valid() const { return score >= 0.0 && score <= 1.0; }
};

// Generic score predictor that works with any metric
//
// Usage:
//   ScorePredictor predictor(params);
//   auto result = predictor.predict(collector, reference_frame, frame_duration);
//
// The predictor creates a SignalAnalyzer internally and uses the metric
// specified in params.metric_name for analysis.
class ScorePredictor {
public:
    ScorePredictor() = default;
    explicit ScorePredictor(ScoreParams const& params) : params_(params) {}

    void setParams(ScoreParams const& params) { params_ = params; }
    ScoreParams const& getParams() const { return params_; }

    // Main prediction entry point - uses configured metric from params
    //
    // Parameters:
    //   collector: The MetricsCollector containing computed metrics
    //   reference_frame: Reference frame for post-reference analysis (typically boom frame)
    //   frame_duration: Duration of each frame in seconds
    //
    // Returns:
    //   ScorePrediction with score in [0,1] range
    ScorePrediction predict(
        metrics::MetricsCollector const& collector,
        int reference_frame,
        double frame_duration) const {

        ScorePrediction result;
        result.method_used = params_.method;

        // ConstantScore doesn't need any data - just return configured value
        if (params_.method == ScoreMethod::ConstantScore) {
            result.score = std::clamp(params_.constant_score, 0.0, 1.0);
            return result;
        }

        // Check metric name is set for other methods
        if (params_.metric_name.empty()) {
            result.score = 0.0;
            return result;
        }

        // Get the raw metric series for boom-independent methods
        auto const* metric_series = collector.getMetric(params_.metric_name);
        if (!metric_series || metric_series->empty()) {
            result.score = 0.0;
            return result;
        }
        auto const& values = metric_series->values();

        // Boom-independent methods: compute directly from values
        switch (params_.method) {
        case ScoreMethod::DynamicRange:
            result.score = computeDynamicRange(values);
            result.score = std::clamp(result.score, 0.0, 1.0);
            return result;
        case ScoreMethod::RiseTime:
            result.score = computeRiseTime(values);
            result.score = std::clamp(result.score, 0.0, 1.0);
            return result;
        case ScoreMethod::Smoothness:
            result.score = computeSmoothness(values, frame_duration);
            result.score = std::clamp(result.score, 0.0, 1.0);
            return result;
        case ScoreMethod::PreBoomContrast:
            result.score = computePreBoomContrast(values, reference_frame, frame_duration);
            result.score = std::clamp(result.score, 0.0, 1.0);
            return result;
        case ScoreMethod::BoomSteepness:
            result.score = computeBoomSteepness(values, reference_frame, frame_duration);
            result.score = std::clamp(result.score, 0.0, 1.0);
            return result;
        case ScoreMethod::BuildupGradient:
            result.score = computeBuildupGradient(values, frame_duration);
            result.score = std::clamp(result.score, 0.0, 1.0);
            return result;
        case ScoreMethod::PeakDominance:
            result.score = computePeakDominance(values);
            result.score = std::clamp(result.score, 0.0, 1.0);
            return result;
        case ScoreMethod::DecayRate:
            result.score = computeDecayRate(values, frame_duration);
            result.score = std::clamp(result.score, 0.0, 1.0);
            return result;
        default:
            break;
        }

        // Boom-dependent methods: use SignalAnalyzer
        metrics::SignalAnalyzer analyzer;
        analyzer.setMetricName(params_.metric_name);
        analyzer.setReferenceFrame(reference_frame);
        analyzer.setFrameDuration(frame_duration);

        // Run analysis (empty EventDetector since we have explicit reference frame)
        analyzer.analyze(collector, metrics::EventDetector{});

        if (!analyzer.hasResults()) {
            result.score = 0.0;
            return result;
        }

        result.score = computeScore(analyzer);
        result.score = std::clamp(result.score, 0.0, 1.0);
        return result;
    }

    // Legacy prediction entry point - uses pre-configured analyzer
    // DEPRECATED: Use predict(collector, reference_frame, frame_duration) instead
    ScorePrediction predict(metrics::SignalAnalyzer const& analyzer) const {
        ScorePrediction result;
        result.method_used = params_.method;

        if (!analyzer.hasResults()) {
            result.score = 0.0;
            return result;
        }

        result.score = computeScore(analyzer);
        result.score = std::clamp(result.score, 0.0, 1.0);
        return result;
    }

private:
    ScoreParams params_;

    // Compute score from analyzer based on configured method
    double computeScore(metrics::SignalAnalyzer const& analyzer) const {
        switch (params_.method) {
        case ScoreMethod::PeakClarity:
            return analyzer.peakClarityScore();
        case ScoreMethod::PostBoomSustain:
            return analyzer.postReferenceAreaNormalized();
        case ScoreMethod::Composite:
            return computeComposite(analyzer);
        case ScoreMethod::ConstantScore:
            // Constant score doesn't need analyzer - return configured value
            return params_.constant_score;
        default:
            return analyzer.peakClarityScore();
        }
    }

    // Composite weighted score
    // Uses weights from params, or defaults to analyzer's qualityScore()
    double computeComposite(metrics::SignalAnalyzer const& analyzer) const {
        if (params_.weights.empty()) {
            // Default: use analyzer's built-in quality score
            return analyzer.score();
        }

        // Custom weighted combination
        double total = 0.0;
        double weight_sum = 0.0;

        for (auto const& [name, weight] : params_.weights) {
            double value = 0.0;
            if (name == "peak_clarity") {
                value = analyzer.peakClarityScore();
            } else if (name == "post_boom_sustain" || name == "post_ref_sustain") {
                value = analyzer.postReferenceAreaNormalized();
            } else if (name == "peak_causticness" || name == "peak_value") {
                value = std::min(1.0, analyzer.getMetrics().peak_value);
            }
            total += value * weight;
            weight_sum += weight;
        }

        return weight_sum > 0 ? total / weight_sum : 0.0;
    }

    // ========================================================================
    // Boom-independent score methods
    // ========================================================================

    // Dynamic range: (max - min) / max
    // High score = high contrast/drama in the signal
    static double computeDynamicRange(std::vector<double> const& values) {
        if (values.empty()) return 0.0;

        auto [min_it, max_it] = std::minmax_element(values.begin(), values.end());
        double min_val = *min_it;
        double max_val = *max_it;

        if (max_val <= 0.0) return 0.0;
        return (max_val - min_val) / max_val;
    }

    // Rise time: peak_frame / total_frames
    // Low score = early peak (action happens quickly)
    // High score = late peak (slow buildup)
    // Inverted so early peak = high score (more dramatic)
    static double computeRiseTime(std::vector<double> const& values) {
        if (values.empty()) return 0.0;

        auto max_it = std::max_element(values.begin(), values.end());
        size_t peak_frame = static_cast<size_t>(std::distance(values.begin(), max_it));

        // Invert: early peak = high score
        double ratio = static_cast<double>(peak_frame) / static_cast<double>(values.size());
        return 1.0 - ratio;
    }

    // Smoothness: 1 / (1 + mean_abs_second_derivative)
    // High score = smooth signal, low score = noisy/jagged
    static double computeSmoothness(std::vector<double> const& values, double frame_duration) {
        if (values.size() < 3) return 1.0;  // Too short to measure

        // Compute second derivative (central difference)
        double sum_abs_d2 = 0.0;
        double dt2 = frame_duration * frame_duration;

        for (size_t i = 1; i + 1 < values.size(); ++i) {
            double d2 = (values[i + 1] - 2.0 * values[i] + values[i - 1]) / dt2;
            sum_abs_d2 += std::abs(d2);
        }

        double mean_abs_d2 = sum_abs_d2 / static_cast<double>(values.size() - 2);

        // Normalize - empirically, values around 1000 are typical for "smooth"
        // Scale factor makes typical smooth signals score ~0.8-0.9
        double scaled = mean_abs_d2 / 10000.0;
        return 1.0 / (1.0 + scaled);
    }

    // ========================================================================
    // Boom-relative score methods
    // ========================================================================

    // Pre-boom contrast: 1 - (avg_before / peak)
    // High score = quiet before boom (good contrast)
    double computePreBoomContrast(std::vector<double> const& values,
                                   int reference_frame,
                                   double frame_duration) const {
        if (values.empty() || reference_frame <= 0) return 0.0;

        // Window before boom
        int window_frames = static_cast<int>(params_.window_seconds / frame_duration);
        int start = std::max(0, reference_frame - window_frames);
        int end = std::min(reference_frame, static_cast<int>(values.size()));

        if (start >= end) return 0.0;

        // Average in pre-boom window
        double sum = 0.0;
        for (int i = start; i < end; ++i) {
            sum += values[i];
        }
        double avg_before = sum / static_cast<double>(end - start);

        // Peak value (at or near reference frame)
        double peak = values[std::min(reference_frame, static_cast<int>(values.size()) - 1)];
        if (peak <= 0.0) return 0.0;

        // Contrast: how much quieter was it before?
        double ratio = avg_before / peak;
        return std::clamp(1.0 - ratio, 0.0, 1.0);
    }

    // Boom steepness: derivative_at_boom / max_derivative
    // High score = sharp transition at boom
    double computeBoomSteepness(std::vector<double> const& values,
                                 int reference_frame,
                                 double frame_duration) const {
        if (values.size() < 3 || reference_frame < 1 ||
            reference_frame >= static_cast<int>(values.size()) - 1) {
            return 0.0;
        }

        // Derivative at boom (central difference)
        double deriv_at_boom = (values[reference_frame + 1] - values[reference_frame - 1])
                              / (2.0 * frame_duration);

        // Find max derivative in the signal
        double max_deriv = 0.0;
        for (size_t i = 1; i + 1 < values.size(); ++i) {
            double d = (values[i + 1] - values[i - 1]) / (2.0 * frame_duration);
            max_deriv = std::max(max_deriv, std::abs(d));
        }

        if (max_deriv <= 0.0) return 0.0;

        // Ratio of boom derivative to max derivative
        // Use absolute value - steepness can be either direction
        return std::clamp(std::abs(deriv_at_boom) / max_deriv, 0.0, 1.0);
    }

    // ========================================================================
    // Additional signal analysis methods
    // ========================================================================

    // Buildup gradient: average slope from start to peak
    // High score = steep, dramatic rise to peak
    static double computeBuildupGradient(std::vector<double> const& values,
                                          double frame_duration) {
        if (values.size() < 2) return 0.0;

        // Find peak frame
        auto max_it = std::max_element(values.begin(), values.end());
        size_t peak_frame = static_cast<size_t>(std::distance(values.begin(), max_it));
        double peak_value = *max_it;

        if (peak_frame == 0 || peak_value <= 0.0) return 0.0;

        // Average gradient from start to peak
        double start_value = values[0];
        double rise = peak_value - start_value;
        double time_to_peak = static_cast<double>(peak_frame) * frame_duration;

        double gradient = rise / time_to_peak;

        // Normalize: typical gradients vary widely, use sigmoid-like normalization
        // Scale factor chosen empirically - adjust based on typical metric ranges
        double normalized = gradient / (gradient + 100.0);  // Approaches 1 as gradient increases
        return std::clamp(normalized, 0.0, 1.0);
    }

    // Peak dominance: peak / mean ratio
    // High score = peak stands out significantly from average
    static double computePeakDominance(std::vector<double> const& values) {
        if (values.empty()) return 0.0;

        double sum = 0.0;
        double max_val = 0.0;
        for (double v : values) {
            sum += v;
            max_val = std::max(max_val, v);
        }

        double mean = sum / static_cast<double>(values.size());
        if (mean <= 0.0) return 0.0;

        double ratio = max_val / mean;

        // Normalize: ratio of 1 = peak equals mean (bad), ratio >> 1 = dominant peak
        // Use log scale for better distribution
        // ratio=1 -> 0, ratio=e -> 0.5, ratio=e^2 -> 0.67, etc.
        double normalized = 1.0 - 1.0 / ratio;  // Simpler: 0 when ratio=1, approaches 1 as ratio->inf
        return std::clamp(normalized, 0.0, 1.0);
    }

    // Decay rate: how quickly signal drops after peak
    // High score = fast, clean decay
    static double computeDecayRate(std::vector<double> const& values,
                                    double frame_duration) {
        if (values.size() < 3) return 0.0;

        // Find peak frame
        auto max_it = std::max_element(values.begin(), values.end());
        size_t peak_frame = static_cast<size_t>(std::distance(values.begin(), max_it));
        double peak_value = *max_it;

        if (peak_frame >= values.size() - 1 || peak_value <= 0.0) return 0.0;

        // Compute average negative derivative after peak
        // Look at a window after the peak (up to 30% of remaining signal or all of it)
        size_t remaining = values.size() - peak_frame - 1;
        size_t window = std::min(remaining, std::max(size_t(10), remaining * 3 / 10));

        double total_drop = 0.0;
        for (size_t i = peak_frame; i < peak_frame + window; ++i) {
            double drop = values[i] - values[i + 1];
            if (drop > 0) total_drop += drop;  // Only count decreases
        }

        double avg_drop_per_frame = total_drop / static_cast<double>(window);
        double decay_rate = avg_drop_per_frame / frame_duration;

        // Normalize relative to peak value
        double relative_decay = decay_rate / peak_value;

        // Sigmoid normalization - typical decay rates map to 0.3-0.8
        double normalized = relative_decay / (relative_decay + 0.1);
        return std::clamp(normalized, 0.0, 1.0);
    }
};

// Convenience function: predict score using params (new API)
inline ScorePrediction predictScore(
    metrics::MetricsCollector const& collector,
    ScoreParams const& params,
    int reference_frame,
    double frame_duration) {
    ScorePredictor predictor(params);
    return predictor.predict(collector, reference_frame, frame_duration);
}

// Legacy convenience function (deprecated)
inline ScorePrediction predictScore(metrics::SignalAnalyzer const& analyzer,
                                    ScoreParams const& params) {
    ScorePredictor predictor(params);
    return predictor.predict(analyzer);
}

// Convert ScorePrediction to PredictionResult
inline PredictionResult toPredictionResult(std::string const& target_name,
                                           ScorePrediction const& prediction) {
    PredictionResult result;
    result.target_name = target_name;
    result.type = PredictionType::Score;
    result.predicted_score = prediction.score;
    return result;
}

}  // namespace optimize
