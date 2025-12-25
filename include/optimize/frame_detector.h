#pragma once

#include "optimize/prediction_target.h"
#include "metrics/metrics_collector.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace optimize {

// Result of frame detection
struct FrameDetection {
    int frame = -1;
    double seconds = 0.0;
    double metric_value = 0.0;  // Value of metric at detected frame
    FrameDetectionMethod method_used = FrameDetectionMethod::MaxValue;

    bool valid() const { return frame >= 0; }
};

// Generic frame detector that works with any metric
// This replaces the specialized BoomDetector
class FrameDetector {
public:
    FrameDetector() = default;
    explicit FrameDetector(FrameDetectionParams const& params) : params_(params) {}

    void setParams(FrameDetectionParams const& params) { params_ = params; }
    FrameDetectionParams const& getParams() const { return params_; }

    // Main detection entry point
    FrameDetection detect(metrics::MetricsCollector const& collector,
                          double frame_duration) const {
        auto const* metric_series = collector.getMetric(params_.metric_name);
        if (!metric_series || metric_series->empty()) {
            // Fall back to AngularCausticness if specified metric not found
            metric_series = collector.getMetric(metrics::MetricNames::AngularCausticness);
            if (!metric_series || metric_series->empty()) {
                return FrameDetection{};
            }
        }

        auto const& values = metric_series->values();

        FrameDetection result;
        switch (params_.method) {
        case FrameDetectionMethod::MaxValue:
            result = detectMaxValue(values, frame_duration);
            break;
        case FrameDetectionMethod::FirstPeakPercent:
            result = detectFirstPeakPercent(values, frame_duration);
            break;
        case FrameDetectionMethod::DerivativePeak:
            result = detectDerivativePeak(values, frame_duration);
            break;
        case FrameDetectionMethod::ThresholdCrossing:
            result = detectThresholdCrossing(values, frame_duration);
            break;
        case FrameDetectionMethod::SecondDerivativePeak:
            result = detectSecondDerivativePeak(values, frame_duration);
            break;
        default:
            result = detectMaxValue(values, frame_duration);
            break;
        }

        // Apply offset (common to all methods) for visual alignment
        // Negative offset = earlier, Positive offset = later
        if (result.frame >= 0 && std::abs(params_.offset_seconds) > 1e-9) {
            int offset_frames = static_cast<int>(params_.offset_seconds / frame_duration);
            result.frame = std::max(0, std::min(static_cast<int>(values.size()) - 1,
                                                 result.frame - offset_frames));
            result.seconds = result.frame * frame_duration;
            // Update value to match the offset frame
            if (result.frame >= 0 && result.frame < static_cast<int>(values.size())) {
                result.metric_value = values[result.frame];
            }
        }

        return result;
    }

private:
    FrameDetectionParams params_;

    // Method 1: Find frame with maximum metric value
    FrameDetection detectMaxValue(std::vector<double> const& values,
                                   double frame_duration) const {
        FrameDetection result;
        result.method_used = FrameDetectionMethod::MaxValue;

        if (values.empty()) return result;

        auto max_it = std::max_element(values.begin(), values.end());
        int max_frame = static_cast<int>(std::distance(values.begin(), max_it));

        result.frame = max_frame;
        result.seconds = result.frame * frame_duration;
        result.metric_value = *max_it;

        return result;
    }

    // Method 2: Find first peak >= X% of max
    FrameDetection detectFirstPeakPercent(std::vector<double> const& values,
                                           double frame_duration) const {
        FrameDetection result;
        result.method_used = FrameDetectionMethod::FirstPeakPercent;

        if (values.empty()) return result;

        // Find maximum value
        double max_val = *std::max_element(values.begin(), values.end());
        double threshold = max_val * params_.peak_percent_threshold;

        // Find first local peak above threshold with sufficient prominence
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
                        result.metric_value = values[i];
                        return result;
                    }
                }
            }
        }

        // No peak found above threshold, fall back to max
        return detectMaxValue(values, frame_duration);
    }

    // Method 3: Find when d(metric)/dt is maximum
    FrameDetection detectDerivativePeak(std::vector<double> const& values,
                                         double frame_duration) const {
        FrameDetection result;
        result.method_used = FrameDetectionMethod::DerivativePeak;

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
            result.metric_value = values[max_deriv_frame];
        } else {
            // Fall back to max if no positive derivative
            return detectMaxValue(values, frame_duration);
        }

        return result;
    }

    // Method 4: First frame where metric crosses threshold
    FrameDetection detectThresholdCrossing(std::vector<double> const& values,
                                            double frame_duration) const {
        FrameDetection result;
        result.method_used = FrameDetectionMethod::ThresholdCrossing;

        if (values.empty()) return result;

        // Find maximum value to compute threshold
        double max_val = *std::max_element(values.begin(), values.end());
        double threshold = max_val * params_.crossing_threshold;

        // Find first frame where metric crosses and stays above threshold
        int consecutive = 0;
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i] >= threshold) {
                consecutive++;
                if (consecutive >= params_.crossing_confirmation) {
                    // Found! Return the frame where crossing started
                    int crossing_frame = static_cast<int>(i) - params_.crossing_confirmation + 1;
                    result.frame = std::max(0, crossing_frame);
                    result.seconds = result.frame * frame_duration;
                    result.metric_value = values[result.frame];
                    return result;
                }
            } else {
                consecutive = 0;
            }
        }

        // No sustained crossing found, fall back to max
        return detectMaxValue(values, frame_duration);
    }

    // Method 5: When d²(metric)/dt² is maximum
    FrameDetection detectSecondDerivativePeak(std::vector<double> const& values,
                                               double frame_duration) const {
        FrameDetection result;
        result.method_used = FrameDetectionMethod::SecondDerivativePeak;

        if (values.size() < 5) return result;

        // Apply smoothing if window > 1
        std::vector<double> smoothed = values;
        if (params_.smoothing_window > 1) {
            smoothed = smoothValues(values, params_.smoothing_window);
        }

        // Compute first derivative (central difference)
        std::vector<double> first_deriv;
        first_deriv.reserve(smoothed.size() - 2);
        for (size_t i = 1; i + 1 < smoothed.size(); ++i) {
            double deriv = (smoothed[i + 1] - smoothed[i - 1]) / (2.0 * frame_duration);
            first_deriv.push_back(deriv);
        }

        // Compute second derivative (central difference of first derivative)
        std::vector<double> second_deriv;
        second_deriv.reserve(first_deriv.size() - 2);
        for (size_t i = 1; i + 1 < first_deriv.size(); ++i) {
            double deriv2 = (first_deriv[i + 1] - first_deriv[i - 1]) / (2.0 * frame_duration);
            second_deriv.push_back(deriv2);
        }

        // Find maximum positive second derivative (acceleration)
        double max_accel = 0.0;
        int max_accel_frame = -1;

        for (size_t i = 0; i < second_deriv.size(); ++i) {
            if (second_deriv[i] > max_accel) {
                max_accel = second_deriv[i];
                // +2 because second_deriv starts at frame 2
                max_accel_frame = static_cast<int>(i + 2);
            }
        }

        if (max_accel_frame >= 0 && max_accel_frame < static_cast<int>(values.size())) {
            result.frame = max_accel_frame;
            result.seconds = result.frame * frame_duration;
            result.metric_value = values[max_accel_frame];
        } else {
            // Fall back to first derivative peak
            return detectDerivativePeak(values, frame_duration);
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

// Convenience function: detect frame using params
inline FrameDetection detectFrame(metrics::MetricsCollector const& collector,
                                   double frame_duration,
                                   FrameDetectionParams const& params) {
    FrameDetector detector(params);
    return detector.detect(collector, frame_duration);
}

// Convert PredictionResult from FrameDetection
inline PredictionResult toPredictionResult(std::string const& target_name,
                                            FrameDetection const& detection) {
    PredictionResult result;
    result.target_name = target_name;
    result.type = PredictionType::Frame;
    result.predicted_frame = detection.frame;
    result.predicted_seconds = detection.seconds;
    result.predicted_score = detection.metric_value;
    return result;
}

}  // namespace optimize
