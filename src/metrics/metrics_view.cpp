#include "metrics/metrics_view.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace metrics {

MetricsView::MetricsView(MetricsCollector const& collector,
                         EventDetector const& events,
                         double frame_duration)
    : frame_duration_(frame_duration) {

    frame_count_ = collector.frameCount();
    if (frame_count_ == 0) {
        return;
    }

    // Build frame indices
    frame_indices_.resize(frame_count_);
    for (int i = 0; i < frame_count_; ++i) {
        frame_indices_[i] = static_cast<double>(i);
    }

    // Copy all metric values and compute derivatives
    for (auto const& name : collector.getMetricNames()) {
        MetricSeries<double> const* series = collector.getMetric(name);
        if (series && !series->empty()) {
            // Copy values
            auto const& vals = series->values();
            values_[name] = vals;

            // Compute derivatives
            derivatives_[name] = series->derivativeHistory();

            // Store type
            metric_types_[name] = collector.getMetricType(name);
        }
    }

    // Copy spread history
    spread_history_ = collector.getSpreadHistory();

    // Copy events
    for (auto const& event : events.getAllEvents()) {
        if (event.confirmed) {
            EventMarker marker;
            marker.name = event.name;
            marker.frame = event.frame;
            marker.seconds = event.seconds;
            marker.value = event.value;
            event_markers_.push_back(marker);
            events_by_name_[event.name] = marker;
        }
    }
}

std::vector<double> const*
MetricsView::getValues(std::string const& metric) const {
    auto it = values_.find(metric);
    if (it != values_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<double> const*
MetricsView::getDerivatives(std::string const& metric) const {
    auto it = derivatives_.find(metric);
    if (it != derivatives_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<double> MetricsView::getSmoothed(std::string const& metric,
                                              int window) const {
    auto it = values_.find(metric);
    if (it == values_.end() || it->second.empty()) {
        return {};
    }

    auto const& vals = it->second;
    std::vector<double> result;
    result.reserve(vals.size());

    int half = window / 2;
    for (size_t i = 0; i < vals.size(); ++i) {
        size_t start = (i >= static_cast<size_t>(half)) ? i - half : 0;
        size_t end = std::min(i + half + 1, vals.size());

        double sum = 0.0;
        for (size_t j = start; j < end; ++j) {
            sum += vals[j];
        }
        result.push_back(sum / static_cast<double>(end - start));
    }

    return result;
}

bool MetricsView::hasMetric(std::string const& metric) const {
    return values_.find(metric) != values_.end();
}

std::vector<std::string> MetricsView::getPhysicsMetrics() const {
    std::vector<std::string> result;
    for (auto const& [name, type] : metric_types_) {
        if (type == MetricType::Physics) {
            result.push_back(name);
        }
    }
    return result;
}

std::vector<std::string> MetricsView::getGPUMetrics() const {
    std::vector<std::string> result;
    for (auto const& [name, type] : metric_types_) {
        if (type == MetricType::GPU) {
            result.push_back(name);
        }
    }
    return result;
}

std::vector<std::string> MetricsView::getDerivedMetrics() const {
    std::vector<std::string> result;
    for (auto const& [name, type] : metric_types_) {
        if (type == MetricType::Derived) {
            result.push_back(name);
        }
    }
    return result;
}

std::vector<std::string> MetricsView::getAllMetrics() const {
    std::vector<std::string> result;
    result.reserve(values_.size());
    for (auto const& [name, _] : values_) {
        result.push_back(name);
    }
    return result;
}

MetricType MetricsView::getMetricType(std::string const& metric) const {
    auto it = metric_types_.find(metric);
    if (it != metric_types_.end()) {
        return it->second;
    }
    return MetricType::Physics;
}

double MetricsView::getCurrentValue(std::string const& metric) const {
    auto it = values_.find(metric);
    if (it != values_.end() && !it->second.empty()) {
        return it->second.back();
    }
    return 0.0;
}

bool MetricsView::hasEvent(std::string const& name) const {
    return events_by_name_.find(name) != events_by_name_.end();
}

std::optional<EventMarker>
MetricsView::getEvent(std::string const& name) const {
    auto it = events_by_name_.find(name);
    if (it != events_by_name_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::pair<double, double>
MetricsView::getRange(std::string const& metric) const {
    auto it = values_.find(metric);
    if (it == values_.end() || it->second.empty()) {
        return {0.0, 1.0};
    }

    auto const& vals = it->second;
    auto [min_it, max_it] = std::minmax_element(vals.begin(), vals.end());
    return {*min_it, *max_it};
}

double MetricsView::getMean(std::string const& metric) const {
    auto it = values_.find(metric);
    if (it == values_.end() || it->second.empty()) {
        return 0.0;
    }

    auto const& vals = it->second;
    return std::accumulate(vals.begin(), vals.end(), 0.0) /
           static_cast<double>(vals.size());
}

double MetricsView::getMax(std::string const& metric) const {
    auto it = values_.find(metric);
    if (it == values_.end() || it->second.empty()) {
        return 0.0;
    }
    return *std::max_element(it->second.begin(), it->second.end());
}

double MetricsView::getMin(std::string const& metric) const {
    auto it = values_.find(metric);
    if (it == values_.end() || it->second.empty()) {
        return 0.0;
    }
    return *std::min_element(it->second.begin(), it->second.end());
}

} // namespace metrics
