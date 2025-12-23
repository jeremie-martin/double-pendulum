#include "metrics/boom_analyzer.h"

#include <algorithm>
#include <cmath>

namespace metrics {

void BoomAnalyzer::analyze(MetricsCollector const& collector,
                            EventDetector const& events) {
    reset();

    // Get boom event
    auto boom_event = events.getEvent(EventNames::Boom);
    if (!boom_event || !boom_event->detected()) {
        return;
    }

    boom_frame_ = boom_event->frame;
    boom_seconds_ = boom_event->seconds;
    quality_.sharpness_ratio = boom_event->sharpness_ratio;
    quality_.variance_at_boom = boom_event->value;

    // Get the metric series
    MetricSeries<double> const* series = collector.getMetric(metric_name_);
    if (!series || series->empty()) {
        has_results_ = true;
        quality_.type = classifyBoomType();
        return;
    }

    size_t boom_idx = static_cast<size_t>(boom_frame_);
    size_t total_frames = series->size();

    // Compute pre-boom statistics
    size_t pre_start = boom_idx > static_cast<size_t>(frames_before_)
                           ? boom_idx - frames_before_
                           : 0;
    double pre_sum = 0.0;
    int pre_count = 0;
    for (size_t i = pre_start; i < boom_idx && i < total_frames; ++i) {
        pre_sum += series->at(i);
        pre_count++;
    }
    if (pre_count > 0) {
        quality_.pre_boom_variance_mean = pre_sum / pre_count;
    }

    // Compute post-boom statistics
    size_t post_end = std::min(boom_idx + frames_after_, total_frames);
    double post_max = 0.0;
    for (size_t i = boom_idx; i < post_end; ++i) {
        post_max = std::max(post_max, series->at(i));
    }
    quality_.post_boom_variance_max = post_max;

    // Find peak derivative near boom
    auto derivs = series->derivativeHistory();
    if (!derivs.empty()) {
        size_t deriv_start = boom_idx > 0 ? boom_idx - 1 : 0;
        size_t deriv_end = std::min(boom_idx + static_cast<size_t>(frames_after_),
                                    derivs.size());

        double max_deriv = 0.0;
        int peak_frame = -1;

        for (size_t i = deriv_start; i < deriv_end; ++i) {
            if (std::abs(derivs[i]) > std::abs(max_deriv)) {
                max_deriv = derivs[i];
                peak_frame = static_cast<int>(i) + 1;  // +1 because derivs are offset
            }
        }

        quality_.peak_derivative = max_deriv;
        if (peak_frame >= 0) {
            quality_.frames_to_peak = peak_frame - boom_frame_;
        }
    }

    // Compute initial acceleration (second derivative at boom)
    if (boom_idx >= 2 && boom_idx < derivs.size()) {
        quality_.initial_acceleration = derivs[boom_idx] - derivs[boom_idx - 1];
    }

    // Classify boom type
    quality_.type = classifyBoomType();
    has_results_ = true;
}

BoomType BoomAnalyzer::classifyBoomType() const {
    if (!has_results_ && quality_.sharpness_ratio == 0.0) {
        return BoomType::Unknown;
    }

    // Sharp boom: high sharpness ratio, quick peak
    if (quality_.sharpness_ratio >= sharpness_threshold_ &&
        quality_.frames_to_peak <= 15) {
        return BoomType::Sharp;
    }

    // Gradual boom: low sharpness, slow buildup
    if (quality_.sharpness_ratio < sharpness_threshold_ / 2.0) {
        return BoomType::Gradual;
    }

    // Oscillating: check if there's significant variance in the derivative
    // (would need more analysis - for now, default to gradual for medium sharpness)
    if (quality_.frames_to_peak > 30) {
        return BoomType::Oscillating;
    }

    // Default: somewhere between sharp and gradual
    return quality_.sharpness_ratio >= sharpness_threshold_ ? BoomType::Sharp
                                                             : BoomType::Gradual;
}

nlohmann::json BoomAnalyzer::toJSON() const {
    nlohmann::json j;
    j["analyzer"] = name();
    j["has_results"] = has_results_;

    if (has_results_) {
        j["boom_frame"] = boom_frame_;
        j["boom_seconds"] = boom_seconds_;
        j["score"] = score();

        nlohmann::json quality;
        quality["sharpness_ratio"] = quality_.sharpness_ratio;
        quality["peak_derivative"] = quality_.peak_derivative;
        quality["frames_to_peak"] = quality_.frames_to_peak;
        quality["initial_acceleration"] = quality_.initial_acceleration;
        quality["pre_boom_variance_mean"] = quality_.pre_boom_variance_mean;
        quality["post_boom_variance_max"] = quality_.post_boom_variance_max;
        quality["variance_at_boom"] = quality_.variance_at_boom;
        quality["type"] = boomTypeToString(quality_.type);
        quality["quality_score"] = quality_.qualityScore();

        j["quality"] = quality;
    }

    return j;
}

} // namespace metrics
