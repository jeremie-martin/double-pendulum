#pragma once

#include "metrics/boom_detection.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "optimize/prediction_target.h"

namespace metrics {

// ============================================================================
// CHAOS DETECTION: Two Separate Systems
// ============================================================================
//
// There are TWO chaos detection mechanisms that serve different purposes:
//
// 1. EventDetector (real-time, during simulation):
//    - Configured via [detection] section: chaos_threshold, chaos_confirmation
//    - Uses ABSOLUTE variance threshold (e.g., 700.0 radians²)
//    - Triggers during simulation loop
//    - Can enable early_stop_after_chaos feature
//    - Result stored in results.chaos_frame, results.chaos_variance
//
// 2. FrameDetector (post-hoc, via [targets.chaos]):
//    - Configured via [targets.chaos] section with method/params
//    - Uses RELATIVE threshold (e.g., 0.8 = 80% of max value)
//    - Runs after simulation completes
//    - More flexible detection methods available
//    - Result stored in results.predictions[]
//
// These systems may detect chaos at DIFFERENT frames due to different
// threshold semantics (absolute vs relative).
//
// ============================================================================

// Common initialization for the metrics system.
// This helper ensures consistent setup across all executables.
//
// Usage:
//   metrics::initializeMetricsSystem(collector, detector, causticness_analyzer,
//                                    config.detection, frame_duration, with_gpu);
//
// Parameters:
//   collector: MetricsCollector to register metrics on
//   detector: EventDetector to configure chaos detection (real-time)
//   causticness_analyzer: CausticnessAnalyzer to set frame duration
//   chaos_threshold: ABSOLUTE variance threshold for chaos detection (e.g., 700.0)
//   chaos_confirmation: Frames above threshold to confirm chaos (e.g., 10)
//   frame_duration: Seconds per frame (simulation.duration / total_frames)
//   with_gpu: If true, also register GPU metrics (for rendering modes)
//
// Note: This configures EventDetector for real-time chaos detection.
// Post-hoc chaos prediction via [targets.chaos] is handled separately
// using FrameDetector with relative thresholds.
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
