#pragma once

#include "metrics/boom_detection.h"
#include "metrics/signal_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "optimize/prediction_target.h"

namespace metrics {

// Common initialization for the metrics system.
// This helper ensures consistent setup across all executables.
//
// Usage:
//   metrics::initializeMetricsSystem(collector, detector, signal_analyzer,
//                                    frame_duration, with_gpu);
//
// Parameters:
//   collector: MetricsCollector to register metrics on
//   detector: EventDetector (cleared, events added post-hoc via boom detection)
//   signal_analyzer: SignalAnalyzer to configure
//   frame_duration: Seconds per frame (simulation.duration / total_frames)
//   with_gpu: If true, also register GPU metrics (for rendering modes)
//   analyzer_metric: Which metric the analyzer should use (default: angular_causticness)
//
// Note: Boom and chaos are both detected post-simulation using FrameDetector
// via [targets.X] configuration. The EventDetector is used only for storing
// detected events, not for real-time threshold detection.
inline void initializeMetricsSystem(MetricsCollector& collector,
                                    EventDetector& detector,
                                    SignalAnalyzer& signal_analyzer,
                                    double frame_duration,
                                    bool with_gpu = false,
                                    std::string const& analyzer_metric = MetricNames::AngularCausticness) {
    // Register metrics
    collector.registerStandardMetrics();
    if (with_gpu) {
        collector.registerGPUMetrics();
    }

    // Clear event detector (events are added post-simulation)
    detector.clearCriteria();

    // Configure signal analyzer
    signal_analyzer.setMetricName(analyzer_metric);
    signal_analyzer.setFrameDuration(frame_duration);
}

// Reset all metrics components for a new simulation run
inline void resetMetricsSystem(MetricsCollector& collector,
                               EventDetector& detector,
                               SignalAnalyzer& signal_analyzer) {
    collector.reset();
    detector.reset();
    signal_analyzer.reset();
}

// Run boom detection and analyzers after simulation completes.
// This is the standard pattern used by all executables:
// 1. Find boom frame using configured method and metric (REQUIRED: boom_params must have metric_name set)
// 2. Force boom event into detector for analyzer access
// 3. Run analyzers
//
// Returns the BoomDetection result (frame may be -1 if no boom found or no target configured)
inline BoomDetection runPostSimulationAnalysis(MetricsCollector const& collector,
                                               EventDetector& detector,
                                               SignalAnalyzer& signal_analyzer,
                                               double frame_duration,
                                               optimize::FrameDetectionParams const& boom_params) {
    // Skip boom detection if no metric specified (target not configured)
    BoomDetection boom;
    if (boom_params.metric_name.empty()) {
        boom.frame = -1;  // No boom target configured
    } else {
        boom = findBoomFrame(collector, frame_duration, boom_params);
    }

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

    // Run analyzer (metric name should already be set via initializeMetricsSystem)
    signal_analyzer.analyze(collector, detector);

    return boom;
}

} // namespace metrics
