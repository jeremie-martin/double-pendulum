#include "metrics/metrics_collector.h"
#include "pendulum.h"

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
}

void MetricsCollector::registerGPUMetrics() {
    registerMetric(MetricNames::MaxValue, MetricType::GPU);
    registerMetric(MetricNames::Brightness, MetricType::GPU);
    registerMetric(MetricNames::ContrastStddev, MetricType::GPU);
    registerMetric(MetricNames::ContrastRange, MetricType::GPU);
    registerMetric(MetricNames::EdgeEnergy, MetricType::GPU);
    registerMetric(MetricNames::ColorVariance, MetricType::GPU);
    registerMetric(MetricNames::Coverage, MetricType::GPU);
    registerMetric(MetricNames::PeakMedianRatio, MetricType::GPU);
    registerMetric(MetricNames::Causticness, MetricType::Derived);
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
    setMetric(MetricNames::ContrastStddev, bundle.contrast_stddev);
    setMetric(MetricNames::ContrastRange, bundle.contrast_range);
    setMetric(MetricNames::EdgeEnergy, bundle.edge_energy);
    setMetric(MetricNames::ColorVariance, bundle.color_variance);
    setMetric(MetricNames::Coverage, bundle.coverage);
    setMetric(MetricNames::PeakMedianRatio, bundle.peak_median_ratio);

    // Compute causticness as derived metric
    double causticness = computeCausticness(bundle);
    setMetric(MetricNames::Causticness, causticness);
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

    setMetric(MetricNames::SpreadRatio, spread.spread_ratio);
    setMetric(MetricNames::CircularSpread, spread.circular_spread);
    setMetric(MetricNames::AngularRange, spread.angular_range);
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

// Helper function for causticness computation (matches FrameAnalysis::causticness())
double MetricsCollector::computeCausticness(GPUMetricsBundle const& m) const {
    // Coverage factor: peaks around 0.35, penalizes both extremes
    double coverage_factor = 0.0;
    if (m.coverage > 0.1 && m.coverage < 0.7) {
        if (m.coverage <= 0.35) {
            coverage_factor = (m.coverage - 0.1) / 0.25;
        } else {
            coverage_factor =
                1.0 - std::pow((m.coverage - 0.35) / 0.35, 1.5);
        }
        coverage_factor = std::max(0.0, coverage_factor);
    }

    // Brightness penalty
    double brightness_factor = 1.0;
    if (m.brightness > 0.15) {
        brightness_factor =
            std::max(0.0, 1.0 - (m.brightness - 0.15) * 4.0);
    }

    // Contrast factor
    double contrast_factor = std::min(1.0, m.contrast_range * 2.0);

    // Base score
    double score = m.edge_energy * (1.0 + m.color_variance * 2.0);

    // Apply factors
    score *= coverage_factor * brightness_factor *
             (0.5 + contrast_factor * 0.5);

    return score;
}

} // namespace metrics
