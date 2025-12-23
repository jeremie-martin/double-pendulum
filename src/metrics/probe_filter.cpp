#include "metrics/probe_filter.h"

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

void ProbeFilter::addCriterion(FilterCriterion const& criterion) {
    criteria_.push_back(criterion);
}

void ProbeFilter::clearCriteria() {
    criteria_.clear();
}

FilterResult ProbeFilter::evaluate(MetricsCollector const& collector,
                                    EventDetector const& events,
                                    SimulationScore const& scores) const {
    for (auto const& criterion : criteria_) {
        FilterResult result =
            evaluateCriterion(criterion, collector, events, scores);
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
    EventDetector const& events, SimulationScore const& scores) const {

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
    }

    return FilterResult::pass();
}

} // namespace metrics
