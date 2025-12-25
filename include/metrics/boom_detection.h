#pragma once

// ============================================================================
// BOOM DETECTION UTILITIES
// ============================================================================
// This file provides convenience utilities for boom frame detection.
// It wraps optimize::FrameDetector for common boom detection use cases.
// ============================================================================

#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "optimize/frame_detector.h"
#include "optimize/prediction_target.h"

#include <optional>

namespace metrics {

// Boom detection result - alias for FrameDetection with a boom-specific name
using BoomDetection = optimize::FrameDetection;

// Find boom frame using default parameters (max angular_causticness with offset)
inline BoomDetection findBoomFrame(MetricsCollector const& collector,
                                    double frame_duration,
                                    double offset_seconds = 0.3) {
    optimize::FrameDetectionParams params;
    params.method = optimize::FrameDetectionMethod::MaxValue;
    params.metric_name = MetricNames::AngularCausticness;
    params.offset_seconds = offset_seconds;

    optimize::FrameDetector detector(params);
    return detector.detect(collector, frame_duration);
}

// Find boom frame using custom FrameDetectionParams
inline BoomDetection findBoomFrame(MetricsCollector const& collector,
                                    double frame_duration,
                                    optimize::FrameDetectionParams const& params) {
    optimize::FrameDetector detector(params);
    return detector.detect(collector, frame_duration);
}

// Force boom event into EventDetector based on frame detection
// This allows analyzers (e.g., CausticnessAnalyzer) to work with detected boom
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
