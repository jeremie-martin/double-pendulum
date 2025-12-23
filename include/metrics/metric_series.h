#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

namespace metrics {

// Result of a threshold crossing detection
struct CrossingResult {
    int frame = -1;           // Frame when threshold was first crossed
    bool rising = true;       // True if crossing from below, false if from above
    double value = 0.0;       // Metric value at crossing
    double derivative = 0.0;  // Derivative at crossing (for sharpness)
};

// Generic time series for a single metric with derivative tracking
template <typename T = double>
class MetricSeries {
public:
    using value_type = T;

    // Core operations
    void push(T value) {
        values_.push_back(value);
        invalidateCache();
    }

    void clear() {
        values_.clear();
        invalidateCache();
    }

    void reserve(size_t n) { values_.reserve(n); }

    size_t size() const { return values_.size(); }
    bool empty() const { return values_.empty(); }

    // Value access
    T current() const { return values_.empty() ? T{} : values_.back(); }

    T at(size_t frame) const {
        return frame < values_.size() ? values_[frame] : T{};
    }

    T const& operator[](size_t frame) const { return values_[frame]; }

    std::span<T const> history() const { return std::span<T const>(values_); }

    std::vector<T> const& values() const { return values_; }

    // Derivative access (first-order, computed on demand)
    T derivative() const {
        if (values_.size() < 2)
            return T{};
        return values_.back() - values_[values_.size() - 2];
    }

    T derivativeAt(size_t frame) const {
        if (frame == 0 || frame >= values_.size())
            return T{};
        return values_[frame] - values_[frame - 1];
    }

    std::vector<T> derivativeHistory() const {
        if (values_.size() < 2)
            return {};

        std::vector<T> derivatives;
        derivatives.reserve(values_.size() - 1);
        for (size_t i = 1; i < values_.size(); ++i) {
            derivatives.push_back(values_[i] - values_[i - 1]);
        }
        return derivatives;
    }

    // Smoothed value using moving average
    T smoothed(size_t window = 5) const {
        return smoothedAt(values_.size() - 1, window);
    }

    T smoothedAt(size_t frame, size_t window = 5) const {
        if (values_.empty() || frame >= values_.size())
            return T{};

        size_t half = window / 2;
        size_t start = (frame >= half) ? frame - half : 0;
        size_t end = std::min(frame + half + 1, values_.size());

        T sum = T{};
        for (size_t i = start; i < end; ++i) {
            sum += values_[i];
        }
        return sum / static_cast<T>(end - start);
    }

    std::vector<T> smoothedHistory(size_t window = 5) const {
        std::vector<T> result;
        result.reserve(values_.size());
        for (size_t i = 0; i < values_.size(); ++i) {
            result.push_back(smoothedAt(i, window));
        }
        return result;
    }

    // Statistics
    T min() const {
        if (values_.empty())
            return T{};
        return *std::min_element(values_.begin(), values_.end());
    }

    T max() const {
        if (values_.empty())
            return T{};
        return *std::max_element(values_.begin(), values_.end());
    }

    T mean() const {
        if (values_.empty())
            return T{};
        return std::accumulate(values_.begin(), values_.end(), T{}) /
               static_cast<T>(values_.size());
    }

    T variance() const {
        if (values_.size() < 2)
            return T{};
        T m = mean();
        T sum_sq = T{};
        for (auto const& v : values_) {
            T diff = v - m;
            sum_sq += diff * diff;
        }
        return sum_sq / static_cast<T>(values_.size() - 1);
    }

    // Threshold crossing detection
    // Returns the first frame where value crosses threshold and stays above for
    // confirmation_frames
    std::optional<CrossingResult>
    findThresholdCrossing(T threshold, int confirmation_frames,
                          bool above = true) const {
        if (values_.empty() || confirmation_frames <= 0)
            return std::nullopt;

        int consecutive = 0;
        int first_cross_frame = -1;

        for (size_t i = 0; i < values_.size(); ++i) {
            bool crosses = above ? (values_[i] > threshold)
                                 : (values_[i] < threshold);

            if (crosses) {
                if (consecutive == 0) {
                    first_cross_frame = static_cast<int>(i);
                }
                consecutive++;

                if (consecutive >= confirmation_frames) {
                    CrossingResult result;
                    result.frame = first_cross_frame;
                    result.rising = above;
                    result.value = values_[first_cross_frame];
                    result.derivative = derivativeAt(first_cross_frame);
                    return result;
                }
            } else {
                consecutive = 0;
                first_cross_frame = -1;
            }
        }

        return std::nullopt;
    }

    // Find peak value (maximum or minimum)
    int findPeak(bool maximum = true) const {
        if (values_.empty())
            return -1;

        auto it = maximum ? std::max_element(values_.begin(), values_.end())
                          : std::min_element(values_.begin(), values_.end());

        return static_cast<int>(std::distance(values_.begin(), it));
    }

    // Find peak in derivative (sharpest change)
    int findDerivativePeak(bool maximum = true) const {
        auto derivs = derivativeHistory();
        if (derivs.empty())
            return -1;

        auto it = maximum ? std::max_element(derivs.begin(), derivs.end())
                          : std::min_element(derivs.begin(), derivs.end());

        // +1 because derivative[i] corresponds to frame i+1
        return static_cast<int>(std::distance(derivs.begin(), it)) + 1;
    }

    // Find derivative peak with minimum prominence
    std::optional<int> findDerivativePeak(T min_prominence) const {
        auto derivs = derivativeHistory();
        if (derivs.empty())
            return std::nullopt;

        auto it = std::max_element(derivs.begin(), derivs.end());
        if (*it >= min_prominence) {
            return static_cast<int>(std::distance(derivs.begin(), it)) + 1;
        }
        return std::nullopt;
    }

    // Get value at a specific percentile (0-100)
    T percentile(double p) const {
        if (values_.empty())
            return T{};

        std::vector<T> sorted = values_;
        std::sort(sorted.begin(), sorted.end());

        double idx = (p / 100.0) * (sorted.size() - 1);
        size_t lower = static_cast<size_t>(idx);
        size_t upper = std::min(lower + 1, sorted.size() - 1);
        double frac = idx - lower;

        return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
    }

private:
    std::vector<T> values_;

    // Cache invalidation (for future derivative caching)
    void invalidateCache() {
        // Reserved for future optimization
    }
};

// Explicit instantiation for common types
extern template class MetricSeries<double>;
extern template class MetricSeries<float>;

} // namespace metrics
