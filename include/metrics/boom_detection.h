#pragma once

// ============================================================================
// BACKWARD COMPATIBILITY WRAPPER
// ============================================================================
// This file provides backward compatibility for code using the old boom
// detection API. It wraps the new optimize::FrameDetector system.
//
// For new code, prefer using optimize::FrameDetector directly.
// ============================================================================

#include "config.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "optimize/frame_detector.h"
#include "optimize/prediction_target.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace metrics {

// Boom detection result (mirrors optimize::FrameDetection)
struct BoomDetection {
    int frame = -1;
    double seconds = 0.0;
    double causticness = 0.0;  // Peak causticness value (metric_value in FrameDetection)
    BoomDetectionMethod method_used = BoomDetectionMethod::MaxCausticness;

    // Convert from FrameDetection
    static BoomDetection fromFrameDetection(optimize::FrameDetection const& fd) {
        BoomDetection result;
        result.frame = fd.frame;
        result.seconds = fd.seconds;
        result.causticness = fd.metric_value;
        // Convert method
        switch (fd.method_used) {
        case optimize::FrameDetectionMethod::MaxValue:
            result.method_used = BoomDetectionMethod::MaxCausticness;
            break;
        case optimize::FrameDetectionMethod::FirstPeakPercent:
            result.method_used = BoomDetectionMethod::FirstPeakPercent;
            break;
        case optimize::FrameDetectionMethod::DerivativePeak:
            result.method_used = BoomDetectionMethod::DerivativePeak;
            break;
        case optimize::FrameDetectionMethod::ThresholdCrossing:
            result.method_used = BoomDetectionMethod::ThresholdCrossing;
            break;
        case optimize::FrameDetectionMethod::SecondDerivativePeak:
            result.method_used = BoomDetectionMethod::SecondDerivativePeak;
            break;
        }
        return result;
    }
};

// Convert BoomDetectionParams to FrameDetectionParams
inline optimize::FrameDetectionParams toFrameDetectionParams(BoomDetectionParams const& bp) {
    optimize::FrameDetectionParams fp;
    switch (bp.method) {
    case BoomDetectionMethod::MaxCausticness:
        fp.method = optimize::FrameDetectionMethod::MaxValue;
        break;
    case BoomDetectionMethod::FirstPeakPercent:
        fp.method = optimize::FrameDetectionMethod::FirstPeakPercent;
        break;
    case BoomDetectionMethod::DerivativePeak:
        fp.method = optimize::FrameDetectionMethod::DerivativePeak;
        break;
    case BoomDetectionMethod::ThresholdCrossing:
        fp.method = optimize::FrameDetectionMethod::ThresholdCrossing;
        break;
    case BoomDetectionMethod::SecondDerivativePeak:
        fp.method = optimize::FrameDetectionMethod::SecondDerivativePeak;
        break;
    }
    fp.metric_name = bp.metric_name;
    fp.offset_seconds = bp.offset_seconds;
    fp.peak_percent_threshold = bp.peak_percent_threshold;
    fp.min_peak_prominence = bp.min_peak_prominence;
    fp.smoothing_window = bp.smoothing_window;
    fp.crossing_threshold = bp.crossing_threshold;
    fp.crossing_confirmation = bp.crossing_confirmation;
    return fp;
}

// Boom detector that wraps FrameDetector
class BoomDetector {
public:
    BoomDetector() = default;
    explicit BoomDetector(BoomDetectionParams const& params)
        : params_(params), detector_(toFrameDetectionParams(params)) {}

    void setParams(BoomDetectionParams const& params) {
        params_ = params;
        detector_.setParams(toFrameDetectionParams(params));
    }

    BoomDetectionParams const& getParams() const { return params_; }

    // Main detection entry point - delegates to FrameDetector
    BoomDetection detect(MetricsCollector const& collector,
                         double frame_duration) const {
        auto detection = detector_.detect(collector, frame_duration);
        return BoomDetection::fromFrameDetection(detection);
    }

private:
    BoomDetectionParams params_;
    optimize::FrameDetector detector_;
};

// Legacy function: Find boom frame based on max angular causticness
// Preserved for backward compatibility
inline BoomDetection findBoomFrame(MetricsCollector const& collector,
                                    double frame_duration,
                                    double offset_seconds = 0.3) {
    BoomDetectionParams params;
    params.method = BoomDetectionMethod::MaxCausticness;
    params.offset_seconds = offset_seconds;

    BoomDetector detector(params);
    return detector.detect(collector, frame_duration);
}

// Find boom frame using custom params
inline BoomDetection findBoomFrame(MetricsCollector const& collector,
                                    double frame_duration,
                                    BoomDetectionParams const& params) {
    BoomDetector detector(params);
    return detector.detect(collector, frame_duration);
}

// Force boom event into EventDetector based on causticness detection
// This allows analyzers (e.g., CausticnessAnalyzer) to work with causticness-based boom
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
