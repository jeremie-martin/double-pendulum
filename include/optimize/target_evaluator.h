#pragma once

#include "optimize/prediction_target.h"
#include "optimize/frame_detector.h"
#include "optimize/score_predictor.h"
#include "metrics/metrics_collector.h"
#include "metrics/event_detector.h"
#include "metrics/causticness_analyzer.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace optimize {

// Multi-target evaluation orchestrator
// Coordinates prediction for multiple targets using shared metrics
class TargetEvaluator {
public:
    TargetEvaluator() = default;

    // Add a prediction target
    void addTarget(PredictionTarget const& target) {
        targets_.push_back(target);
    }

    // Set all targets at once
    void setTargets(std::vector<PredictionTarget> const& targets) {
        targets_ = targets;
    }

    // Clear all targets
    void clearTargets() {
        targets_.clear();
    }

    // Get configured targets
    std::vector<PredictionTarget> const& getTargets() const {
        return targets_;
    }

    // Evaluate all targets and return predictions
    std::vector<PredictionResult> evaluate(
        metrics::MetricsCollector const& collector,
        metrics::EventDetector const& events,
        metrics::CausticnessAnalyzer const& analyzer,
        double frame_duration) const {

        std::vector<PredictionResult> results;
        results.reserve(targets_.size());

        for (auto const& target : targets_) {
            PredictionResult result = evaluateTarget(
                target, collector, events, analyzer, frame_duration);
            results.push_back(result);
        }

        return results;
    }

    // Evaluate a single target by name
    std::optional<PredictionResult> evaluateByName(
        std::string const& name,
        metrics::MetricsCollector const& collector,
        metrics::EventDetector const& events,
        metrics::CausticnessAnalyzer const& analyzer,
        double frame_duration) const {

        for (auto const& target : targets_) {
            if (target.name == name) {
                return evaluateTarget(target, collector, events, analyzer, frame_duration);
            }
        }
        return std::nullopt;
    }

    // Get prediction result by target name from results vector
    static std::optional<PredictionResult> findByName(
        std::vector<PredictionResult> const& results,
        std::string const& name) {

        for (auto const& r : results) {
            if (r.target_name == name) {
                return r;
            }
        }
        return std::nullopt;
    }

    // Convenience: get boom frame from results
    static std::optional<int> getBoomFrame(std::vector<PredictionResult> const& results) {
        auto boom = findByName(results, "boom");
        if (boom && boom->isFrame() && boom->valid()) {
            return boom->predicted_frame;
        }
        return std::nullopt;
    }

    // Convenience: get chaos frame from results
    static std::optional<int> getChaosFrame(std::vector<PredictionResult> const& results) {
        auto chaos = findByName(results, "chaos");
        if (chaos && chaos->isFrame() && chaos->valid()) {
            return chaos->predicted_frame;
        }
        return std::nullopt;
    }

    // Convenience: get boom quality from results
    static std::optional<double> getBoomQuality(std::vector<PredictionResult> const& results) {
        auto quality = findByName(results, "boom_quality");
        if (quality && quality->isScore()) {
            return quality->predicted_score;
        }
        return std::nullopt;
    }

private:
    std::vector<PredictionTarget> targets_;

    // Evaluate a single target
    PredictionResult evaluateTarget(
        PredictionTarget const& target,
        metrics::MetricsCollector const& collector,
        metrics::EventDetector const& events,
        metrics::CausticnessAnalyzer const& analyzer,
        double frame_duration) const {

        if (target.isFrame()) {
            return evaluateFrameTarget(target, collector, frame_duration);
        } else {
            return evaluateScoreTarget(target, analyzer);
        }
    }

    // Evaluate frame-type target
    PredictionResult evaluateFrameTarget(
        PredictionTarget const& target,
        metrics::MetricsCollector const& collector,
        double frame_duration) const {

        auto const& params = target.frameParams();
        FrameDetector detector(params);
        FrameDetection detection = detector.detect(collector, frame_duration);
        return toPredictionResult(target.name, detection);
    }

    // Evaluate score-type target
    PredictionResult evaluateScoreTarget(
        PredictionTarget const& target,
        metrics::CausticnessAnalyzer const& analyzer) const {

        auto const& params = target.scoreParams();
        ScorePredictor predictor(params);
        ScorePrediction prediction = predictor.predict(analyzer);
        return optimize::toPredictionResult(target.name, prediction);
    }
};

// ============================================================================
// DEFAULT TARGETS
// ============================================================================

// Create default boom target
inline PredictionTarget createDefaultBoomTarget(std::string const& metric_name = "angular_causticness") {
    PredictionTarget target;
    target.name = "boom";
    target.type = PredictionType::Frame;

    FrameDetectionParams params;
    params.method = FrameDetectionMethod::MaxValue;
    params.metric_name = metric_name;
    params.offset_seconds = 0.0;
    target.params = params;

    return target;
}

// Create default chaos target
inline PredictionTarget createDefaultChaosTarget(std::string const& metric_name = "variance") {
    PredictionTarget target;
    target.name = "chaos";
    target.type = PredictionType::Frame;

    FrameDetectionParams params;
    params.method = FrameDetectionMethod::ThresholdCrossing;
    params.metric_name = metric_name;
    params.crossing_threshold = 0.8;
    params.crossing_confirmation = 10;
    target.params = params;

    return target;
}

// Create default boom quality target
inline PredictionTarget createDefaultBoomQualityTarget(std::string const& metric_name = "angular_causticness") {
    PredictionTarget target;
    target.name = "boom_quality";
    target.type = PredictionType::Score;

    ScoreParams params;
    params.method = ScoreMethod::Composite;
    params.metric_name = metric_name;
    // Empty weights = use analyzer's default qualityScore()
    target.params = params;

    return target;
}

// Create default targets for backward compatibility
inline std::vector<PredictionTarget> createDefaultTargets(
    std::string const& boom_metric = "angular_causticness",
    std::string const& chaos_metric = "variance") {

    return {
        createDefaultBoomTarget(boom_metric),
        createDefaultChaosTarget(chaos_metric),
        createDefaultBoomQualityTarget(boom_metric)
    };
}

// ============================================================================
// CONFIG CONVERSION (requires config.h)
// ============================================================================

// Forward declaration - include config.h when using these functions
struct TargetConfig;

// Convert TargetConfig (from config.h) to PredictionTarget
// This is defined here to avoid circular dependencies
inline PredictionTarget targetConfigToPredictionTarget(
    std::string const& name,
    std::string const& type,
    std::string const& metric,
    std::string const& method,
    double offset_seconds,
    double peak_percent_threshold,
    double min_peak_prominence,
    int smoothing_window,
    double crossing_threshold,
    int crossing_confirmation,
    std::vector<std::pair<std::string, double>> const& weights) {

    PredictionTarget target;
    target.name = name;

    if (type == "score" || type == "quality") {
        target.type = PredictionType::Score;

        ScoreParams params;
        params.metric_name = metric;
        params.method = parseScoreMethod(method);
        params.weights = weights;
        target.params = params;
    } else {
        target.type = PredictionType::Frame;

        FrameDetectionParams params;
        params.metric_name = metric;
        params.method = parseFrameDetectionMethod(method);
        params.offset_seconds = offset_seconds;
        params.peak_percent_threshold = peak_percent_threshold;
        params.min_peak_prominence = min_peak_prominence;
        params.smoothing_window = smoothing_window;
        params.crossing_threshold = crossing_threshold;
        params.crossing_confirmation = crossing_confirmation;
        target.params = params;
    }

    return target;
}

}  // namespace optimize
