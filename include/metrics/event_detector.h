#pragma once

#include "metrics/metric_series.h"
#include "metrics/metrics_collector.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace metrics {

// Configurable event detection criteria
struct EventCriteria {
    std::string metric_name;        // Which metric to watch
    double threshold = 0.0;         // Threshold value
    int confirmation_frames = 10;   // Consecutive frames needed
    bool use_derivative = false;    // Detect based on derivative instead
    bool above_threshold = true;    // Above vs below threshold

    // Builder pattern for fluent API
    EventCriteria& metric(std::string const& name) {
        metric_name = name;
        return *this;
    }
    EventCriteria& thresh(double t) {
        threshold = t;
        return *this;
    }
    EventCriteria& confirm(int frames) {
        confirmation_frames = frames;
        return *this;
    }
    EventCriteria& derivative(bool d = true) {
        use_derivative = d;
        return *this;
    }
    EventCriteria& below(bool b = true) {
        above_threshold = !b;
        return *this;
    }
};

// Detected event with quality metrics
struct DetectedEvent {
    std::string name;               // Event name (e.g., "boom", "chaos")
    int frame = -1;                 // Frame when detected (-1 = not detected)
    double seconds = 0.0;           // Time in seconds
    double value = 0.0;             // Metric value at detection
    double derivative = 0.0;        // Derivative at detection (for sharpness)
    double sharpness_ratio = 0.0;   // derivative / threshold (quality metric)
    bool confirmed = false;         // True if fully confirmed

    bool detected() const { return frame >= 0; }
};

// Generic event detection engine
class EventDetector {
public:
    using EventCallback = std::function<void(DetectedEvent const&)>;

    EventDetector() = default;
    ~EventDetector() = default;

    // Configure events to detect
    void addCriteria(std::string const& event_name, EventCriteria const& criteria);
    void removeCriteria(std::string const& event_name);
    void clearCriteria();

    // Convenience methods for common events
    void addBoomCriteria(double threshold = 0.1, int confirm = 10,
                         std::string const& metric = "variance");
    void addChaosCriteria(double threshold = 700.0, int confirm = 10,
                          std::string const& metric = "variance");

    // Update detection state (called each frame)
    void update(MetricsCollector const& collector, double frame_duration_seconds);

    // Reset detection state
    void reset();

    // Access results
    std::optional<DetectedEvent> getEvent(std::string const& name) const;
    std::vector<DetectedEvent> getAllEvents() const;
    bool hasEvent(std::string const& name) const;
    bool isDetected(std::string const& name) const;

    // Manually set an event (for use with pre-computed results)
    void forceEvent(std::string const& name, DetectedEvent const& event);

    // Get all event names that have been configured
    std::vector<std::string> getEventNames() const;

    // Event callbacks (for early termination hooks, logging, etc.)
    void onEvent(std::string const& name, EventCallback callback);

    // Get the criteria for an event
    EventCriteria const* getCriteria(std::string const& name) const;

private:
    struct DetectionState {
        int consecutive_frames = 0;
        int first_cross_frame = -1;
        double first_cross_value = 0.0;
        double first_cross_derivative = 0.0;
    };

    std::unordered_map<std::string, EventCriteria> criteria_;
    std::unordered_map<std::string, DetectedEvent> detected_events_;
    std::unordered_map<std::string, DetectionState> detection_state_;
    std::unordered_map<std::string, std::vector<EventCallback>> callbacks_;

    double frame_duration_ = 0.0;

    void checkEvent(std::string const& name, EventCriteria const& criteria,
                    MetricsCollector const& collector);

    void triggerCallbacks(std::string const& name, DetectedEvent const& event);
};

// Standard event names
namespace EventNames {
constexpr const char* Boom = "boom";
constexpr const char* Chaos = "chaos";  // Formerly "white"
} // namespace EventNames

} // namespace metrics
