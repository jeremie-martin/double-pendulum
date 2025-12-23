#pragma once

#include "metrics/analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

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
//    - ProbeFilter evaluates if the probe passes
//    - Only passing probes proceed to full rendering
//
// 2. PROBE PIPELINE (probe_pipeline.cpp):
//    - ProbePipeline wraps probe filtering with multi-phase support
//    - Phase 1: Physics-only probe (fast, no GPU)
//    - Phase 2: Low-res render probe (optional, with GPU metrics)
//    - Filter evaluation happens at each phase via finalizePhase()
//
// EVALUATION FLOW:
//    Criteria → ProbeFilter → evaluate(collector, events, scores) → FilterResult
//
// CRITERION TYPES:
//    - Event: Require an event exists (e.g., boom detected)
//    - EventTiming: Event must occur within time range
//    - Metric: Final metric value must meet threshold (e.g., uniformity > 0.9)
//    - Score: Analyzer score must meet threshold (e.g., peak_clarity > 0.75)
//
// IMPORTANT: Scores (from analyzers) are only available AFTER analyzers run.
// Make sure to call runPostSimulationAnalysis() or equivalent before filtering.
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
    Event,       // Event must exist (e.g., boom detected)
    EventTiming, // Event must be in time range
    Metric,      // Metric must meet threshold
    Score        // Analyzer score must meet threshold
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

    // Add criteria
    void addEventRequired(std::string const& event_name);
    void addEventTiming(std::string const& event_name, double min_seconds,
                        double max_seconds);
    void addMetricThreshold(std::string const& metric_name, double min_value);
    void addMetricRange(std::string const& metric_name, double min_value,
                        double max_value);
    void addScoreThreshold(std::string const& score_name, double min_value);

    // Add custom criterion
    void addCriterion(FilterCriterion const& criterion);

    // Clear all criteria
    void clearCriteria();

    // Evaluate filter against collected data
    FilterResult evaluate(MetricsCollector const& collector,
                          EventDetector const& events,
                          SimulationScore const& scores = {}) const;

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
                                   SimulationScore const& scores) const;
};

} // namespace metrics
