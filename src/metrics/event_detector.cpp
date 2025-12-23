#include "metrics/event_detector.h"

#include <cmath>

namespace metrics {

void EventDetector::addCriteria(std::string const& event_name,
                                 EventCriteria const& criteria) {
    criteria_[event_name] = criteria;
    detection_state_[event_name] = DetectionState{};
}

void EventDetector::removeCriteria(std::string const& event_name) {
    criteria_.erase(event_name);
    detection_state_.erase(event_name);
    detected_events_.erase(event_name);
    callbacks_.erase(event_name);
}

void EventDetector::clearCriteria() {
    criteria_.clear();
    detection_state_.clear();
    detected_events_.clear();
    // Keep callbacks - they might be re-added
}

void EventDetector::addBoomCriteria(double threshold, int confirm,
                                     std::string const& metric) {
    EventCriteria criteria;
    criteria.metric_name = metric;
    criteria.threshold = threshold;
    criteria.confirmation_frames = confirm;
    criteria.use_derivative = false;
    criteria.above_threshold = true;
    addCriteria(EventNames::Boom, criteria);
}

void EventDetector::addChaosCriteria(double threshold, int confirm,
                                      std::string const& metric) {
    EventCriteria criteria;
    criteria.metric_name = metric;
    criteria.threshold = threshold;
    criteria.confirmation_frames = confirm;
    criteria.use_derivative = false;
    criteria.above_threshold = true;
    addCriteria(EventNames::Chaos, criteria);
}

void EventDetector::update(MetricsCollector const& collector,
                            double frame_duration_seconds) {
    frame_duration_ = frame_duration_seconds;

    for (auto const& [name, criteria] : criteria_) {
        // Skip if already detected
        auto it = detected_events_.find(name);
        if (it != detected_events_.end() && it->second.confirmed) {
            continue;
        }

        // For chaos, only detect after boom
        if (name == EventNames::Chaos) {
            auto boom_it = detected_events_.find(EventNames::Boom);
            if (boom_it == detected_events_.end() || !boom_it->second.confirmed) {
                continue;
            }
        }

        checkEvent(name, criteria, collector);
    }
}

void EventDetector::reset() {
    detection_state_.clear();
    detected_events_.clear();

    // Re-initialize detection state for all criteria
    for (auto const& [name, _] : criteria_) {
        detection_state_[name] = DetectionState{};
    }
}

std::optional<DetectedEvent>
EventDetector::getEvent(std::string const& name) const {
    auto it = detected_events_.find(name);
    if (it != detected_events_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<DetectedEvent> EventDetector::getAllEvents() const {
    std::vector<DetectedEvent> events;
    events.reserve(detected_events_.size());
    for (auto const& [name, event] : detected_events_) {
        events.push_back(event);
    }
    return events;
}

bool EventDetector::hasEvent(std::string const& name) const {
    return detected_events_.find(name) != detected_events_.end();
}

bool EventDetector::isDetected(std::string const& name) const {
    auto it = detected_events_.find(name);
    return it != detected_events_.end() && it->second.confirmed;
}

void EventDetector::forceEvent(std::string const& name, DetectedEvent const& event) {
    DetectedEvent forced_event = event;
    forced_event.name = name;
    forced_event.confirmed = true;
    detected_events_[name] = forced_event;
}

std::vector<std::string> EventDetector::getEventNames() const {
    std::vector<std::string> names;
    names.reserve(criteria_.size());
    for (auto const& [name, _] : criteria_) {
        names.push_back(name);
    }
    return names;
}

void EventDetector::onEvent(std::string const& name, EventCallback callback) {
    callbacks_[name].push_back(std::move(callback));
}

EventCriteria const*
EventDetector::getCriteria(std::string const& name) const {
    auto it = criteria_.find(name);
    if (it != criteria_.end()) {
        return &it->second;
    }
    return nullptr;
}

void EventDetector::checkEvent(std::string const& name,
                                EventCriteria const& criteria,
                                MetricsCollector const& collector) {
    MetricSeries<double> const* series = collector.getMetric(criteria.metric_name);
    if (!series || series->empty()) {
        return;
    }

    DetectionState& state = detection_state_[name];

    // Get the current value (or derivative)
    double current_value;
    double current_derivative = series->derivative();

    if (criteria.use_derivative) {
        current_value = current_derivative;
    } else {
        current_value = series->current();
    }

    // Check if threshold is crossed
    bool crosses = criteria.above_threshold
                       ? (current_value > criteria.threshold)
                       : (current_value < criteria.threshold);

    if (crosses) {
        if (state.consecutive_frames == 0) {
            // First crossing
            state.first_cross_frame = static_cast<int>(series->size()) - 1;
            state.first_cross_value = series->current();
            state.first_cross_derivative = current_derivative;
        }
        state.consecutive_frames++;

        if (state.consecutive_frames >= criteria.confirmation_frames) {
            // Event confirmed!
            DetectedEvent event;
            event.name = name;
            event.frame = state.first_cross_frame;
            event.seconds = static_cast<double>(state.first_cross_frame) *
                           frame_duration_;
            event.value = state.first_cross_value;
            event.derivative = state.first_cross_derivative;

            // Compute sharpness ratio
            if (std::abs(criteria.threshold) > 1e-10) {
                event.sharpness_ratio = std::abs(state.first_cross_derivative) /
                                        std::abs(criteria.threshold);
            }

            event.confirmed = true;
            detected_events_[name] = event;

            // Trigger callbacks
            triggerCallbacks(name, event);
        }
    } else {
        // Reset consecutive counter
        state.consecutive_frames = 0;
        state.first_cross_frame = -1;
    }
}

void EventDetector::triggerCallbacks(std::string const& name,
                                      DetectedEvent const& event) {
    auto it = callbacks_.find(name);
    if (it != callbacks_.end()) {
        for (auto const& callback : it->second) {
            callback(event);
        }
    }
}

} // namespace metrics
