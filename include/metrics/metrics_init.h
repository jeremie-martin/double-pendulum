#pragma once

#include "metrics/boom_detection.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "optimize/prediction_target.h"

namespace metrics {

// Common initialization for the metrics system.
// This helper ensures consistent setup across all executables.
//
// Usage:
//   metrics::initializeMetricsSystem(collector, detector, causticness_analyzer,
//                                    frame_duration, with_gpu);
//
// Parameters:
//   collector: MetricsCollector to register metrics on
//   detector: EventDetector (cleared, events added post-hoc via boom detection)
//   causticness_analyzer: CausticnessAnalyzer to set frame duration
//   frame_duration: Seconds per frame (simulation.duration / total_frames)
//   with_gpu: If true, also register GPU metrics (for rendering modes)
//
// Note: Boom and chaos are both detected post-simulation using FrameDetector
// via [targets.X] configuration. The EventDetector is used only for storing
// detected events, not for real-time threshold detection.
inline void initializeMetricsSystem(MetricsCollector& collector,
                                    EventDetector& detector,
                                    CausticnessAnalyzer& causticness_analyzer,
                                    double frame_duration,
                                    bool with_gpu = false) {
    // Register metrics
    collector.registerStandardMetrics();
    if (with_gpu) {
        collector.registerGPUMetrics();
    }

    // Clear event detector (events are added post-simulation)
    detector.clearCriteria();

    // Set frame duration for time-based calculations
    causticness_analyzer.setFrameDuration(frame_duration);
}

// Reset all metrics components for a new simulation run
inline void resetMetricsSystem(MetricsCollector& collector,
                               EventDetector& detector,
                               CausticnessAnalyzer& causticness_analyzer) {
    collector.reset();
    detector.reset();
    causticness_analyzer.reset();
}

// Run boom detection and analyzers after simulation completes.
// This is the standard pattern used by all executables:
// 1. Find boom frame using configured method and metric
// 2. Force boom event into detector for analyzer access
// 3. Run analyzers
//
// Returns the BoomDetection result (frame may be -1 if no boom found)
inline BoomDetection runPostSimulationAnalysis(MetricsCollector const& collector,
                                               EventDetector& detector,
                                               CausticnessAnalyzer& causticness_analyzer,
                                               double frame_duration,
                                               optimize::FrameDetectionParams const& boom_params = {}) {
    // Detect boom using configured method and metric
    auto boom = findBoomFrame(collector, frame_duration, boom_params);

    if (boom.frame >= 0) {
        // Get variance at boom for the event
        double variance_at_boom = 0.0;
        if (auto const* var_series = collector.getMetric(MetricNames::Variance)) {
            if (boom.frame < static_cast<int>(var_series->size())) {
                variance_at_boom = var_series->at(boom.frame);
            }
        }
        forceBoomEvent(detector, boom, variance_at_boom);
    }

    // Run analyzer
    causticness_analyzer.analyze(collector, detector);

    return boom;
}

} // namespace metrics
