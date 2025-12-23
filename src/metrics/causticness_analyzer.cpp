#include "metrics/causticness_analyzer.h"

#include <algorithm>
#include <cmath>

namespace metrics {

void CausticnessAnalyzer::analyze(MetricsCollector const& collector,
                                   EventDetector const& events) {
    reset();

    // Get angular causticness series (physics-based metric)
    MetricSeries<double> const* series =
        collector.getMetric(MetricNames::AngularCausticness);
    if (!series || series->empty()) {
        return;
    }

    size_t total_frames = series->size();

    // Get boom event for post-boom analysis
    auto boom_event = events.getEvent(EventNames::Boom);
    boom_frame_ = boom_event && boom_event->detected() ? boom_event->frame : 0;

    // Calculate frame duration (estimate from collector if possible)
    // Default to 1/60s if not available
    frame_duration_ = 1.0 / 60.0;

    // Find overall peak and statistics
    double sum = 0.0;
    double max_val = 0.0;
    int max_frame = 0;
    int frames_above = 0;

    for (size_t i = 0; i < total_frames; ++i) {
        double val = series->at(i);
        sum += val;

        if (val > max_val) {
            max_val = val;
            max_frame = static_cast<int>(i);
        }

        if (val >= quality_threshold_) {
            frames_above++;
        }
    }

    metrics_.peak_causticness = max_val;
    metrics_.peak_frame = max_frame;
    metrics_.peak_seconds = max_frame * frame_duration_;
    metrics_.average_causticness = sum / static_cast<double>(total_frames);
    metrics_.frames_above_threshold = frames_above;
    metrics_.time_above_threshold = frames_above * frame_duration_;
    metrics_.total_causticness = sum;

    // Post-boom analysis
    if (boom_frame_ >= 0 && static_cast<size_t>(boom_frame_) < total_frames) {
        size_t boom_idx = static_cast<size_t>(boom_frame_);
        int post_boom_frames =
            static_cast<int>(post_boom_window_seconds_ / frame_duration_);
        size_t end_frame = std::min(boom_idx + post_boom_frames, total_frames);

        double post_sum = 0.0;
        double post_max = 0.0;
        int post_max_frame = boom_frame_;
        int post_count = 0;

        for (size_t i = boom_idx; i < end_frame; ++i) {
            double val = series->at(i);
            post_sum += val;
            post_count++;

            if (val > post_max) {
                post_max = val;
                post_max_frame = static_cast<int>(i);
            }
        }

        if (post_count > 0) {
            metrics_.post_boom_average = post_sum / post_count;
        }
        metrics_.post_boom_peak = post_max;
        metrics_.post_boom_peak_frame = post_max_frame;

        // Sample at intervals after boom
        samples_.clear();
        sample_times_.clear();

        for (double t = 0.0; t <= post_boom_window_seconds_; t += sampling_interval_) {
            int frame = boom_frame_ + static_cast<int>(t / frame_duration_);
            if (frame >= 0 && static_cast<size_t>(frame) < total_frames) {
                samples_.push_back(series->at(frame));
                sample_times_.push_back(t);
            }
        }
    }

    has_results_ = true;
}

std::vector<std::pair<double, double>>
CausticnessAnalyzer::getSampleTimeline() const {
    std::vector<std::pair<double, double>> timeline;
    timeline.reserve(samples_.size());

    for (size_t i = 0; i < samples_.size() && i < sample_times_.size(); ++i) {
        timeline.emplace_back(sample_times_[i], samples_[i]);
    }

    return timeline;
}

nlohmann::json CausticnessAnalyzer::toJSON() const {
    nlohmann::json j;
    j["analyzer"] = name();
    j["has_results"] = has_results_;

    if (has_results_) {
        j["score"] = score();

        nlohmann::json m;
        m["peak_causticness"] = metrics_.peak_causticness;
        m["peak_frame"] = metrics_.peak_frame;
        m["peak_seconds"] = metrics_.peak_seconds;
        m["average_causticness"] = metrics_.average_causticness;
        m["time_above_threshold"] = metrics_.time_above_threshold;
        m["frames_above_threshold"] = metrics_.frames_above_threshold;
        m["total_causticness"] = metrics_.total_causticness;
        m["post_boom_average"] = metrics_.post_boom_average;
        m["post_boom_peak"] = metrics_.post_boom_peak;
        m["post_boom_peak_frame"] = metrics_.post_boom_peak_frame;
        m["quality_score"] = metrics_.qualityScore();

        j["metrics"] = m;

        // Include samples
        j["samples"] = samples_;
        j["sample_times"] = sample_times_;
    }

    return j;
}

} // namespace metrics
