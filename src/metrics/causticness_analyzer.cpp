#include "metrics/causticness_analyzer.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace metrics {

std::vector<CausticnessPeak>
CausticnessAnalyzer::findPeaks(std::vector<double> const& values) const {
    std::vector<CausticnessPeak> peaks;
    if (values.size() < 3) {
        return peaks;
    }

    // Calculate minimum separation in frames
    int min_sep_frames = static_cast<int>(min_peak_separation_ / frame_duration_);
    if (min_sep_frames < 1) {
        min_sep_frames = 1;
    }

    // Find global max for minimum height threshold
    double global_max = *std::max_element(values.begin(), values.end());
    double min_height = global_max * min_peak_height_fraction_;

    // Find local maxima
    for (size_t i = 1; i < values.size() - 1; ++i) {
        if (values[i] > values[i - 1] && values[i] > values[i + 1] &&
            values[i] >= min_height) {
            // Check distance from previous peak
            if (peaks.empty() ||
                (static_cast<int>(i) - peaks.back().frame) >= min_sep_frames) {
                CausticnessPeak peak;
                peak.frame = static_cast<int>(i);
                peak.value = values[i];
                peak.seconds = i * frame_duration_;
                peaks.push_back(peak);
            } else if (values[i] > peaks.back().value) {
                // Replace previous peak if this one is higher and within separation
                peaks.back().frame = static_cast<int>(i);
                peaks.back().value = values[i];
                peaks.back().seconds = i * frame_duration_;
            }
        }
    }

    return peaks;
}

void CausticnessAnalyzer::computePeakClarity(std::vector<double> const& values) {
    detected_peaks_ = findPeaks(values);

    if (detected_peaks_.empty()) {
        metrics_.peak_clarity_score = 1.0;  // No peaks = no competition
        metrics_.competing_peaks_count = 0;
        metrics_.max_competitor_ratio = 0.0;
        metrics_.nearest_competitor_seconds = 0.0;
        return;
    }

    // Find the main peak (highest)
    auto main_it = std::max_element(
        detected_peaks_.begin(), detected_peaks_.end(),
        [](auto const& a, auto const& b) { return a.value < b.value; });

    double main_value = main_it->value;
    int main_frame = main_it->frame;

    // Find all peaks before the main peak
    double max_preceding = 0.0;
    double nearest_distance = std::numeric_limits<double>::max();
    int competing_count = 0;

    for (auto const& peak : detected_peaks_) {
        if (peak.frame < main_frame) {
            competing_count++;
            if (peak.value > max_preceding) {
                max_preceding = peak.value;
            }
            double distance = (main_frame - peak.frame) * frame_duration_;
            if (distance < nearest_distance) {
                nearest_distance = distance;
            }
        }
    }

    metrics_.competing_peaks_count = competing_count;

    if (max_preceding == 0.0) {
        // No preceding peaks - perfect clarity
        metrics_.peak_clarity_score = 1.0;
        metrics_.max_competitor_ratio = 0.0;
        metrics_.nearest_competitor_seconds = 0.0;
    } else {
        // Score: main / (main + competitor) gives 0.5-1.0 range
        // Higher is better, 1.0 = no competition, 0.5 = equal peaks
        metrics_.peak_clarity_score = main_value / (main_value + max_preceding);
        metrics_.max_competitor_ratio = max_preceding / main_value;
        metrics_.nearest_competitor_seconds = nearest_distance;
    }
}

void CausticnessAnalyzer::computePostBoomArea(std::vector<double> const& values) {
    if (metrics_.peak_frame < 0 || values.empty()) {
        metrics_.post_boom_area = 0.0;
        metrics_.post_boom_area_normalized = 0.0;
        metrics_.post_boom_duration = 0.0;
        return;
    }

    // Use the peak frame as the "boom" for area calculation
    int boom_frame = metrics_.peak_frame;
    double remaining_seconds = (values.size() - boom_frame) * frame_duration_;
    double window_seconds = std::min(post_boom_window_seconds_, remaining_seconds);
    int window_frames = static_cast<int>(window_seconds / frame_duration_);

    if (window_frames <= 0) {
        metrics_.post_boom_area = 0.0;
        metrics_.post_boom_area_normalized = 0.0;
        metrics_.post_boom_duration = 0.0;
        return;
    }

    double area = 0.0;
    size_t end_frame =
        std::min(static_cast<size_t>(boom_frame + window_frames), values.size());

    for (size_t i = boom_frame; i < end_frame; ++i) {
        area += values[i];
    }

    metrics_.post_boom_area = area * frame_duration_;  // Actual area
    metrics_.post_boom_duration = window_seconds;

    // Normalize: area / (window * peak) gives 0-1 range for "sustained interest"
    // If causticness stayed at peak level the whole time, normalized = 1.0
    double max_possible_area = window_frames * metrics_.peak_causticness;
    if (max_possible_area > 0.0) {
        metrics_.post_boom_area_normalized =
            std::min(1.0, area / max_possible_area);
    } else {
        metrics_.post_boom_area_normalized = 0.0;
    }
}

void CausticnessAnalyzer::analyze(MetricsCollector const& collector,
                                  EventDetector const& events) {
    reset();

    // Get angular causticness series (physics-based metric)
    MetricSeries<double> const* series =
        collector.getMetric(MetricNames::AngularCausticness);
    if (!series || series->empty()) {
        return;
    }

    total_frames_ = series->size();
    auto const& values = series->values();

    // Get boom event for post-boom analysis (legacy, may not be used)
    auto boom_event = events.getEvent(EventNames::Boom);
    boom_frame_ = boom_event && boom_event->detected() ? boom_event->frame : 0;

    // Calculate frame duration (use configured value, or estimate from event if available)
    // If not set externally via setFrameDuration(), try to get from event
    if (frame_duration_ <= 0.0) {
        if (boom_event && boom_event->detected() && boom_event->frame > 0) {
            // Estimate from event timing
            frame_duration_ = boom_event->seconds / boom_event->frame;
        } else {
            // Default to 1/60s if nothing else available
            // This is a fallback - callers should use setFrameDuration() for accuracy
            frame_duration_ = 1.0 / 60.0;
            if (!warned_frame_duration_fallback_) {
                std::cerr << "Warning: CausticnessAnalyzer using fallback frame_duration "
                          << "(1/60s). Call setFrameDuration() for accurate results.\n";
                warned_frame_duration_fallback_ = true;
            }
        }
    }

    // Find overall peak and statistics
    double sum = 0.0;
    double max_val = 0.0;
    int max_frame = 0;
    int frames_above = 0;

    for (size_t i = 0; i < total_frames_; ++i) {
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
    metrics_.average_causticness = sum / static_cast<double>(total_frames_);
    metrics_.frames_above_threshold = frames_above;
    metrics_.time_above_threshold = frames_above * frame_duration_;
    metrics_.total_causticness = sum;

    // Post-boom analysis
    if (boom_frame_ >= 0 && static_cast<size_t>(boom_frame_) < total_frames_) {
        size_t boom_idx = static_cast<size_t>(boom_frame_);
        int post_boom_frames =
            static_cast<int>(post_boom_window_seconds_ / frame_duration_);
        size_t end_frame = std::min(boom_idx + post_boom_frames, total_frames_);

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
            if (frame >= 0 && static_cast<size_t>(frame) < total_frames_) {
                samples_.push_back(series->at(frame));
                sample_times_.push_back(t);
            }
        }
    }

    // Peak clarity analysis (find competing peaks before the main peak)
    computePeakClarity(values);

    // Post-boom area calculation
    computePostBoomArea(values);

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

        // Peak clarity metrics (new)
        m["peak_clarity_score"] = metrics_.peak_clarity_score;
        m["competing_peaks_count"] = metrics_.competing_peaks_count;
        m["max_competitor_ratio"] = metrics_.max_competitor_ratio;
        m["nearest_competitor_seconds"] = metrics_.nearest_competitor_seconds;

        // Post-boom area metrics (new)
        m["post_boom_area"] = metrics_.post_boom_area;
        m["post_boom_area_normalized"] = metrics_.post_boom_area_normalized;
        m["post_boom_duration"] = metrics_.post_boom_duration;

        j["metrics"] = m;

        // Include samples
        j["samples"] = samples_;
        j["sample_times"] = sample_times_;

        // Include detected peaks (for debugging/visualization)
        nlohmann::json peaks_json = nlohmann::json::array();
        for (auto const& peak : detected_peaks_) {
            peaks_json.push_back({{"frame", peak.frame},
                                  {"value", peak.value},
                                  {"seconds", peak.seconds}});
        }
        j["detected_peaks"] = peaks_json;
    }

    return j;
}

} // namespace metrics
