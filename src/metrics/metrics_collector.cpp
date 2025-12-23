#include "metrics/metrics_collector.h"
#include "pendulum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace metrics {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double TWO_PI = 2.0 * PI;
constexpr double HALF_PI = PI / 2.0;
} // namespace

MetricsCollector::MetricsCollector() = default;
MetricsCollector::~MetricsCollector() = default;

void MetricsCollector::registerMetric(std::string const& name, MetricType type) {
    if (metrics_.find(name) == metrics_.end()) {
        metrics_[name] = MetricSeries<double>();
        metric_types_[name] = type;
    }
}

void MetricsCollector::registerStandardMetrics() {
    registerMetric(MetricNames::Variance, MetricType::Physics);
    registerMetric(MetricNames::SpreadRatio, MetricType::Physics);
    registerMetric(MetricNames::CircularSpread, MetricType::Physics);
    registerMetric(MetricNames::AngularRange, MetricType::Physics);
    registerMetric(MetricNames::TotalEnergy, MetricType::Physics);
    registerMetric(MetricNames::AngularCausticness, MetricType::Physics);
}

void MetricsCollector::registerGPUMetrics() {
    registerMetric(MetricNames::MaxValue, MetricType::GPU);
    registerMetric(MetricNames::Brightness, MetricType::GPU);
    registerMetric(MetricNames::Coverage, MetricType::GPU);
}

void MetricsCollector::beginFrame(int frame_number) {
    current_frame_ = frame_number;
}

void MetricsCollector::setMetric(std::string const& name, double value) {
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        // Ensure the series is at the right frame
        while (it->second.size() < static_cast<size_t>(current_frame_)) {
            it->second.push(0.0);
        }
        if (it->second.size() == static_cast<size_t>(current_frame_)) {
            it->second.push(value);
        }
    }
}

void MetricsCollector::setGPUMetrics(GPUMetricsBundle const& bundle) {
    setMetric(MetricNames::MaxValue, bundle.max_value);
    setMetric(MetricNames::Brightness, bundle.brightness);
    setMetric(MetricNames::Coverage, bundle.coverage);
}

void MetricsCollector::updateGPUMetricsAtFrame(GPUMetricsBundle const& bundle, int frame) {
    // Helper to update a metric at a specific frame
    auto updateAt = [this, frame](std::string const& name, double value) {
        auto* series = getMetricMutable(name);
        if (series) {
            series->updateAt(static_cast<size_t>(frame), value);
        }
    };

    updateAt(MetricNames::MaxValue, bundle.max_value);
    updateAt(MetricNames::Brightness, bundle.brightness);
    updateAt(MetricNames::Coverage, bundle.coverage);
}

void MetricsCollector::endFrame() {
    // Nothing special needed for now
}

void MetricsCollector::updateFromPendulums(
    std::vector<Pendulum> const& pendulums) {
    if (pendulums.empty())
        return;

    std::vector<double> angle1s, angle2s;
    angle1s.reserve(pendulums.size());
    angle2s.reserve(pendulums.size());

    double total_energy = 0.0;
    for (auto const& p : pendulums) {
        angle1s.push_back(p.getTheta1());
        angle2s.push_back(p.getTheta2());
        total_energy += p.totalEnergy();
    }

    updateFromAngles(angle1s, angle2s);
    setMetric(MetricNames::TotalEnergy, total_energy);
}

void MetricsCollector::updateFromAngles(std::vector<double> const& angle1s,
                                        std::vector<double> const& angle2s) {
    // Compute variance from angle2
    double variance = computeVariance(angle2s);
    setMetric(MetricNames::Variance, variance);

    // Compute spread metrics from angle1
    SpreadMetrics spread = computeSpread(angle1s);
    current_spread_ = spread;
    spread_history_.push_back(spread);

    // Keep spread_history bounded to prevent unbounded memory growth
    if (MAX_SPREAD_HISTORY > 0 && spread_history_.size() > MAX_SPREAD_HISTORY) {
        // Remove oldest entries to stay within limit
        spread_history_.erase(spread_history_.begin(),
                              spread_history_.begin() +
                                  (spread_history_.size() - MAX_SPREAD_HISTORY));
    }

    setMetric(MetricNames::SpreadRatio, spread.spread_ratio);
    setMetric(MetricNames::CircularSpread, spread.circular_spread);
    setMetric(MetricNames::AngularRange, spread.angular_range);

    // Compute angular causticness from both angles
    double angular_causticness = computeAngularCausticness(angle1s, angle2s);
    setMetric(MetricNames::AngularCausticness, angular_causticness);
}

void MetricsCollector::reset() {
    for (auto& [name, series] : metrics_) {
        series.clear();
    }
    current_frame_ = -1;
    current_spread_ = SpreadMetrics{};
    spread_history_.clear();
}

MetricSeries<double> const*
MetricsCollector::getMetric(std::string const& name) const {
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return &it->second;
    }
    return nullptr;
}

MetricSeries<double>*
MetricsCollector::getMetricMutable(std::string const& name) {
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<std::string> MetricsCollector::getMetricNames() const {
    std::vector<std::string> names;
    names.reserve(metrics_.size());
    for (auto const& [name, _] : metrics_) {
        names.push_back(name);
    }
    return names;
}

std::vector<std::string>
MetricsCollector::getMetricNames(MetricType type) const {
    std::vector<std::string> names;
    for (auto const& [name, t] : metric_types_) {
        if (t == type) {
            names.push_back(name);
        }
    }
    return names;
}

MetricType MetricsCollector::getMetricType(std::string const& name) const {
    auto it = metric_types_.find(name);
    if (it != metric_types_.end()) {
        return it->second;
    }
    return MetricType::Physics; // Default
}

std::vector<MetricSnapshot> MetricsCollector::getSnapshot() const {
    std::vector<MetricSnapshot> snapshots;
    snapshots.reserve(metrics_.size());

    for (auto const& [name, series] : metrics_) {
        MetricSnapshot snap;
        snap.name = name;
        snap.current = series.current();
        snap.derivative = series.derivative();
        snap.min = series.min();
        snap.max = series.max();
        snap.mean = series.mean();
        snap.type = getMetricType(name);
        snapshots.push_back(snap);
    }

    return snapshots;
}

int MetricsCollector::frameCount() const {
    // Return the maximum frame count across all metrics
    int max_frames = 0;
    for (auto const& [name, series] : metrics_) {
        max_frames =
            std::max(max_frames, static_cast<int>(series.size()));
    }
    return max_frames;
}

double MetricsCollector::frameToSeconds(int frame, double duration,
                                        int total_frames) const {
    if (total_frames <= 0)
        return 0.0;
    return static_cast<double>(frame) * duration /
           static_cast<double>(total_frames);
}

double MetricsCollector::getVariance() const {
    auto* series = getMetric(MetricNames::Variance);
    return series ? series->current() : 0.0;
}

double MetricsCollector::getSpreadRatio() const {
    return current_spread_.spread_ratio;
}

double MetricsCollector::getUniformity() const {
    return current_spread_.circular_spread;
}

double MetricsCollector::getBrightness() const {
    auto* series = getMetric(MetricNames::Brightness);
    return series ? series->current() : 0.0;
}

double MetricsCollector::getCoverage() const {
    auto* series = getMetric(MetricNames::Coverage);
    return series ? series->current() : 0.0;
}

bool MetricsCollector::hasMetric(std::string const& name) const {
    return metrics_.find(name) != metrics_.end();
}

void MetricsCollector::exportCSV(
    std::string const& path, std::vector<std::string> const& columns) const {
    std::ofstream file(path);
    if (!file.is_open()) {
        return;
    }

    // Determine which columns to export
    std::vector<std::string> cols = columns;
    if (cols.empty()) {
        cols = getMetricNames();
    }

    // Write header
    file << "frame";
    for (auto const& col : cols) {
        if (hasMetric(col)) {
            file << "," << col;
        }
    }
    file << "\n";

    // Write data
    int frames = frameCount();
    for (int f = 0; f < frames; ++f) {
        file << f;
        for (auto const& col : cols) {
            auto* series = getMetric(col);
            if (series) {
                file << "," << std::fixed << std::setprecision(6)
                     << series->at(f);
            }
        }
        file << "\n";
    }
}

SpreadMetrics
MetricsCollector::computeSpread(std::vector<double> const& angle1s) const {
    SpreadMetrics metrics;
    if (angle1s.empty()) {
        return metrics;
    }

    double cos_sum = 0.0;
    double sin_sum = 0.0;
    double sum = 0.0;
    int above_count = 0;
    double min_angle = PI;
    double max_angle = -PI;

    size_t n = angle1s.size();

    for (double angle1 : angle1s) {
        // Normalize angle to [-pi, pi]
        double normalized = std::fmod(angle1, TWO_PI);
        if (normalized > PI)
            normalized -= TWO_PI;
        if (normalized < -PI)
            normalized += TWO_PI;

        // Count above horizontal
        if (std::abs(normalized) > HALF_PI) {
            above_count++;
        }

        // Circular statistics
        cos_sum += std::cos(normalized);
        sin_sum += std::sin(normalized);
        sum += normalized;

        min_angle = std::min(min_angle, normalized);
        max_angle = std::max(max_angle, normalized);
    }

    metrics.spread_ratio =
        static_cast<double>(above_count) / static_cast<double>(n);

    double cos_mean = cos_sum / static_cast<double>(n);
    double sin_mean = sin_sum / static_cast<double>(n);
    double mean_resultant_length =
        std::sqrt(cos_mean * cos_mean + sin_mean * sin_mean);
    metrics.circular_spread = 1.0 - mean_resultant_length;

    double range = max_angle - min_angle;
    metrics.angular_range = range / TWO_PI;

    metrics.angle1_mean = sum / static_cast<double>(n);

    double var_sum = 0.0;
    for (double angle1 : angle1s) {
        double normalized = std::fmod(angle1, TWO_PI);
        if (normalized > PI)
            normalized -= TWO_PI;
        if (normalized < -PI)
            normalized += TWO_PI;
        double diff = normalized - metrics.angle1_mean;
        var_sum += diff * diff;
    }
    metrics.angle1_variance = var_sum / static_cast<double>(n);

    return metrics;
}

double MetricsCollector::computeVariance(
    std::vector<double> const& angles) const {
    if (angles.empty())
        return 0.0;

    double sum = 0.0;
    for (double v : angles) {
        sum += v;
    }
    double mean = sum / static_cast<double>(angles.size());

    double var_sum = 0.0;
    for (double v : angles) {
        double diff = v - mean;
        var_sum += diff * diff;
    }
    return var_sum / static_cast<double>(angles.size());
}

std::pair<double, double> MetricsCollector::computeCircularStats(
    std::vector<double> const& angles) const {
    if (angles.empty())
        return {0.0, 0.0};

    double cos_sum = 0.0;
    double sin_sum = 0.0;

    for (double angle : angles) {
        cos_sum += std::cos(angle);
        sin_sum += std::sin(angle);
    }

    double n = static_cast<double>(angles.size());
    double cos_mean = cos_sum / n;
    double sin_mean = sin_sum / n;

    double circular_mean = std::atan2(sin_mean, cos_mean);
    double resultant_length =
        std::sqrt(cos_mean * cos_mean + sin_mean * sin_mean);

    return {circular_mean, resultant_length};
}

double MetricsCollector::computeAngularCausticness(
    std::vector<double> const& angle1s,
    std::vector<double> const& angle2s) const {

    if (angle1s.empty() || angle1s.size() != angle2s.size()) {
        return 0.0;
    }

    // Scale number of sectors based on N for consistent statistics
    // Target: ~30-50 pendulums per sector for meaningful statistics
    // This makes the metric independent of pendulum count
    constexpr int MIN_SECTORS = 8;
    constexpr int MAX_SECTORS = 72;
    constexpr int TARGET_PER_SECTOR = 40;

    int N = static_cast<int>(angle1s.size());
    int num_sectors = std::max(MIN_SECTORS, std::min(MAX_SECTORS, N / TARGET_PER_SECTOR));
    double sector_width = TWO_PI / num_sectors;

    // Use vector for dynamic sector count
    std::vector<int> sector_counts(num_sectors, 0);

    // The tip of the pendulum is at angle (th1 + th2) from vertical
    // This determines where it appears visually on the circle
    for (size_t i = 0; i < angle1s.size(); ++i) {
        double tip_angle = angle1s[i] + angle2s[i];

        // Normalize to [0, 2π)
        tip_angle = std::fmod(tip_angle, TWO_PI);
        if (tip_angle < 0) tip_angle += TWO_PI;

        int sector = static_cast<int>(tip_angle / sector_width) % num_sectors;
        sector_counts[sector]++;
    }

    // Compute coverage: fraction of sectors with at least one pendulum
    int occupied_sectors = 0;
    int total_count = 0;

    for (int count : sector_counts) {
        if (count > 0) occupied_sectors++;
        total_count += count;
    }

    if (total_count == 0) return 0.0;

    double coverage = static_cast<double>(occupied_sectors) / num_sectors;

    // Compute Gini coefficient to measure inequality of distribution
    // Gini = 0: perfectly uniform, Gini = 1: maximally concentrated
    // Gini is inherently scale-independent (normalized by total count)
    std::sort(sector_counts.begin(), sector_counts.end());

    double gini_sum = 0.0;
    for (int i = 0; i < num_sectors; ++i) {
        // Gini formula: sum of (2i - n - 1) * x_i / (n * sum(x))
        gini_sum += (2.0 * (i + 1) - num_sectors - 1) * sector_counts[i];
    }
    double gini = gini_sum / (num_sectors * static_cast<double>(total_count));

    // Causticness = coverage × gini
    // - Early phase: low coverage × high gini = LOW
    // - Interesting phase: medium coverage × medium gini = HIGH
    // - Chaos: high coverage × low gini = LOW
    return coverage * gini;
}

} // namespace metrics
