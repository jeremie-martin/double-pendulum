#include "metrics/probe_filter.h"

#include <algorithm>
#include <sstream>

namespace metrics {

std::string FilterCriterion::describe() const {
    std::ostringstream oss;

    switch (type) {
    case FilterCriterionType::Event:
        oss << "require " << target << " event";
        break;

    case FilterCriterionType::EventTiming:
        oss << target << " timing: ";
        if (min_time)
            oss << ">= " << *min_time << "s";
        if (min_time && max_time)
            oss << ", ";
        if (max_time)
            oss << "<= " << *max_time << "s";
        break;

    case FilterCriterionType::Metric:
        oss << target << ": ";
        if (min_value)
            oss << ">= " << *min_value;
        if (min_value && max_value)
            oss << ", ";
        if (max_value)
            oss << "<= " << *max_value;
        break;

    case FilterCriterionType::Score:
        oss << target << " score >= " << (min_value ? *min_value : 0.0);
        break;

    case FilterCriterionType::TargetFrame:
        oss << "target " << target << " frame: ";
        if (require_event)
            oss << "required, ";
        if (min_time)
            oss << ">= " << *min_time << "s";
        if (min_time && max_time)
            oss << ", ";
        if (max_time)
            oss << "<= " << *max_time << "s";
        break;

    case FilterCriterionType::TargetScore:
        oss << "target " << target << " score: ";
        if (require_event)
            oss << "required, ";
        if (min_value)
            oss << ">= " << *min_value;
        if (min_value && max_value)
            oss << ", ";
        if (max_value)
            oss << "<= " << *max_value;
        break;
    }

    return oss.str();
}

void ProbeFilter::addEventRequired(std::string const& event_name) {
    FilterCriterion c;
    c.type = FilterCriterionType::Event;
    c.target = event_name;
    c.require_event = true;
    criteria_.push_back(c);
}

void ProbeFilter::addEventTiming(std::string const& event_name,
                                  double min_seconds, double max_seconds) {
    FilterCriterion c;
    c.type = FilterCriterionType::EventTiming;
    c.target = event_name;
    c.min_time = min_seconds;
    c.max_time = max_seconds;
    criteria_.push_back(c);
}

void ProbeFilter::addMetricThreshold(std::string const& metric_name,
                                      double min_value) {
    FilterCriterion c;
    c.type = FilterCriterionType::Metric;
    c.target = metric_name;
    c.min_value = min_value;
    criteria_.push_back(c);
}

void ProbeFilter::addMetricRange(std::string const& metric_name,
                                  double min_value, double max_value) {
    FilterCriterion c;
    c.type = FilterCriterionType::Metric;
    c.target = metric_name;
    c.min_value = min_value;
    c.max_value = max_value;
    criteria_.push_back(c);
}

void ProbeFilter::addScoreThreshold(std::string const& score_name,
                                     double min_value) {
    FilterCriterion c;
    c.type = FilterCriterionType::Score;
    c.target = score_name;
    c.min_value = min_value;
    criteria_.push_back(c);
}

void ProbeFilter::addTargetConstraint(std::string const& target_name, bool required,
                                       std::optional<double> min_seconds,
                                       std::optional<double> max_seconds,
                                       std::optional<double> min_score,
                                       std::optional<double> max_score) {
    // Determine if this is a frame or score constraint based on which params are set
    bool is_frame = min_seconds.has_value() || max_seconds.has_value();
    bool is_score = min_score.has_value() || max_score.has_value();

    // If both are set, add two criteria (one for frame, one for score)
    // If neither are set but required=true, default to frame constraint
    if (is_frame || (!is_score && required)) {
        FilterCriterion c;
        c.type = FilterCriterionType::TargetFrame;
        c.target = target_name;
        c.require_event = required;
        c.min_time = min_seconds;
        c.max_time = max_seconds;
        criteria_.push_back(c);
    }

    if (is_score) {
        FilterCriterion c;
        c.type = FilterCriterionType::TargetScore;
        c.target = target_name;
        c.require_event = required;
        c.min_value = min_score;
        c.max_value = max_score;
        criteria_.push_back(c);
    }
}

void ProbeFilter::addCriterion(FilterCriterion const& criterion) {
    criteria_.push_back(criterion);
}

void ProbeFilter::clearCriteria() {
    criteria_.clear();
}

FilterResult ProbeFilter::evaluate(MetricsCollector const& collector,
                                    EventDetector const& events,
                                    SimulationScore const& scores,
                                    std::vector<optimize::PredictionResult> const& predictions) const {
    for (auto const& criterion : criteria_) {
        FilterResult result =
            evaluateCriterion(criterion, collector, events, scores, predictions);
        if (!result.passed) {
            return result;
        }
    }
    return FilterResult::pass();
}

std::string ProbeFilter::describe() const {
    std::ostringstream oss;
    oss << "ProbeFilter with " << criteria_.size() << " criteria:\n";
    for (size_t i = 0; i < criteria_.size(); ++i) {
        oss << "  " << (i + 1) << ". " << criteria_[i].describe() << "\n";
    }
    return oss.str();
}

FilterResult ProbeFilter::evaluateCriterion(
    FilterCriterion const& criterion, MetricsCollector const& collector,
    EventDetector const& events, SimulationScore const& scores,
    std::vector<optimize::PredictionResult> const& predictions) const {

    switch (criterion.type) {
    case FilterCriterionType::Event: {
        if (criterion.require_event) {
            if (!events.isDetected(criterion.target)) {
                return FilterResult::fail("no " + criterion.target +
                                          " detected");
            }
        }
        break;
    }

    case FilterCriterionType::EventTiming: {
        auto event = events.getEvent(criterion.target);
        if (!event || !event->detected()) {
            return FilterResult::fail("no " + criterion.target + " detected");
        }

        if (criterion.min_time && event->seconds < *criterion.min_time) {
            std::ostringstream oss;
            oss << criterion.target << " too early (" << event->seconds
                << "s < " << *criterion.min_time << "s)";
            return FilterResult::fail(oss.str());
        }

        if (criterion.max_time && event->seconds > *criterion.max_time) {
            std::ostringstream oss;
            oss << criterion.target << " too late (" << event->seconds
                << "s > " << *criterion.max_time << "s)";
            return FilterResult::fail(oss.str());
        }
        break;
    }

    case FilterCriterionType::Metric: {
        MetricSeries<double> const* series =
            collector.getMetric(criterion.target);
        if (!series || series->empty()) {
            return FilterResult::fail("metric " + criterion.target +
                                      " not available");
        }

        // Check final value (or could check max/mean depending on use case)
        double value = series->current();

        if (criterion.min_value && value < *criterion.min_value) {
            std::ostringstream oss;
            oss << criterion.target << " too low (" << value << " < "
                << *criterion.min_value << ")";
            return FilterResult::fail(oss.str());
        }

        if (criterion.max_value && value > *criterion.max_value) {
            std::ostringstream oss;
            oss << criterion.target << " too high (" << value << " > "
                << *criterion.max_value << ")";
            return FilterResult::fail(oss.str());
        }
        break;
    }

    case FilterCriterionType::Score: {
        if (!scores.has(criterion.target)) {
            return FilterResult::fail("score " + criterion.target +
                                      " not available");
        }

        double value = scores.get(criterion.target);
        if (criterion.min_value && value < *criterion.min_value) {
            std::ostringstream oss;
            oss << criterion.target << " score too low (" << value << " < "
                << *criterion.min_value << ")";
            return FilterResult::fail(oss.str());
        }
        break;
    }

    case FilterCriterionType::TargetFrame: {
        // Find matching prediction by target name
        auto it = std::find_if(predictions.begin(), predictions.end(),
            [&](auto const& p) { return p.target_name == criterion.target; });

        if (it == predictions.end() || it->predicted_frame < 0) {
            if (criterion.require_event) {
                return FilterResult::fail("target '" + criterion.target +
                                          "' not detected");
            }
            // Not required and not found - skip
            break;
        }

        // Check timing bounds
        if (criterion.min_time && it->predicted_seconds < *criterion.min_time) {
            std::ostringstream oss;
            oss << criterion.target << " too early (" << it->predicted_seconds
                << "s < " << *criterion.min_time << "s)";
            return FilterResult::fail(oss.str());
        }

        if (criterion.max_time && it->predicted_seconds > *criterion.max_time) {
            std::ostringstream oss;
            oss << criterion.target << " too late (" << it->predicted_seconds
                << "s > " << *criterion.max_time << "s)";
            return FilterResult::fail(oss.str());
        }
        break;
    }

    case FilterCriterionType::TargetScore: {
        // Find matching prediction by target name
        auto it = std::find_if(predictions.begin(), predictions.end(),
            [&](auto const& p) { return p.target_name == criterion.target; });

        if (it == predictions.end()) {
            if (criterion.require_event) {
                return FilterResult::fail("target '" + criterion.target +
                                          "' not found");
            }
            // Not required and not found - skip
            break;
        }

        if (criterion.min_value && it->predicted_score < *criterion.min_value) {
            std::ostringstream oss;
            oss << criterion.target << " score too low (" << it->predicted_score
                << " < " << *criterion.min_value << ")";
            return FilterResult::fail(oss.str());
        }

        if (criterion.max_value && it->predicted_score > *criterion.max_value) {
            std::ostringstream oss;
            oss << criterion.target << " score too high (" << it->predicted_score
                << " > " << *criterion.max_value << ")";
            return FilterResult::fail(oss.str());
        }
        break;
    }
    }

    return FilterResult::pass();
}

} // namespace metrics
