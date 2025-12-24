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

    // New caustic metrics
    registerMetric(MetricNames::R1, MetricType::Physics);
    registerMetric(MetricNames::R2, MetricType::Physics);
    registerMetric(MetricNames::JointConcentration, MetricType::Physics);
    registerMetric(MetricNames::TipCausticness, MetricType::Physics);
    registerMetric(MetricNames::SpatialConcentration, MetricType::Physics);

    // Alternative caustic metrics (experimental)
    registerMetric(MetricNames::CVCausticness, MetricType::Physics);
    registerMetric(MetricNames::OrganizationCausticness, MetricType::Physics);
    registerMetric(MetricNames::FoldCausticness, MetricType::Physics);

    // New paradigm metrics (local coherence based)
    registerMetric(MetricNames::TrajectorySmoothness, MetricType::Physics);
    registerMetric(MetricNames::Curvature, MetricType::Physics);
    registerMetric(MetricNames::TrueFolds, MetricType::Physics);
    registerMetric(MetricNames::LocalCoherence, MetricType::Physics);
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
    // NOTE: If columns is empty, uses getMetricNames() which may have arbitrary order
    // depending on hash map iteration. For reproducible ordering, pass explicit column list.
    // See Simulation::saveMetricsCSV() for canonical column order.
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

void MetricsCollector::updateFromStates(std::vector<PendulumState> const& states) {
    if (states.empty()) return;

    // Extract angles and positions
    std::vector<double> angle1s, angle2s;
    std::vector<double> x2s, y2s;
    angle1s.reserve(states.size());
    angle2s.reserve(states.size());
    x2s.reserve(states.size());
    y2s.reserve(states.size());

    for (auto const& s : states) {
        angle1s.push_back(s.th1);
        angle2s.push_back(s.th2);
        x2s.push_back(s.x2);
        y2s.push_back(s.y2);
    }

    // Call existing angle-based computations (variance, spread, angular_causticness)
    updateFromAngles(angle1s, angle2s);

    // Compute new caustic metrics - Tier 1: angle-based
    // Use causticness formula (coverage × gini) instead of raw concentration
    // This gives the desired low→high→low pattern instead of high→low
    double r1 = computeCausticnessFromAngles(angle1s);
    double r2 = computeCausticnessFromAngles(angle2s);
    setMetric(MetricNames::R1, r1);
    setMetric(MetricNames::R2, r2);
    setMetric(MetricNames::JointConcentration, r1 * r2);

    // Compute new caustic metrics - Tier 2: position-based
    double tip_causticness = computeTipCausticness(x2s, y2s);
    setMetric(MetricNames::TipCausticness, tip_causticness);

    double spatial_concentration = computeSpatialConcentration(x2s, y2s);
    setMetric(MetricNames::SpatialConcentration, spatial_concentration);

    // Alternative caustic metrics (experimental)
    double cv_causticness = computeCVCausticness(angle1s, angle2s);
    setMetric(MetricNames::CVCausticness, cv_causticness);

    double organization = computeOrganizationCausticness(angle1s, angle2s);
    setMetric(MetricNames::OrganizationCausticness, organization);

    double fold_causticness = computeFoldCausticness(x2s, y2s);
    setMetric(MetricNames::FoldCausticness, fold_causticness);

    // New paradigm metrics (local coherence based)
    double smoothness = computeTrajectorySmoothness(x2s, y2s);
    setMetric(MetricNames::TrajectorySmoothness, smoothness);

    double curvature = computeCurvature(x2s, y2s);
    setMetric(MetricNames::Curvature, curvature);

    double true_folds = computeTrueFolds(x2s, y2s);
    setMetric(MetricNames::TrueFolds, true_folds);

    double local_coherence = computeLocalCoherence(x2s, y2s);
    setMetric(MetricNames::LocalCoherence, local_coherence);
}

double MetricsCollector::computeCausticnessFromAngles(
    std::vector<double> const& angles) const {
    if (angles.empty()) return 0.0;

    // Scale number of sectors based on N for consistent statistics
    constexpr int MIN_SECTORS = 8;
    constexpr int MAX_SECTORS = 72;
    constexpr int TARGET_PER_SECTOR = 40;

    int N = static_cast<int>(angles.size());
    int num_sectors = std::max(MIN_SECTORS, std::min(MAX_SECTORS, N / TARGET_PER_SECTOR));
    double sector_width = TWO_PI / num_sectors;

    std::vector<int> sector_counts(num_sectors, 0);

    for (double angle : angles) {
        // Normalize to [0, 2π)
        double normalized = std::fmod(angle, TWO_PI);
        if (normalized < 0) normalized += TWO_PI;

        int sector = static_cast<int>(normalized / sector_width) % num_sectors;
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

    // Compute Gini coefficient
    std::sort(sector_counts.begin(), sector_counts.end());

    double gini_sum = 0.0;
    for (int i = 0; i < num_sectors; ++i) {
        gini_sum += (2.0 * (i + 1) - num_sectors - 1) * sector_counts[i];
    }
    double gini = gini_sum / (num_sectors * static_cast<double>(total_count));

    return coverage * gini;
}

double MetricsCollector::computeTipCausticness(
    std::vector<double> const& x2s,
    std::vector<double> const& y2s) const {
    if (x2s.empty() || x2s.size() != y2s.size()) return 0.0;

    // Compute actual tip angles from Cartesian positions
    // atan2(x, y) gives angle from vertical (y-axis), matching pendulum convention
    std::vector<double> tip_angles;
    tip_angles.reserve(x2s.size());

    for (size_t i = 0; i < x2s.size(); ++i) {
        // atan2(x, y) for angle from vertical (positive y = down in pendulum coords)
        double tip_angle = std::atan2(x2s[i], y2s[i]);
        tip_angles.push_back(tip_angle);
    }

    // Use same causticness algorithm as angular_causticness
    return computeCausticnessFromAngles(tip_angles);
}

double MetricsCollector::computeSpatialConcentration(
    std::vector<double> const& x2s,
    std::vector<double> const& y2s) const {
    if (x2s.empty() || x2s.size() != y2s.size()) return 0.0;

    int N = static_cast<int>(x2s.size());

    // Adaptive grid size: target ~40 pendulums per cell for statistical stability
    // This makes the metric N-independent
    constexpr int MIN_GRID = 4;
    constexpr int MAX_GRID = 32;
    constexpr int TARGET_PER_CELL = 40;

    int grid_size = std::max(MIN_GRID, std::min(MAX_GRID,
        static_cast<int>(std::sqrt(static_cast<double>(N) / TARGET_PER_CELL))));

    // Find bounds
    double min_x = *std::min_element(x2s.begin(), x2s.end());
    double max_x = *std::max_element(x2s.begin(), x2s.end());
    double min_y = *std::min_element(y2s.begin(), y2s.end());
    double max_y = *std::max_element(y2s.begin(), y2s.end());

    // Add small margin to avoid edge cases
    double range_x = max_x - min_x;
    double range_y = max_y - min_y;
    if (range_x < 1e-10) range_x = 1.0;
    if (range_y < 1e-10) range_y = 1.0;

    double cell_width = range_x / grid_size;
    double cell_height = range_y / grid_size;

    // Build 2D histogram - O(N) operation
    int num_cells = grid_size * grid_size;
    std::vector<int> histogram(num_cells, 0);

    for (size_t i = 0; i < x2s.size(); ++i) {
        int xi = std::min(grid_size - 1,
            static_cast<int>((x2s[i] - min_x) / cell_width));
        int yi = std::min(grid_size - 1,
            static_cast<int>((y2s[i] - min_y) / cell_height));
        histogram[yi * grid_size + xi]++;
    }

    // Compute coverage (fraction of cells with at least one pendulum)
    int occupied_cells = 0;
    for (int count : histogram) {
        if (count > 0) occupied_cells++;
    }
    double coverage = static_cast<double>(occupied_cells) / num_cells;

    // Compute Gini coefficient on histogram (more stable than peak/mean)
    // Gini = 0: perfectly uniform, Gini = 1: maximally concentrated
    std::sort(histogram.begin(), histogram.end());

    double gini_sum = 0.0;
    for (int i = 0; i < num_cells; ++i) {
        gini_sum += (2.0 * (i + 1) - num_cells - 1) * histogram[i];
    }
    double gini = gini_sum / (num_cells * static_cast<double>(N));

    // Return coverage × gini for consistent behavior with other caustic metrics
    // - Early phase: low coverage × high gini = LOW (all clustered)
    // - Interesting phase: medium coverage × medium gini = HIGH (structured spread)
    // - Chaos: high coverage × low gini = LOW (uniform spread)
    return coverage * gini;
}

double MetricsCollector::computeCVCausticness(
    std::vector<double> const& angle1s,
    std::vector<double> const& angle2s) const {
    // CV-based causticness: uses coefficient of variation instead of Gini
    // CV = σ/μ is more sensitive to "spikiness" that characterizes caustics

    if (angle1s.empty() || angle1s.size() != angle2s.size()) return 0.0;

    // Scale sectors adaptively for N-independence
    constexpr int MIN_SECTORS = 8;
    constexpr int MAX_SECTORS = 72;
    constexpr int TARGET_PER_SECTOR = 40;

    int N = static_cast<int>(angle1s.size());
    int num_sectors = std::max(MIN_SECTORS, std::min(MAX_SECTORS, N / TARGET_PER_SECTOR));
    double sector_width = TWO_PI / num_sectors;

    std::vector<int> sector_counts(num_sectors, 0);

    // Use combined tip angle (th1 + th2)
    for (size_t i = 0; i < angle1s.size(); ++i) {
        double tip_angle = angle1s[i] + angle2s[i];
        tip_angle = std::fmod(tip_angle, TWO_PI);
        if (tip_angle < 0) tip_angle += TWO_PI;

        int sector = static_cast<int>(tip_angle / sector_width) % num_sectors;
        sector_counts[sector]++;
    }

    // Compute coverage
    int occupied_sectors = 0;
    for (int count : sector_counts) {
        if (count > 0) occupied_sectors++;
    }
    double coverage = static_cast<double>(occupied_sectors) / num_sectors;

    // Compute coefficient of variation (CV = std_dev / mean)
    double mean = static_cast<double>(N) / num_sectors;
    double variance = 0.0;
    for (int count : sector_counts) {
        double diff = count - mean;
        variance += diff * diff;
    }
    variance /= num_sectors;
    double std_dev = std::sqrt(variance);
    double cv = (mean > 1e-10) ? std_dev / mean : 0.0;

    // Normalize CV to roughly 0-1 range (CV can be 0-2+ for very spiky data)
    // Then multiply by coverage for the desired low→high→low pattern
    double normalized_cv = std::min(1.0, cv / 1.5);

    return coverage * normalized_cv;
}

double MetricsCollector::computeOrganizationCausticness(
    std::vector<double> const& angle1s,
    std::vector<double> const& angle2s) const {
    // Organization causticness: (1 - R1*R2) × coverage
    // - R1, R2 are mean resultant lengths (0=dispersed, 1=concentrated)
    // - (1 - R1*R2) is high when angles are spread but not fully random
    // - Coverage ensures we're actually exploring the space

    if (angle1s.empty() || angle1s.size() != angle2s.size()) return 0.0;

    // Compute R1 (mean resultant length for angle1)
    double cos_sum1 = 0.0, sin_sum1 = 0.0;
    for (double angle : angle1s) {
        cos_sum1 += std::cos(angle);
        sin_sum1 += std::sin(angle);
    }
    double n = static_cast<double>(angle1s.size());
    double R1 = std::sqrt(std::pow(cos_sum1/n, 2) + std::pow(sin_sum1/n, 2));

    // Compute R2 (mean resultant length for angle2)
    double cos_sum2 = 0.0, sin_sum2 = 0.0;
    for (double angle : angle2s) {
        cos_sum2 += std::cos(angle);
        sin_sum2 += std::sin(angle);
    }
    double R2 = std::sqrt(std::pow(cos_sum2/n, 2) + std::pow(sin_sum2/n, 2));

    // Compute sector coverage for combined tip angle
    constexpr int MIN_SECTORS = 8;
    constexpr int MAX_SECTORS = 72;
    constexpr int TARGET_PER_SECTOR = 40;

    int N = static_cast<int>(angle1s.size());
    int num_sectors = std::max(MIN_SECTORS, std::min(MAX_SECTORS, N / TARGET_PER_SECTOR));
    double sector_width = TWO_PI / num_sectors;

    std::vector<bool> occupied(num_sectors, false);
    for (size_t i = 0; i < angle1s.size(); ++i) {
        double tip_angle = angle1s[i] + angle2s[i];
        tip_angle = std::fmod(tip_angle, TWO_PI);
        if (tip_angle < 0) tip_angle += TWO_PI;
        int sector = static_cast<int>(tip_angle / sector_width) % num_sectors;
        occupied[sector] = true;
    }

    int occupied_count = 0;
    for (bool occ : occupied) {
        if (occ) occupied_count++;
    }
    double coverage = static_cast<double>(occupied_count) / num_sectors;

    // Organization = (1 - R1*R2) × coverage
    // - Early: R1≈1, R2≈1 → (1-1)=0 → LOW
    // - Caustic: R moderate → (1-R1*R2) moderate, coverage high → HIGH
    // - Chaos: R≈0 → (1-0)=1, but also high coverage → need to check if this works
    double organization = (1.0 - R1 * R2) * coverage;

    return organization;
}

double MetricsCollector::computeFoldCausticness(
    std::vector<double> const& x2s,
    std::vector<double> const& y2s) const {
    // Fold causticness: leverages natural ordering of pendulums by initial angle
    // At caustics, adjacent pendulums (i and i+1) have highly variable distances:
    // - Some are very close (at "folds" where trajectories bunch)
    // - Some are far apart (between folds)
    // This creates high coefficient of variation in adjacent-pair distances

    if (x2s.size() < 2 || x2s.size() != y2s.size()) return 0.0;

    int N = static_cast<int>(x2s.size());

    // 1. Compute spatial spread (how far tips are from center)
    double sum_r = 0.0;
    for (int i = 0; i < N; ++i) {
        sum_r += std::sqrt(x2s[i] * x2s[i] + y2s[i] * y2s[i]);
    }
    double mean_radius = sum_r / N;
    double max_radius = 2.0;  // L1 + L2 (assuming unit lengths)
    double spread = std::min(1.0, mean_radius / max_radius);

    // 2. Compute adjacent-pair distance statistics
    // Pendulums are ordered by initial angle, so adjacent pairs started nearly identical
    double sum_d = 0.0, sum_d2 = 0.0;
    for (int i = 0; i < N - 1; ++i) {
        double dx = x2s[i + 1] - x2s[i];
        double dy = y2s[i + 1] - y2s[i];
        double d = std::sqrt(dx * dx + dy * dy);
        sum_d += d;
        sum_d2 += d * d;
    }
    double mean_d = sum_d / (N - 1);
    double var_d = sum_d2 / (N - 1) - mean_d * mean_d;
    double std_d = std::sqrt(std::max(0.0, var_d));

    // Coefficient of variation: high when some pairs close, others far
    double cv = (mean_d > 1e-10) ? std_d / mean_d : 0.0;

    // Normalize CV to roughly 0-1 range
    double clustering = std::min(1.0, cv / 1.5);

    // 3. Combine: need high spread AND high CV
    // - Start: spread≈0 → metric≈0 (all tips at same spot)
    // - Caustic: spread high, CV high → metric high (folds visible)
    // - Chaos: spread high, CV low → metric low (uniform spacing)
    return spread * clustering;
}

// === NEW PARADIGM METRICS ===
// Based on neighbor distance statistics and their autocorrelation

double MetricsCollector::computeTrajectorySmoothness(
    std::vector<double> const& x2s,
    std::vector<double> const& y2s) const {
    // RENAMED PURPOSE: Neighbor Distance Autocorrelation
    //
    // Key insight: At caustics, small neighbor distances CLUSTER together
    // (contiguous fold regions), while in chaos they're scattered randomly.
    //
    // Lag-1 autocorrelation of neighbor distances:
    // - Caustic: d[i] small implies d[i+1] likely small (same fold) → positive autocorr
    // - Chaos: d[i] and d[i+1] are independent → autocorr ≈ 0
    //
    // This directly measures the "coherent structure" that defines caustics.

    if (x2s.size() < 4 || x2s.size() != y2s.size()) return 0.0;

    int N = static_cast<int>(x2s.size());

    // Compute spread (circular_spread from angle data is better, but use spatial as fallback)
    auto const* spread_metric = getMetric(MetricNames::CircularSpread);
    double spread = 0.0;
    if (spread_metric && spread_metric->size() > 0) {
        spread = spread_metric->current();
    } else {
        // Fallback: spatial spread
        double sum_r = 0.0;
        for (int i = 0; i < N; ++i) {
            sum_r += std::sqrt(x2s[i] * x2s[i] + y2s[i] * y2s[i]);
        }
        spread = std::min(1.0, (sum_r / N) / 2.0);
    }

    if (spread < 0.05) return 0.0;  // Not spread enough yet

    // Compute neighbor distances
    std::vector<double> distances;
    distances.reserve(N - 1);
    for (int i = 0; i < N - 1; ++i) {
        double dx = x2s[i + 1] - x2s[i];
        double dy = y2s[i + 1] - y2s[i];
        distances.push_back(std::sqrt(dx * dx + dy * dy));
    }

    // Compute mean and variance of distances
    double sum_d = 0.0, sum_d2 = 0.0;
    for (double d : distances) {
        sum_d += d;
        sum_d2 += d * d;
    }
    double mean_d = sum_d / distances.size();
    double var_d = sum_d2 / distances.size() - mean_d * mean_d;

    if (var_d < 1e-12) return 0.0;  // All same distance (start or perfect chaos)

    // Compute lag-1 autocovariance
    double autocovar = 0.0;
    for (size_t i = 0; i + 1 < distances.size(); ++i) {
        autocovar += (distances[i] - mean_d) * (distances[i + 1] - mean_d);
    }
    autocovar /= (distances.size() - 1);

    // Autocorrelation = autocovariance / variance
    double autocorr = autocovar / var_d;

    // autocorr ranges roughly -1 to 1
    // Positive = clustering (caustics), Near-zero = random (chaos), Negative = alternating
    // We only care about positive autocorrelation
    double positive_autocorr = std::max(0.0, autocorr);

    // Combine with spread: need both spread AND clustering for caustic
    return spread * positive_autocorr;
}

double MetricsCollector::computeCurvature(
    std::vector<double> const& x2s,
    std::vector<double> const& y2s) const {
    // RENAMED PURPOSE: Distance Bimodality (P90/P10 ratio)
    //
    // At caustics, neighbor distances are BIMODAL:
    // - Fold regions: very small distances (many θ → same pos)
    // - Between folds: normal distances
    //
    // In chaos, distances follow a continuous (Rayleigh-like) distribution.
    //
    // P90/P10 ratio captures this bimodality:
    // - Caustic: P10 is tiny (folds), P90 is normal → HIGH ratio
    // - Chaos: continuous distribution → moderate ratio
    // - Start: all same → P90 ≈ P10 → ratio ≈ 1

    if (x2s.size() < 10 || x2s.size() != y2s.size()) return 0.0;

    int N = static_cast<int>(x2s.size());

    // Compute spread
    auto const* spread_metric = getMetric(MetricNames::CircularSpread);
    double spread = 0.0;
    if (spread_metric && spread_metric->size() > 0) {
        spread = spread_metric->current();
    } else {
        double sum_r = 0.0;
        for (int i = 0; i < N; ++i) {
            sum_r += std::sqrt(x2s[i] * x2s[i] + y2s[i] * y2s[i]);
        }
        spread = std::min(1.0, (sum_r / N) / 2.0);
    }

    if (spread < 0.05) return 0.0;

    // Compute neighbor distances
    std::vector<double> distances;
    distances.reserve(N - 1);
    for (int i = 0; i < N - 1; ++i) {
        double dx = x2s[i + 1] - x2s[i];
        double dy = y2s[i + 1] - y2s[i];
        distances.push_back(std::sqrt(dx * dx + dy * dy));
    }

    // Sort to get percentiles
    std::sort(distances.begin(), distances.end());

    size_t n = distances.size();
    size_t p10_idx = n / 10;
    size_t p90_idx = (n * 9) / 10;

    double p10 = distances[p10_idx];
    double p90 = distances[p90_idx];

    if (p10 < 1e-12) {
        // Very small P10 means strong fold (or numerical issues)
        // Use median as fallback denominator
        double median = distances[n / 2];
        if (median < 1e-12) return 0.0;
        p10 = median * 0.01;  // Treat as very small
    }

    double ratio = p90 / p10;

    // Ratio typically ranges from ~1 (start/uniform) to ~100+ (strong folds)
    // Normalize: log scale works well for ratios
    // ratio=1 → 0, ratio=10 → 1, ratio=100 → 2
    double log_ratio = std::log10(std::max(1.0, ratio));

    // Normalize to 0-1 range (ratio of 10-50 is typical for good caustics)
    double normalized = std::min(1.0, log_ratio / 2.0);

    // Combine with spread
    return spread * normalized;
}

double MetricsCollector::computeTrueFolds(
    std::vector<double> const& x2s,
    std::vector<double> const& y2s) const {
    // RENAMED PURPOSE: Neighbor Distance Gini
    //
    // Gini coefficient measures inequality in distribution.
    // At caustics: highly unequal distances (some tiny at folds, some normal)
    // In chaos: more uniform distribution (random but similar scale)
    // At start: all same distance → Gini = 0
    //
    // Unlike CV which is also high for random distributions,
    // Gini specifically measures "some values much smaller than others",
    // which is exactly what folds produce.

    if (x2s.size() < 10 || x2s.size() != y2s.size()) return 0.0;

    int N = static_cast<int>(x2s.size());

    // Compute spread
    auto const* spread_metric = getMetric(MetricNames::CircularSpread);
    double spread = 0.0;
    if (spread_metric && spread_metric->size() > 0) {
        spread = spread_metric->current();
    } else {
        double sum_r = 0.0;
        for (int i = 0; i < N; ++i) {
            sum_r += std::sqrt(x2s[i] * x2s[i] + y2s[i] * y2s[i]);
        }
        spread = std::min(1.0, (sum_r / N) / 2.0);
    }

    if (spread < 0.05) return 0.0;

    // Compute neighbor distances
    std::vector<double> distances;
    distances.reserve(N - 1);
    double sum_d = 0.0;
    for (int i = 0; i < N - 1; ++i) {
        double dx = x2s[i + 1] - x2s[i];
        double dy = y2s[i + 1] - y2s[i];
        double d = std::sqrt(dx * dx + dy * dy);
        distances.push_back(d);
        sum_d += d;
    }

    if (sum_d < 1e-12) return 0.0;

    // Sort for Gini calculation
    std::sort(distances.begin(), distances.end());

    // Compute Gini coefficient
    // Gini = (2 * sum(i * x[i]) - (n+1) * sum(x[i])) / (n * sum(x[i]))
    double weighted_sum = 0.0;
    for (size_t i = 0; i < distances.size(); ++i) {
        weighted_sum += (i + 1) * distances[i];
    }
    size_t n = distances.size();
    double gini = (2.0 * weighted_sum - (n + 1) * sum_d) / (n * sum_d);

    // Gini ranges 0 (perfect equality) to 1 (maximal inequality)
    // For random distributions (exponential/Rayleigh), Gini is typically 0.3-0.5
    // For fold distributions, Gini should be higher (0.6-0.8)
    //
    // Subtract the "chaos baseline" of ~0.4
    double adjusted_gini = std::max(0.0, (gini - 0.35) / 0.65);

    return spread * adjusted_gini;
}

double MetricsCollector::computeLocalCoherence(
    std::vector<double> const& x2s,
    std::vector<double> const& y2s) const {
    // RENAMED PURPOSE: Min/Median Ratio (fold strength)
    //
    // At folds, dpos/dθ → 0, meaning neighbor distances become tiny.
    // The minimum neighbor distance relative to median captures this:
    // - Caustic: min << median (strong folds) → low ratio → high metric
    // - Chaos: min is random sample, min/median ≈ 0.1-0.3 → moderate
    // - Start: all same → ratio ≈ 1 → low metric
    //
    // We invert: metric = (1 - min/median) × spread

    if (x2s.size() < 10 || x2s.size() != y2s.size()) return 0.0;

    int N = static_cast<int>(x2s.size());

    // Compute spread
    auto const* spread_metric = getMetric(MetricNames::CircularSpread);
    double spread = 0.0;
    if (spread_metric && spread_metric->size() > 0) {
        spread = spread_metric->current();
    } else {
        double sum_r = 0.0;
        for (int i = 0; i < N; ++i) {
            sum_r += std::sqrt(x2s[i] * x2s[i] + y2s[i] * y2s[i]);
        }
        spread = std::min(1.0, (sum_r / N) / 2.0);
    }

    if (spread < 0.05) return 0.0;

    // Compute neighbor distances
    std::vector<double> distances;
    distances.reserve(N - 1);
    for (int i = 0; i < N - 1; ++i) {
        double dx = x2s[i + 1] - x2s[i];
        double dy = y2s[i + 1] - y2s[i];
        distances.push_back(std::sqrt(dx * dx + dy * dy));
    }

    // Find min and median
    std::sort(distances.begin(), distances.end());
    double min_d = distances[0];
    double median_d = distances[distances.size() / 2];

    if (median_d < 1e-12) return 0.0;

    // Ratio of min to median
    // For folds: min is tiny, ratio → 0
    // For chaos: min is ~10% of median (random), ratio ≈ 0.1-0.3
    // For start: all same, ratio ≈ 1
    double ratio = min_d / median_d;

    // Invert and adjust: we want high value for small ratio (strong folds)
    // But we need to subtract the chaos baseline (~0.1-0.3)
    // metric = (1 - ratio) would give 0.7-0.9 for chaos, 0.99+ for folds
    //
    // Use log scale for better discrimination:
    // ratio = 0.001 (strong fold) → -log10 = 3
    // ratio = 0.1 (chaos) → -log10 = 1
    // ratio = 1 (start) → -log10 = 0
    double log_inverse = -std::log10(std::max(1e-6, ratio));

    // Normalize: log_inverse ranges 0-6, typical caustic is 1.5-3
    // Subtract chaos baseline of ~1
    double adjusted = std::max(0.0, (log_inverse - 1.0) / 2.5);

    return spread * std::min(1.0, adjusted);
}

} // namespace metrics
