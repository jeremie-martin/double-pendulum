#pragma once

#include "metrics/analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace metrics {

// Event marker for GUI display
struct EventMarker {
    std::string name;
    int frame = -1;
    double seconds = 0.0;
    double value = 0.0;
};

// Immutable view of metrics for thread-safe GUI access
// Takes a snapshot of the current state that won't change
class MetricsView {
public:
    // Create a view from current state
    MetricsView(MetricsCollector const& collector,
                EventDetector const& events,
                double frame_duration);

    // Create empty view
    MetricsView() = default;

    // Check if view has data
    bool empty() const { return frame_count_ == 0; }
    int frameCount() const { return frame_count_; }

    // Get frame indices (for x-axis in plots)
    std::vector<double> const& getFrameIndices() const { return frame_indices_; }

    // Get metric values
    std::vector<double> const* getValues(std::string const& metric) const;

    // Get derivative values
    std::vector<double> const* getDerivatives(std::string const& metric) const;

    // Get smoothed values
    std::vector<double> getSmoothed(std::string const& metric,
                                     int window = 5) const;

    // Check if metric exists
    bool hasMetric(std::string const& metric) const;

    // Get all metric names by type
    std::vector<std::string> getPhysicsMetrics() const;
    std::vector<std::string> getGPUMetrics() const;
    std::vector<std::string> getDerivedMetrics() const;
    std::vector<std::string> getAllMetrics() const;

    // Get metric type
    MetricType getMetricType(std::string const& metric) const;

    // Get current (latest) value for a metric
    double getCurrentValue(std::string const& metric) const;

    // Get event markers for display on graph
    std::vector<EventMarker> getEventMarkers() const { return event_markers_; }

    // Check if a specific event was detected
    bool hasEvent(std::string const& name) const;
    std::optional<EventMarker> getEvent(std::string const& name) const;

    // Get spread metrics history
    std::vector<SpreadMetrics> const& getSpreadHistory() const {
        return spread_history_;
    }

    // Frame duration
    double frameDuration() const { return frame_duration_; }

    // Convert frame to seconds
    double frameToSeconds(int frame) const {
        return frame * frame_duration_;
    }

    // Get min/max for a metric (for auto-scaling plots)
    std::pair<double, double> getRange(std::string const& metric) const;

    // Statistics
    double getMean(std::string const& metric) const;
    double getMax(std::string const& metric) const;
    double getMin(std::string const& metric) const;

private:
    int frame_count_ = 0;
    double frame_duration_ = 0.0;

    std::vector<double> frame_indices_;

    // Cached metric values (name -> values)
    std::unordered_map<std::string, std::vector<double>> values_;
    std::unordered_map<std::string, std::vector<double>> derivatives_;
    std::unordered_map<std::string, MetricType> metric_types_;

    // Spread metrics history
    std::vector<SpreadMetrics> spread_history_;

    // Event markers
    std::vector<EventMarker> event_markers_;
    std::unordered_map<std::string, EventMarker> events_by_name_;
};

} // namespace metrics
