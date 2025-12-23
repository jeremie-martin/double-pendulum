#pragma once

#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

#include <algorithm>
#include <optional>

namespace metrics {

// Boom detection result
struct BoomDetection {
    int frame = -1;
    double seconds = 0.0;
    double causticness = 0.0;  // Peak causticness value
};

// Find boom frame based on max angular causticness
// Returns the frame of peak causticness, offset by a configurable amount
// to better align with visual boom timing.
//
// Parameters:
//   collector: MetricsCollector containing AngularCausticness metric
//   frame_duration: Duration of each frame in seconds
//   offset_seconds: Time offset to apply (default 0.3s)
//
// Returns:
//   BoomDetection with frame, seconds, and causticness value
//   frame will be -1 if no causticness data available
inline BoomDetection findBoomFrame(MetricsCollector const& collector,
                                    double frame_duration,
                                    double offset_seconds = 0.3) {
    BoomDetection result;

    auto const* caustic_series =
        collector.getMetric(MetricNames::AngularCausticness);
    if (!caustic_series || caustic_series->empty()) {
        return result;
    }

    auto const& values = caustic_series->values();
    auto max_it = std::max_element(values.begin(), values.end());
    int max_frame = static_cast<int>(std::distance(values.begin(), max_it));

    // Apply offset for better visual alignment
    int offset_frames = static_cast<int>(offset_seconds / frame_duration);
    result.frame = std::max(0, max_frame - offset_frames);
    result.seconds = result.frame * frame_duration;
    result.causticness = *max_it;

    return result;
}

// Force boom event into EventDetector based on causticness detection
// This allows analyzers (like BoomAnalyzer) to work with causticness-based boom
inline void forceBoomEvent(EventDetector& detector,
                           BoomDetection const& boom,
                           double variance_at_boom = 0.0) {
    if (boom.frame < 0) {
        return;
    }

    DetectedEvent event;
    event.frame = boom.frame;
    event.seconds = boom.seconds;
    event.value = variance_at_boom;  // For backwards compatibility
    // Note: sharpness_ratio will be 0 since we're not using threshold crossing
    detector.forceEvent(EventNames::Boom, event);
}

} // namespace metrics
