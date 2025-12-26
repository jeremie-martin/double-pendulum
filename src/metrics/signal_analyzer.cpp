#include "metrics/signal_analyzer.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace metrics {

double SignalAnalyzer::computeProminence(std::vector<double> const& values,
                                         size_t peak_idx) const {
    // Prominence = peak_value - max(left_base, right_base)
    // where left_base is the minimum between the peak and the nearest higher peak
    // to the left (or edge), and right_base is the same for the right side.
    // This is a standard signal processing metric for peak significance.

    double peak_val = values[peak_idx];
    size_t n = values.size();

    // Find left base: go left until we hit a higher value or edge
    double left_min = peak_val;
    for (size_t i = peak_idx; i > 0; --i) {
        size_t idx = i - 1;
        if (values[idx] > peak_val) {
            // Found a higher point, stop
            break;
        }
        if (values[idx] < left_min) {
            left_min = values[idx];
        }
    }

    // Find right base: go right until we hit a higher value or edge
    double right_min = peak_val;
    for (size_t i = peak_idx + 1; i < n; ++i) {
        if (values[i] > peak_val) {
            // Found a higher point, stop
            break;
        }
        if (values[i] < right_min) {
            right_min = values[i];
        }
    }

    // Prominence is height above the higher of the two bases
    double base = std::max(left_min, right_min);
    return peak_val - base;
}

std::vector<SignalPeak>
SignalAnalyzer::findPeaks(std::vector<double> const& values) const {
    // Find peaks with prominence-based filtering.
    // Algorithm:
    // 1. Find all local maxima (point higher than both neighbors)
    // 2. Filter by minimum height (fraction of global max)
    // 3. Filter by minimum prominence (fraction of global max)
    // 4. Enforce minimum separation (keep higher peak if too close)

    std::vector<SignalPeak> peaks;
    if (values.size() < 3) {
        return peaks;
    }

    // Calculate thresholds
    int min_sep_frames = static_cast<int>(min_peak_separation_ / frame_duration_);
    if (min_sep_frames < 1) {
        min_sep_frames = 1;
    }

    double global_max = *std::max_element(values.begin(), values.end());
    double min_height = global_max * min_peak_height_fraction_;
    double min_prominence = global_max * min_prominence_fraction_;

    // Step 1: Find all local maxima above minimum height
    std::vector<size_t> candidates;
    for (size_t i = 1; i < values.size() - 1; ++i) {
        if (values[i] > values[i - 1] && values[i] > values[i + 1] &&
            values[i] >= min_height) {
            candidates.push_back(i);
        }
    }

    // Step 2: Filter by prominence and collect prominent peaks
    struct ProminentPeak {
        size_t idx;
        double value;
        double prominence;
    };
    std::vector<ProminentPeak> prominent_peaks;

    for (size_t idx : candidates) {
        double prom = computeProminence(values, idx);
        if (prom >= min_prominence) {
            prominent_peaks.push_back({idx, values[idx], prom});
        }
    }

    // Step 3: Enforce minimum separation (keep higher peak)
    for (auto const& pp : prominent_peaks) {
        if (peaks.empty() ||
            (static_cast<int>(pp.idx) - peaks.back().frame) >= min_sep_frames) {
            SignalPeak peak;
            peak.frame = static_cast<int>(pp.idx);
            peak.value = pp.value;
            peak.seconds = pp.idx * frame_duration_;
            peak.prominence = pp.prominence;
            peaks.push_back(peak);
        } else if (pp.value > peaks.back().value) {
            // Replace previous peak if this one is higher and within separation
            peaks.back().frame = static_cast<int>(pp.idx);
            peaks.back().value = pp.value;
            peaks.back().seconds = pp.idx * frame_duration_;
            peaks.back().prominence = pp.prominence;
        }
    }

    return peaks;
}

void SignalAnalyzer::computePeakClarity(std::vector<double> const& values) {
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

void SignalAnalyzer::computePostReferenceArea(std::vector<double> const& values) {
    if (metrics_.peak_frame < 0 || values.empty()) {
        metrics_.post_ref_area = 0.0;
        metrics_.post_ref_area_normalized = 0.0;
        metrics_.post_ref_duration = 0.0;
        return;
    }

    // Use the resolved reference frame for area calculation
    // This is either user-set reference_frame_ or peak_frame
    int ref_frame = actual_reference_frame_;
    if (ref_frame < 0) {
        ref_frame = metrics_.peak_frame;
    }

    // Clamp ref_frame to valid range to prevent underflow
    if (ref_frame < 0 || static_cast<size_t>(ref_frame) >= values.size()) {
        metrics_.post_ref_area = 0.0;
        metrics_.post_ref_area_normalized = 0.0;
        metrics_.post_ref_duration = 0.0;
        return;
    }

    double remaining_seconds = (values.size() - static_cast<size_t>(ref_frame)) * frame_duration_;
    double window_seconds = std::min(post_ref_window_seconds_, remaining_seconds);
    int window_frames = static_cast<int>(window_seconds / frame_duration_);

    if (window_frames <= 0) {
        metrics_.post_ref_area = 0.0;
        metrics_.post_ref_area_normalized = 0.0;
        metrics_.post_ref_duration = 0.0;
        return;
    }

    double area = 0.0;
    size_t end_frame =
        std::min(static_cast<size_t>(ref_frame + window_frames), values.size());

    for (size_t i = ref_frame; i < end_frame; ++i) {
        area += values[i];
    }

    metrics_.post_ref_area = area * frame_duration_;  // Actual area
    metrics_.post_ref_duration = window_seconds;

    // Normalize: area / (window * peak) gives 0-1 range for "sustained interest"
    // If value stayed at peak level the whole time, normalized = 1.0
    double max_possible_area = window_frames * metrics_.peak_value;
    if (max_possible_area > 0.0) {
        metrics_.post_ref_area_normalized =
            std::min(1.0, area / max_possible_area);
    } else {
        metrics_.post_ref_area_normalized = 0.0;
    }
}

void SignalAnalyzer::analyze(MetricsCollector const& collector,
                             EventDetector const& events) {
    reset();

    // CRITICAL: Check that metric_name is set
    if (metric_name_.empty()) {
        std::cerr << "SignalAnalyzer: metric_name not set. "
                  << "Call setMetricName() before analyze().\n";
        return;
    }

    // Get the configured metric series
    MetricSeries<double> const* series = collector.getMetric(metric_name_);
    if (!series || series->empty()) {
        std::cerr << "SignalAnalyzer: metric '" << metric_name_ << "' not found or empty.\n";
        return;
    }

    total_frames_ = series->size();
    auto const& values = series->values();

    // Resolve reference frame:
    // 1. If user set reference_frame_, use that
    // 2. Otherwise try to get from boom event (legacy)
    // 3. Otherwise will use peak frame (set later)
    if (reference_frame_ >= 0) {
        actual_reference_frame_ = reference_frame_;
    } else {
        auto boom_event = events.getEvent(EventNames::Boom);
        if (boom_event && boom_event->detected()) {
            actual_reference_frame_ = boom_event->frame;
        } else {
            actual_reference_frame_ = -1;  // Will use peak frame
        }
    }

    // Calculate frame duration (use configured value, or estimate from event if available)
    if (frame_duration_ <= 0.0) {
        auto boom_event = events.getEvent(EventNames::Boom);
        if (boom_event && boom_event->detected() && boom_event->frame > 0) {
            // Estimate from event timing
            frame_duration_ = boom_event->seconds / boom_event->frame;
        } else {
            // Default to 1/60s if nothing else available
            frame_duration_ = 1.0 / 60.0;
            if (!warned_frame_duration_fallback_) {
                std::cerr << "Warning: SignalAnalyzer using fallback frame_duration "
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

    metrics_.peak_value = max_val;
    metrics_.peak_frame = max_frame;
    metrics_.peak_seconds = max_frame * frame_duration_;
    metrics_.average_value = sum / static_cast<double>(total_frames_);
    metrics_.frames_above_threshold = frames_above;
    metrics_.time_above_threshold = frames_above * frame_duration_;
    metrics_.total_value = sum;

    // If no reference frame was set, use peak frame
    if (actual_reference_frame_ < 0) {
        actual_reference_frame_ = max_frame;
    }

    // Post-reference analysis
    if (actual_reference_frame_ >= 0 &&
        static_cast<size_t>(actual_reference_frame_) < total_frames_) {
        size_t ref_idx = static_cast<size_t>(actual_reference_frame_);
        int post_ref_frames =
            static_cast<int>(post_ref_window_seconds_ / frame_duration_);
        size_t end_frame = std::min(ref_idx + post_ref_frames, total_frames_);

        double post_sum = 0.0;
        double post_max = 0.0;
        int post_max_frame = actual_reference_frame_;
        int post_count = 0;

        for (size_t i = ref_idx; i < end_frame; ++i) {
            double val = series->at(i);
            post_sum += val;
            post_count++;

            if (val > post_max) {
                post_max = val;
                post_max_frame = static_cast<int>(i);
            }
        }

        if (post_count > 0) {
            metrics_.post_ref_average = post_sum / post_count;
        }
        metrics_.post_ref_peak = post_max;
        metrics_.post_ref_peak_frame = post_max_frame;

        // Sample at intervals after reference
        samples_.clear();
        sample_times_.clear();

        for (double t = 0.0; t <= post_ref_window_seconds_; t += sampling_interval_) {
            int frame = actual_reference_frame_ + static_cast<int>(t / frame_duration_);
            if (frame >= 0 && static_cast<size_t>(frame) < total_frames_) {
                samples_.push_back(series->at(frame));
                sample_times_.push_back(t);
            }
        }
    }

    // Peak clarity analysis (find competing peaks before the main peak)
    computePeakClarity(values);

    // Post-reference area calculation
    computePostReferenceArea(values);

    has_results_ = true;
}

std::vector<std::pair<double, double>>
SignalAnalyzer::getSampleTimeline() const {
    std::vector<std::pair<double, double>> timeline;
    timeline.reserve(samples_.size());

    for (size_t i = 0; i < samples_.size() && i < sample_times_.size(); ++i) {
        timeline.emplace_back(sample_times_[i], samples_[i]);
    }

    return timeline;
}

nlohmann::json SignalAnalyzer::toJSON() const {
    nlohmann::json j;
    j["analyzer"] = name();
    j["metric"] = metric_name_;
    j["has_results"] = has_results_;

    if (has_results_) {
        j["score"] = score();

        nlohmann::json m;
        m["peak_value"] = metrics_.peak_value;
        m["peak_frame"] = metrics_.peak_frame;
        m["peak_seconds"] = metrics_.peak_seconds;
        m["average_value"] = metrics_.average_value;
        m["time_above_threshold"] = metrics_.time_above_threshold;
        m["frames_above_threshold"] = metrics_.frames_above_threshold;
        m["total_value"] = metrics_.total_value;
        m["post_ref_average"] = metrics_.post_ref_average;
        m["post_ref_peak"] = metrics_.post_ref_peak;
        m["post_ref_peak_frame"] = metrics_.post_ref_peak_frame;
        m["quality_score"] = metrics_.qualityScore();

        // Peak clarity metrics
        m["peak_clarity_score"] = metrics_.peak_clarity_score;
        m["competing_peaks_count"] = metrics_.competing_peaks_count;
        m["max_competitor_ratio"] = metrics_.max_competitor_ratio;
        m["nearest_competitor_seconds"] = metrics_.nearest_competitor_seconds;

        // Post-reference area metrics
        m["post_ref_area"] = metrics_.post_ref_area;
        m["post_ref_area_normalized"] = metrics_.post_ref_area_normalized;
        m["post_ref_duration"] = metrics_.post_ref_duration;

        // Legacy field names for backward compatibility
        m["peak_causticness"] = metrics_.peak_value;
        m["post_boom_area"] = metrics_.post_ref_area;
        m["post_boom_area_normalized"] = metrics_.post_ref_area_normalized;

        j["metrics"] = m;

        // Include samples
        j["samples"] = samples_;
        j["sample_times"] = sample_times_;

        // Include detected peaks (for debugging/visualization)
        nlohmann::json peaks_json = nlohmann::json::array();
        for (auto const& peak : detected_peaks_) {
            peaks_json.push_back({{"frame", peak.frame},
                                  {"value", peak.value},
                                  {"seconds", peak.seconds},
                                  {"prominence", peak.prominence}});
        }
        j["detected_peaks"] = peaks_json;
    }

    return j;
}

} // namespace metrics
