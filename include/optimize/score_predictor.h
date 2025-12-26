#pragma once

#include "optimize/prediction_target.h"
#include "metrics/signal_analyzer.h"
#include "metrics/metrics_collector.h"
#include "metrics/event_detector.h"

#include <algorithm>
#include <cmath>
#include <string>

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

        // Check metric name is set
        if (params_.metric_name.empty()) {
            result.score = 0.0;
            return result;
        }

        // Create and configure analyzer
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
