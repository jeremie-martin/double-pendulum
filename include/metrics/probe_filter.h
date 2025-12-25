#pragma once

#include "metrics/analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "optimize/prediction_target.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace metrics {

// =============================================================================
// Probe Filter System
// =============================================================================
//
// The ProbeFilter evaluates whether a simulation meets quality criteria.
// It's used in two contexts:
//
// 1. BATCH GENERATION (batch_generator.cpp):
//    - User specifies filter criteria in TOML config (FilterCriteria struct)
//    - FilterCriteria::toProbeFilter() converts to ProbeFilter
//    - BatchGenerator uses Simulation::runProbe() with fewer pendulums
//    - ProbeFilter evaluates if the probe passes (with predictions)
//    - Only passing probes proceed to full rendering
//
// 2. PROBE PIPELINE (probe_pipeline.cpp):
//    - ProbePipeline wraps probe filtering with multi-phase support
//    - Phase 1: Physics-only probe (fast, no GPU)
//    - Phase 2: Low-res render probe (optional, with GPU metrics)
//    - Filter evaluation happens at each phase via finalizePhase()
//
// EVALUATION FLOW:
//    Criteria → ProbeFilter → evaluate(collector, events, scores, predictions) → FilterResult
//
// CRITERION TYPES:
//    - Event: Require an event exists (legacy, for EventDetector events)
//    - EventTiming: Event must occur within time range (legacy)
//    - Metric: Final metric value must meet threshold (e.g., uniformity > 0.9)
//    - Score: Analyzer score must meet threshold (legacy)
//    - TargetFrame: Check PredictionResult for frame target (boom, chaos)
//    - TargetScore: Check PredictionResult for score target (boom_quality)
//
// IMPORTANT: Use TargetFrame/TargetScore for new code. They evaluate against
// PredictionResult objects from the multi-target prediction system.
// =============================================================================

// Result of filter evaluation
struct FilterResult {
    bool passed = false;
    std::string reason;  // Empty if passed, rejection reason otherwise

    operator bool() const { return passed; }

    static FilterResult pass() { return {true, ""}; }
    static FilterResult fail(std::string const& reason) {
        return {false, reason};
    }
};

// Types of filter criteria
enum class FilterCriterionType {
    Event,       // Event must exist (legacy, for EventDetector events)
    EventTiming, // Event must be in time range (legacy)
    Metric,      // Metric must meet threshold (e.g., uniformity)
    Score,       // Analyzer score must meet threshold (legacy)
    TargetFrame, // Check PredictionResult for frame target (boom, chaos)
    TargetScore  // Check PredictionResult for score target (boom_quality)
};

// Single filter criterion
struct FilterCriterion {
    FilterCriterionType type;
    std::string target;  // Event name, metric name, or score name

    // For event checks
    bool require_event = true;

    // For timing checks (seconds)
    std::optional<double> min_time;
    std::optional<double> max_time;

    // For metric/score threshold checks
    std::optional<double> min_value;
    std::optional<double> max_value;

    // Human-readable description
    std::string describe() const;
};

// Probe filter with multiple criteria
class ProbeFilter {
public:
    ProbeFilter() = default;
    ~ProbeFilter() = default;

    // Add legacy criteria (for backward compatibility)
    void addEventRequired(std::string const& event_name);
    void addEventTiming(std::string const& event_name, double min_seconds,
                        double max_seconds);
    void addScoreThreshold(std::string const& score_name, double min_value);

    // Add metric threshold (still used for general constraints like uniformity)
    void addMetricThreshold(std::string const& metric_name, double min_value);
    void addMetricRange(std::string const& metric_name, double min_value,
                        double max_value);

    // Add target constraint (new system - evaluates against PredictionResult)
    void addTargetConstraint(std::string const& target_name, bool required,
                             std::optional<double> min_seconds,
                             std::optional<double> max_seconds,
                             std::optional<double> min_score,
                             std::optional<double> max_score);

    // Add custom criterion
    void addCriterion(FilterCriterion const& criterion);

    // Clear all criteria
    void clearCriteria();

    // Evaluate filter against collected data and predictions
    // This is the primary evaluation method for the new target-based system
    FilterResult evaluate(MetricsCollector const& collector,
                          EventDetector const& events,
                          SimulationScore const& scores,
                          std::vector<optimize::PredictionResult> const& predictions) const;

    // Legacy evaluate without predictions (for backward compatibility)
    FilterResult evaluate(MetricsCollector const& collector,
                          EventDetector const& events,
                          SimulationScore const& scores = {}) const {
        return evaluate(collector, events, scores, {});
    }

    // Get all criteria
    std::vector<FilterCriterion> const& getCriteria() const { return criteria_; }

    // Check if any criteria are defined
    bool empty() const { return criteria_.empty(); }

    // Get human-readable description of all criteria
    std::string describe() const;

private:
    std::vector<FilterCriterion> criteria_;

    FilterResult evaluateCriterion(FilterCriterion const& criterion,
                                   MetricsCollector const& collector,
                                   EventDetector const& events,
                                   SimulationScore const& scores,
                                   std::vector<optimize::PredictionResult> const& predictions) const;
};

} // namespace metrics
