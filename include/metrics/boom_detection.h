#pragma once

#include "config.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace metrics {

// Boom detection result
struct BoomDetection {
    int frame = -1;
    double seconds = 0.0;
    double causticness = 0.0;  // Peak causticness value
    BoomDetectionMethod method_used = BoomDetectionMethod::MaxCausticness;
};

// Enhanced boom detector with multiple detection methods
class BoomDetector {
public:
    BoomDetector() = default;
    explicit BoomDetector(BoomDetectionParams const& params) : params_(params) {}

    void setParams(BoomDetectionParams const& params) { params_ = params; }
    BoomDetectionParams const& getParams() const { return params_; }

    // Main detection entry point
    BoomDetection detect(MetricsCollector const& collector,
                         double frame_duration) const {
        auto const* caustic_series =
            collector.getMetric(params_.metric_name);
        if (!caustic_series || caustic_series->empty()) {
            // Fall back to AngularCausticness if specified metric not found
            caustic_series = collector.getMetric(MetricNames::AngularCausticness);
            if (!caustic_series || caustic_series->empty()) {
                return BoomDetection{};
            }
        }

        auto const& values = caustic_series->values();

        BoomDetection result;
        switch (params_.method) {
        case BoomDetectionMethod::MaxCausticness:
            result = detectMaxCausticness(values, frame_duration);
            break;
        case BoomDetectionMethod::FirstPeakPercent:
            result = detectFirstPeakPercent(values, frame_duration);
            break;
        case BoomDetectionMethod::DerivativePeak:
            result = detectDerivativePeak(values, frame_duration);
            break;
        default:
            result = detectMaxCausticness(values, frame_duration);
            break;
        }

        // Apply offset (common to all methods) for visual alignment
        // Negative offset = earlier, Positive offset = later
        if (result.frame >= 0 && std::abs(params_.offset_seconds) > 1e-9) {
            int offset_frames = static_cast<int>(params_.offset_seconds / frame_duration);
            result.frame = std::max(0, std::min(static_cast<int>(values.size()) - 1,
                                                 result.frame - offset_frames));
            result.seconds = result.frame * frame_duration;
            // Update causticness to match the offset frame
            if (result.frame >= 0 && result.frame < static_cast<int>(values.size())) {
                result.causticness = values[result.frame];
            }
        }

        return result;
    }

private:
    BoomDetectionParams params_;

    // Method 1: Find frame with maximum causticness (peak visual richness)
    // This is the original behavior - finds when caustics are most prominent
    // Note: offset is applied centrally in detect() for all methods
    BoomDetection detectMaxCausticness(std::vector<double> const& values,
                                        double frame_duration) const {
        BoomDetection result;
        result.method_used = BoomDetectionMethod::MaxCausticness;

        auto max_it = std::max_element(values.begin(), values.end());
        int max_frame = static_cast<int>(std::distance(values.begin(), max_it));

        result.frame = max_frame;
        result.seconds = result.frame * frame_duration;
        result.causticness = *max_it;

        return result;
    }

    // Method 2: Find first peak >= X% of max (boom onset detection)
    // This finds when caustics FIRST become significant, not peak
    BoomDetection detectFirstPeakPercent(std::vector<double> const& values,
                                          double frame_duration) const {
        BoomDetection result;
        result.method_used = BoomDetectionMethod::FirstPeakPercent;

        if (values.empty()) return result;

        // Find maximum causticness
        double max_val = *std::max_element(values.begin(), values.end());
        double threshold = max_val * params_.peak_percent_threshold;

        // Find first local peak above threshold
        // A local peak is where values[i] > values[i-1] && values[i] >= values[i+1]
        // with prominence above min_peak_prominence
        for (size_t i = 1; i + 1 < values.size(); ++i) {
            // Check if this is a local maximum
            if (values[i] >= values[i - 1] && values[i] >= values[i + 1]) {
                // Check if it's above threshold
                if (values[i] >= threshold) {
                    // Check prominence (height above surrounding minimum)
                    double left_min = values[i];
                    double right_min = values[i];

                    // Look left for minimum
                    for (size_t j = i; j > 0; --j) {
                        left_min = std::min(left_min, values[j - 1]);
                        if (values[j - 1] > values[j]) break;
                    }

                    // Look right for minimum (limited window)
                    for (size_t j = i; j + 1 < values.size() && j < i + 30; ++j) {
                        right_min = std::min(right_min, values[j + 1]);
                        if (values[j + 1] > values[j]) break;
                    }

                    double prominence = values[i] - std::max(left_min, right_min);
                    if (prominence >= params_.min_peak_prominence * max_val) {
                        result.frame = static_cast<int>(i);
                        result.seconds = result.frame * frame_duration;
                        result.causticness = values[i];
                        return result;
                    }
                }
            }
        }

        // No peak found above threshold, fall back to max
        return detectMaxCausticness(values, frame_duration);
    }

    // Method 3: Find when d(causticness)/dt is maximum (steepest transition)
    // This detects when caustics are forming most rapidly
    BoomDetection detectDerivativePeak(std::vector<double> const& values,
                                        double frame_duration) const {
        BoomDetection result;
        result.method_used = BoomDetectionMethod::DerivativePeak;

        if (values.size() < 3) return result;

        // Apply smoothing if window > 1
        std::vector<double> smoothed = values;
        if (params_.smoothing_window > 1) {
            smoothed = smoothValues(values, params_.smoothing_window);
        }

        // Compute derivative (central difference)
        std::vector<double> derivative;
        derivative.reserve(smoothed.size() - 2);
        for (size_t i = 1; i + 1 < smoothed.size(); ++i) {
            double deriv = (smoothed[i + 1] - smoothed[i - 1]) / (2.0 * frame_duration);
            derivative.push_back(deriv);
        }

        // Find maximum positive derivative
        double max_deriv = 0.0;
        int max_deriv_frame = -1;

        for (size_t i = 0; i < derivative.size(); ++i) {
            if (derivative[i] > max_deriv) {
                max_deriv = derivative[i];
                max_deriv_frame = static_cast<int>(i + 1);  // +1 because derivative starts at frame 1
            }
        }

        if (max_deriv_frame >= 0) {
            result.frame = max_deriv_frame;
            result.seconds = result.frame * frame_duration;
            result.causticness = values[max_deriv_frame];
        } else {
            // Fall back to max if no positive derivative
            return detectMaxCausticness(values, frame_duration);
        }

        return result;
    }

    // Simple moving average smoothing
    static std::vector<double> smoothValues(std::vector<double> const& values,
                                             int window) {
        if (window <= 1 || values.size() < static_cast<size_t>(window)) {
            return values;
        }

        std::vector<double> smoothed;
        smoothed.reserve(values.size());

        int half_window = window / 2;
        for (size_t i = 0; i < values.size(); ++i) {
            double sum = 0.0;
            int count = 0;
            for (int j = -half_window; j <= half_window; ++j) {
                int idx = static_cast<int>(i) + j;
                if (idx >= 0 && idx < static_cast<int>(values.size())) {
                    sum += values[idx];
                    count++;
                }
            }
            smoothed.push_back(sum / count);
        }

        return smoothed;
    }
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
