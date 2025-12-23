#pragma once

#include "metrics/analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace metrics {

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
