#pragma once

#include "metrics/boom_detection.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

namespace metrics {

// Common initialization for the metrics system.
// This helper ensures consistent setup across all executables.
//
// Usage:
//   metrics::initializeMetricsSystem(collector, detector, causticness_analyzer,
//                                    config.detection, frame_duration, with_gpu);
//
// Parameters:
//   collector: MetricsCollector to register metrics on
//   detector: EventDetector to configure chaos detection
//   causticness_analyzer: CausticnessAnalyzer to set frame duration
//   chaos_threshold: Variance threshold for chaos detection (e.g., 700.0)
//   chaos_confirmation: Frames above threshold to confirm chaos (e.g., 10)
//   frame_duration: Seconds per frame (simulation.duration / total_frames)
//   with_gpu: If true, also register GPU metrics (for rendering modes)
//
// Note: Boom detection uses max causticness (via findBoomFrame/forceBoomEvent)
// rather than threshold crossing. Chaos detection still uses threshold.
inline void initializeMetricsSystem(MetricsCollector& collector,
                                    EventDetector& detector,
                                    CausticnessAnalyzer& causticness_analyzer,
                                    double chaos_threshold,
                                    int chaos_confirmation,
                                    double frame_duration,
                                    bool with_gpu = false) {
    // Register metrics
    collector.registerStandardMetrics();
    if (with_gpu) {
        collector.registerGPUMetrics();
    }

    // Configure event detection
    // Note: Only chaos uses threshold detection. Boom is detected via
    // max angular causticness using findBoomFrame() after simulation.
    detector.clearCriteria();
    detector.addChaosCriteria(chaos_threshold, chaos_confirmation,
                              MetricNames::Variance);

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
                                               BoomDetectionParams const& boom_params = {}) {
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
