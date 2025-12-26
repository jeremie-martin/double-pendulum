#pragma once

#include "optimize/prediction_target.h"
#include "optimize/frame_detector.h"
#include "optimize/score_predictor.h"
#include "metrics/metrics_collector.h"
#include "metrics/event_detector.h"
#include "metrics/signal_analyzer.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace optimize {

// Multi-target evaluation orchestrator
// Coordinates prediction for multiple targets using shared metrics
//
// The evaluator processes targets in two phases:
// 1. Frame targets (boom, chaos) - evaluated first
// 2. Score targets (boom_quality) - evaluated second, using boom frame as reference
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
    // This is the new preferred API - does not require pre-built analyzer
    std::vector<PredictionResult> evaluate(
        metrics::MetricsCollector const& collector,
        double frame_duration) const {

        std::vector<PredictionResult> results;
        results.reserve(targets_.size());

        // Phase 1: Evaluate all frame targets first
        std::optional<int> boom_frame;
        for (auto const& target : targets_) {
            if (target.isFrame()) {
                PredictionResult result = evaluateFrameTarget(target, collector, frame_duration);
                results.push_back(result);

                // Track boom frame for score targets
                if (target.name == "boom" && result.valid()) {
                    boom_frame = result.predicted_frame;
                }
            }
        }

        // Phase 2: Evaluate score targets using boom frame as reference
        for (auto const& target : targets_) {
            if (target.isScore()) {
                PredictionResult result = evaluateScoreTarget(
                    target, collector, boom_frame, frame_duration);
                results.push_back(result);
            }
        }

        return results;
    }

    // Legacy API for backward compatibility
    // DEPRECATED: Use evaluate(collector, frame_duration) instead
    std::vector<PredictionResult> evaluate(
        metrics::MetricsCollector const& collector,
        metrics::EventDetector const& events,
        metrics::SignalAnalyzer const& analyzer,
        double frame_duration) const {

        std::vector<PredictionResult> results;
        results.reserve(targets_.size());

        // Get boom frame from analyzer or events
        std::optional<int> boom_frame;
        if (analyzer.hasResults()) {
            boom_frame = analyzer.getMetrics().peak_frame;
        } else {
            auto boom_event = events.getEvent(metrics::EventNames::Boom);
            if (boom_event && boom_event->detected()) {
                boom_frame = boom_event->frame;
            }
        }

        for (auto const& target : targets_) {
            PredictionResult result;
            if (target.isFrame()) {
                result = evaluateFrameTarget(target, collector, frame_duration);
                // Update boom_frame if this is the boom target
                if (target.name == "boom" && result.valid()) {
                    boom_frame = result.predicted_frame;
                }
            } else {
                result = evaluateScoreTarget(target, collector, boom_frame, frame_duration);
            }
            results.push_back(result);
        }

        return results;
    }

    // Evaluate a single target by name
    std::optional<PredictionResult> evaluateByName(
        std::string const& name,
        metrics::MetricsCollector const& collector,
        std::optional<int> reference_frame,
        double frame_duration) const {

        for (auto const& target : targets_) {
            if (target.name == name) {
                if (target.isFrame()) {
                    return evaluateFrameTarget(target, collector, frame_duration);
                } else {
                    return evaluateScoreTarget(target, collector, reference_frame, frame_duration);
                }
            }
        }
        return std::nullopt;
    }

    // Legacy version for backward compatibility
    std::optional<PredictionResult> evaluateByName(
        std::string const& name,
        metrics::MetricsCollector const& collector,
        metrics::EventDetector const& events,
        metrics::SignalAnalyzer const& analyzer,
        double frame_duration) const {

        std::optional<int> ref_frame;
        if (analyzer.hasResults()) {
            ref_frame = analyzer.getMetrics().peak_frame;
        }
        return evaluateByName(name, collector, ref_frame, frame_duration);
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

    // Evaluate score-type target using the new generic API
    PredictionResult evaluateScoreTarget(
        PredictionTarget const& target,
        metrics::MetricsCollector const& collector,
        std::optional<int> reference_frame,
        double frame_duration) const {

        auto const& params = target.scoreParams();
        ScorePredictor predictor(params);

        // Use reference frame if available, otherwise -1 (will use peak frame)
        int ref = reference_frame.value_or(-1);
        ScorePrediction prediction = predictor.predict(collector, ref, frame_duration);
        return toPredictionResult(target.name, prediction);
    }
};

// ============================================================================
// TARGET CREATION HELPERS
// ============================================================================
// NOTE: These helpers require explicit metric names. No defaults.
// Targets should be defined in config files using [targets.X] sections.

// Create boom target with given metric
inline PredictionTarget createBoomTarget(std::string const& metric_name) {
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

// Create chaos target with given metric
inline PredictionTarget createChaosTarget(std::string const& metric_name) {
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

// Create boom quality target with given metric
inline PredictionTarget createBoomQualityTarget(std::string const& metric_name) {
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
