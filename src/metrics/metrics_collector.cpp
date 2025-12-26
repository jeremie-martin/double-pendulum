#include "metrics/metrics_collector.h"
#include "metrics/metric_registry.h"
#include "pendulum.h"
#include "simulation_data.h"

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

void MetricsCollector::setMetricConfig(std::string const& name, MetricConfig const& config) {
    metric_configs_[name] = config;
}

void MetricsCollector::setAllMetricConfigs(
    std::unordered_map<std::string, MetricConfig> const& configs) {
    metric_configs_ = configs;
}

MetricConfig const* MetricsCollector::getMetricConfig(std::string const& name) const {
    auto it = metric_configs_.find(name);
    return it != metric_configs_.end() ? &it->second : nullptr;
}

void MetricsCollector::registerMetric(std::string const& name, MetricType type) {
    if (metrics_.find(name) == metrics_.end()) {
        metrics_[name] = MetricSeries<double>();
        metric_types_[name] = type;
    }
}

void MetricsCollector::registerStandardMetrics() {
    // Register all physics metrics from the central registry
    for (auto const& m : METRIC_REGISTRY) {
        if (m.source == MetricSource::Physics) {
            registerMetric(m.name, MetricType::Physics);
        }
    }
}

void MetricsCollector::registerGPUMetrics() {
    // Register all GPU metrics from the central registry
    for (auto const& m : METRIC_REGISTRY) {
        if (m.source == MetricSource::GPU) {
            registerMetric(m.name, MetricType::GPU);
        }
    }
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
    std::vector<double> omega1s, omega2s;
    angle1s.reserve(pendulums.size());
    angle2s.reserve(pendulums.size());
    omega1s.reserve(pendulums.size());
    omega2s.reserve(pendulums.size());

    double total_energy = 0.0;

    // Note: L1 and L2 are fixed at 1.0 in this codebase (see pendulum.h defaults)
    constexpr double L1 = 1.0;
    constexpr double L2 = 1.0;

    for (auto const& p : pendulums) {
        angle1s.push_back(p.getTheta1());
        angle2s.push_back(p.getTheta2());
        omega1s.push_back(p.getOmega1());
        omega2s.push_back(p.getOmega2());
        total_energy += p.totalEnergy();
    }

    updateFromAngles(angle1s, angle2s);
    // Store mean energy per pendulum for N-independence
    // (total energy would scale linearly with N)
    setMetric(MetricNames::TotalEnergy, total_energy / pendulums.size());

    // Compute velocity-based metrics
    std::vector<double> vx2s, vy2s;
    computeTipVelocities(angle1s, angle2s, omega1s, omega2s, L1, L2, vx2s, vy2s);

    double vel_dispersion = computeVelocityDispersion(vx2s, vy2s);
    setMetric(MetricNames::VelocityDispersion, vel_dispersion);

    double speed_var = computeSpeedVariance(vx2s, vy2s);
    setMetric(MetricNames::SpeedVariance, speed_var);

    double bimodality = computeVelocityBimodality(vx2s, vy2s);
    setMetric(MetricNames::VelocityBimodality, bimodality);

    // Advanced velocity-based metrics
    // Note: Using default masses M1=M2=1.0 (same as default in pendulum.h)
    constexpr double M1 = 1.0;
    constexpr double M2 = 1.0;
    constexpr double G = 9.81;

    double ang_momentum = computeAngularMomentumSpread(angle1s, angle2s, omega1s, omega2s, L1, L2, M1, M2);
    setMetric(MetricNames::AngularMomentumSpread, ang_momentum);

    double accel_disp = computeAccelerationDispersion(angle1s, angle2s, omega1s, omega2s, L1, L2, G);
    setMetric(MetricNames::AccelerationDispersion, accel_disp);
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

    // Get per-metric params (falls back to defaults if not configured)
    auto params = getMetricParams<SectorMetricParams>(MetricNames::AngularCausticness);

    // Scale number of sectors based on N for consistent statistics
    // Target: ~30-50 pendulums per sector for meaningful statistics
    // This makes the metric independent of pendulum count
    int N = static_cast<int>(angle1s.size());
    int num_sectors = std::max(params.min_sectors,
                               std::min(params.max_sectors, N / params.target_per_sector));
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

    // Compute coverage with birthday-problem correction for N-independence
    int occupied_sectors = 0;
    int total_count = 0;

    for (int count : sector_counts) {
        if (count > 0) occupied_sectors++;
        total_count += count;
    }

    if (total_count == 0) return 0.0;

    double raw_coverage = static_cast<double>(occupied_sectors) / num_sectors;

    // Birthday problem correction: compute expected coverage for uniform distribution
    // E[occupied] = M * (1 - (1-1/M)^N), so expected_fraction = 1 - (1-1/M)^N
    double p_empty = std::pow(1.0 - 1.0 / num_sectors, N);
    double expected_coverage = 1.0 - p_empty;

    // Normalized coverage: 1.0 means exactly as expected for uniform
    double normalized_coverage = (expected_coverage > 0.01)
        ? std::min(1.0, raw_coverage / expected_coverage)
        : raw_coverage;

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

    // Causticness = normalized_coverage × gini
    // - Early phase: low coverage × high gini = LOW
    // - Interesting phase: medium coverage × medium gini = HIGH
    // - Chaos: high coverage × low gini = LOW
    return normalized_coverage * gini;
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
    auto r1_params = getMetricParams<SectorMetricParams>(MetricNames::R1);
    auto r2_params = getMetricParams<SectorMetricParams>(MetricNames::R2);
    double r1 = computeCausticnessFromAngles(angle1s, r1_params);
    double r2 = computeCausticnessFromAngles(angle2s, r2_params);
    setMetric(MetricNames::R1, r1);
    setMetric(MetricNames::R2, r2);
    setMetric(MetricNames::JointConcentration, r1 * r2);

    // Compute new caustic metrics - Tier 2: position-based
    double tip_causticness = computeTipCausticness(x2s, y2s);
    setMetric(MetricNames::TipCausticness, tip_causticness);

    // Alternative caustic metrics (experimental)
    double cv_causticness = computeCVCausticness(angle1s, angle2s);
    setMetric(MetricNames::CVCausticness, cv_causticness);

    double organization = computeOrganizationCausticness(angle1s, angle2s);
    setMetric(MetricNames::OrganizationCausticness, organization);

    // Local coherence metric
    double local_coherence = computeLocalCoherence(x2s, y2s);
    setMetric(MetricNames::LocalCoherence, local_coherence);

    // === Velocity-based metrics ===
    // Now that PendulumState includes w1/w2, we can compute velocity metrics
    std::vector<double> w1s, w2s;
    w1s.reserve(states.size());
    w2s.reserve(states.size());
    for (auto const& s : states) {
        w1s.push_back(s.w1);
        w2s.push_back(s.w2);
    }

    // Physics constants (fixed in this codebase)
    constexpr double L1 = 1.0, L2 = 1.0, M1 = 1.0, M2 = 1.0, G = 9.81;

    std::vector<double> vx2s, vy2s;
    computeTipVelocities(angle1s, angle2s, w1s, w2s, L1, L2, vx2s, vy2s);

    setMetric(MetricNames::VelocityDispersion, computeVelocityDispersion(vx2s, vy2s));
    setMetric(MetricNames::SpeedVariance, computeSpeedVariance(vx2s, vy2s));
    setMetric(MetricNames::VelocityBimodality, computeVelocityBimodality(vx2s, vy2s));
    setMetric(MetricNames::AngularMomentumSpread,
              computeAngularMomentumSpread(angle1s, angle2s, w1s, w2s, L1, L2, M1, M2));
    setMetric(MetricNames::AccelerationDispersion,
              computeAccelerationDispersion(angle1s, angle2s, w1s, w2s, L1, L2, G));
}

void MetricsCollector::updateFromPackedStates(
    simulation_data::PackedState const* states, size_t count) {
    if (!states || count == 0) return;

    // Reuse internal buffers to avoid allocation
    angle1_buf_.resize(count);
    angle2_buf_.resize(count);
    x2_buf_.resize(count);
    y2_buf_.resize(count);
    w1_buf_.resize(count);
    w2_buf_.resize(count);

    // Extract data from packed states (float -> double)
    for (size_t i = 0; i < count; ++i) {
        angle1_buf_[i] = static_cast<double>(states[i].th1);
        angle2_buf_[i] = static_cast<double>(states[i].th2);
        x2_buf_[i] = static_cast<double>(states[i].x2);
        y2_buf_[i] = static_cast<double>(states[i].y2);
        w1_buf_[i] = static_cast<double>(states[i].w1);
        w2_buf_[i] = static_cast<double>(states[i].w2);
    }

    // Call existing angle-based computations (variance, spread, angular_causticness)
    updateFromAngles(angle1_buf_, angle2_buf_);

    // Compute new caustic metrics - Tier 1: angle-based
    auto r1_params = getMetricParams<SectorMetricParams>(MetricNames::R1);
    auto r2_params = getMetricParams<SectorMetricParams>(MetricNames::R2);
    double r1 = computeCausticnessFromAngles(angle1_buf_, r1_params);
    double r2 = computeCausticnessFromAngles(angle2_buf_, r2_params);
    setMetric(MetricNames::R1, r1);
    setMetric(MetricNames::R2, r2);
    setMetric(MetricNames::JointConcentration, r1 * r2);

    // Compute new caustic metrics - Tier 2: position-based
    double tip_causticness = computeTipCausticness(x2_buf_, y2_buf_);
    setMetric(MetricNames::TipCausticness, tip_causticness);

    // Alternative caustic metrics (experimental)
    double cv_causticness = computeCVCausticness(angle1_buf_, angle2_buf_);
    setMetric(MetricNames::CVCausticness, cv_causticness);

    double organization = computeOrganizationCausticness(angle1_buf_, angle2_buf_);
    setMetric(MetricNames::OrganizationCausticness, organization);

    // Local coherence metric
    double local_coherence = computeLocalCoherence(x2_buf_, y2_buf_);
    setMetric(MetricNames::LocalCoherence, local_coherence);

    // === Velocity-based metrics ===
    // Physics constants (fixed in this codebase)
    constexpr double L1 = 1.0, L2 = 1.0, M1 = 1.0, M2 = 1.0, G = 9.81;

    std::vector<double> vx2s, vy2s;
    computeTipVelocities(angle1_buf_, angle2_buf_, w1_buf_, w2_buf_, L1, L2, vx2s, vy2s);

    setMetric(MetricNames::VelocityDispersion, computeVelocityDispersion(vx2s, vy2s));
    setMetric(MetricNames::SpeedVariance, computeSpeedVariance(vx2s, vy2s));
    setMetric(MetricNames::VelocityBimodality, computeVelocityBimodality(vx2s, vy2s));
    setMetric(MetricNames::AngularMomentumSpread,
              computeAngularMomentumSpread(angle1_buf_, angle2_buf_, w1_buf_, w2_buf_, L1, L2, M1, M2));
    setMetric(MetricNames::AccelerationDispersion,
              computeAccelerationDispersion(angle1_buf_, angle2_buf_, w1_buf_, w2_buf_, L1, L2, G));
}

double MetricsCollector::computeCausticnessFromAngles(
    std::vector<double> const& angles,
    SectorMetricParams const& params) const {
    if (angles.empty()) return 0.0;

    // Scale number of sectors based on N for consistent statistics
    int N = static_cast<int>(angles.size());
    int num_sectors = std::max(params.min_sectors,
                               std::min(params.max_sectors, N / params.target_per_sector));
    double sector_width = TWO_PI / num_sectors;

    std::vector<int> sector_counts(num_sectors, 0);

    for (double angle : angles) {
        // Normalize to [0, 2π)
        double normalized = std::fmod(angle, TWO_PI);
        if (normalized < 0) normalized += TWO_PI;

        int sector = static_cast<int>(normalized / sector_width) % num_sectors;
        sector_counts[sector]++;
    }

    // Compute coverage with birthday-problem correction for N-independence
    // Expected coverage under uniform random: E[occupied] = M * (1 - (1-1/M)^N)
    // By normalizing observed/expected, we get N-independent coverage measure
    int occupied_sectors = 0;
    int total_count = 0;

    for (int count : sector_counts) {
        if (count > 0) occupied_sectors++;
        total_count += count;
    }

    if (total_count == 0) return 0.0;

    double raw_coverage = static_cast<double>(occupied_sectors) / num_sectors;

    // Birthday problem correction: compute expected coverage for uniform distribution
    double p_empty = std::pow(1.0 - 1.0 / num_sectors, N);  // Prob(sector empty)
    double expected_coverage = 1.0 - p_empty;

    // Normalized coverage: 1.0 means exactly as expected for uniform, >1 means more spread
    // Clamp to [0, 1] for the final metric
    double normalized_coverage = (expected_coverage > 0.01)
        ? std::min(1.0, raw_coverage / expected_coverage)
        : raw_coverage;

    // Compute Gini coefficient (inherently N-independent)
    std::sort(sector_counts.begin(), sector_counts.end());

    double gini_sum = 0.0;
    for (int i = 0; i < num_sectors; ++i) {
        gini_sum += (2.0 * (i + 1) - num_sectors - 1) * sector_counts[i];
    }
    double gini = gini_sum / (num_sectors * static_cast<double>(total_count));

    return normalized_coverage * gini;
}

double MetricsCollector::computeTipCausticness(
    std::vector<double> const& x2s,
    std::vector<double> const& y2s) const {
    if (x2s.empty() || x2s.size() != y2s.size()) return 0.0;

    // Get per-metric params
    auto params = getMetricParams<SectorMetricParams>(MetricNames::TipCausticness);

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
    return computeCausticnessFromAngles(tip_angles, params);
}

double MetricsCollector::computeCVCausticness(
    std::vector<double> const& angle1s,
    std::vector<double> const& angle2s) const {
    // CV-based causticness: uses coefficient of variation instead of Gini
    // CV = σ/μ is more sensitive to "spikiness" that characterizes caustics

    if (angle1s.empty() || angle1s.size() != angle2s.size()) return 0.0;

    // Get per-metric params
    auto params = getMetricParams<CVSectorMetricParams>(MetricNames::CVCausticness);

    // Scale sectors adaptively for N-independence
    int N = static_cast<int>(angle1s.size());
    int num_sectors = std::max(params.min_sectors,
                               std::min(params.max_sectors, N / params.target_per_sector));
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

    // Compute coverage with birthday-problem correction
    int occupied_sectors = 0;
    for (int count : sector_counts) {
        if (count > 0) occupied_sectors++;
    }
    double raw_coverage = static_cast<double>(occupied_sectors) / num_sectors;

    // Birthday problem correction for N-independence
    double p_empty = std::pow(1.0 - 1.0 / num_sectors, N);
    double expected_coverage = 1.0 - p_empty;
    double normalized_coverage = (expected_coverage > 0.01)
        ? std::min(1.0, raw_coverage / expected_coverage)
        : raw_coverage;

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
    // Then multiply by normalized coverage for the desired low→high→low pattern
    double normalized_cv = std::min(1.0, cv / params.cv_normalization);

    return normalized_coverage * normalized_cv;
}

double MetricsCollector::computeOrganizationCausticness(
    std::vector<double> const& angle1s,
    std::vector<double> const& angle2s) const {
    // Organization causticness: (1 - R1*R2) × coverage
    // - R1, R2 are mean resultant lengths (0=dispersed, 1=concentrated)
    // - (1 - R1*R2) is high when angles are spread but not fully random
    // - Coverage ensures we're actually exploring the space

    if (angle1s.empty() || angle1s.size() != angle2s.size()) return 0.0;

    // Get per-metric params
    auto params = getMetricParams<SectorMetricParams>(MetricNames::OrganizationCausticness);

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
    int N = static_cast<int>(angle1s.size());
    int num_sectors = std::max(params.min_sectors,
                               std::min(params.max_sectors, N / params.target_per_sector));
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
    double raw_coverage = static_cast<double>(occupied_count) / num_sectors;

    // Birthday problem correction for N-independence
    double p_empty = std::pow(1.0 - 1.0 / num_sectors, N);
    double expected_coverage = 1.0 - p_empty;
    double normalized_coverage = (expected_coverage > 0.01)
        ? std::min(1.0, raw_coverage / expected_coverage)
        : raw_coverage;

    // Organization = (1 - R1*R2) × normalized_coverage
    // - Early: R1≈1, R2≈1 → (1-1)=0 → LOW
    // - Caustic: R moderate → (1-R1*R2) moderate, coverage high → HIGH
    // - Chaos: R≈0 → (1-0)=1, but also high coverage → HIGH (this is expected)
    double organization = (1.0 - R1 * R2) * normalized_coverage;

    return organization;
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

    // Get per-metric params
    auto params = getMetricParams<LocalCoherenceMetricParams>(MetricNames::LocalCoherence);

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
        spread = std::min(1.0, (sum_r / N) / params.max_radius);
    }

    if (spread < params.min_spread_threshold) return 0.0;

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
    // Subtract chaos baseline
    double adjusted = std::max(0.0,
        (log_inverse - params.log_inverse_baseline) / params.log_inverse_divisor);

    return spread * std::min(1.0, adjusted);
}

// === VELOCITY-BASED METRICS ===
// These metrics capture the dynamics (velocities) rather than just positions
// Key insight: at boom, pendulums slow down then rapidly diverge in opposite directions

void MetricsCollector::computeTipVelocities(
    std::vector<double> const& th1s,
    std::vector<double> const& th2s,
    std::vector<double> const& w1s,
    std::vector<double> const& w2s,
    double L1, double L2,
    std::vector<double>& vx2s,
    std::vector<double>& vy2s) const {

    size_t N = th1s.size();
    vx2s.resize(N);
    vy2s.resize(N);

    // Tip velocity from pendulum kinematics:
    // x2 = L1*sin(θ1) + L2*sin(θ1+θ2)
    // y2 = L1*cos(θ1) + L2*cos(θ1+θ2)
    //
    // Taking derivatives (θ2 is relative angle, so combined angular velocity is ω1+ω2):
    // vx2 = L1*ω1*cos(θ1) + L2*(ω1+ω2)*cos(θ1+θ2)
    // vy2 = -L1*ω1*sin(θ1) - L2*(ω1+ω2)*sin(θ1+θ2)

    for (size_t i = 0; i < N; ++i) {
        double th1 = th1s[i];
        double th2 = th2s[i];
        double w1 = w1s[i];
        double w2 = w2s[i];

        double th12 = th1 + th2;  // Combined angle for second arm
        double w12 = w1 + w2;     // Combined angular velocity

        vx2s[i] = L1 * w1 * std::cos(th1) + L2 * w12 * std::cos(th12);
        vy2s[i] = -L1 * w1 * std::sin(th1) - L2 * w12 * std::sin(th12);
    }
}

double MetricsCollector::computeVelocityDispersion(
    std::vector<double> const& vx2s,
    std::vector<double> const& vy2s) const {
    // Velocity Dispersion: How spread out are velocity DIRECTIONS?
    //
    // Uses circular statistics on velocity direction angles:
    // 1 - R (mean resultant length), same as circular_spread
    //
    // Expected behavior:
    // - Start: all moving same direction → dispersion ≈ 0
    // - Boom: half left, half right → dispersion HIGH (maybe bimodal, but still dispersed)
    // - Chaos: random directions → dispersion ≈ 1 (uniform on circle)
    //
    // This is N-independent by construction (circular mean is normalized by N)

    if (vx2s.empty() || vx2s.size() != vy2s.size()) return 0.0;

    double cos_sum = 0.0;
    double sin_sum = 0.0;

    for (size_t i = 0; i < vx2s.size(); ++i) {
        double speed = std::sqrt(vx2s[i] * vx2s[i] + vy2s[i] * vy2s[i]);
        if (speed < 1e-12) continue;  // Skip stationary pendulums

        // Velocity direction angle
        double vangle = std::atan2(vy2s[i], vx2s[i]);
        cos_sum += std::cos(vangle);
        sin_sum += std::sin(vangle);
    }

    double n = static_cast<double>(vx2s.size());
    double cos_mean = cos_sum / n;
    double sin_mean = sin_sum / n;

    // Mean resultant length R
    double R = std::sqrt(cos_mean * cos_mean + sin_mean * sin_mean);

    // Dispersion = 1 - R (0 = all same direction, 1 = uniform/dispersed)
    return 1.0 - R;
}

double MetricsCollector::computeSpeedVariance(
    std::vector<double> const& vx2s,
    std::vector<double> const& vy2s) const {
    // Speed Variance: Normalized variance of tip speeds
    //
    // Expected behavior:
    // - Start: all same speed → variance ≈ 0
    // - Boom: some fast, some slow (different divergence rates) → variance HIGH
    // - Chaos: random speeds → moderate variance
    //
    // We use coefficient of variation (CV = std/mean) for scale-independence

    if (vx2s.empty() || vx2s.size() != vy2s.size()) return 0.0;

    // Compute speeds
    std::vector<double> speeds;
    speeds.reserve(vx2s.size());

    for (size_t i = 0; i < vx2s.size(); ++i) {
        speeds.push_back(std::sqrt(vx2s[i] * vx2s[i] + vy2s[i] * vy2s[i]));
    }

    // Compute mean and variance
    double sum = 0.0, sum2 = 0.0;
    for (double s : speeds) {
        sum += s;
        sum2 += s * s;
    }

    double n = static_cast<double>(speeds.size());
    double mean = sum / n;
    double variance = sum2 / n - mean * mean;

    if (mean < 1e-12) return 0.0;  // All stationary

    // Coefficient of variation
    double cv = std::sqrt(std::max(0.0, variance)) / mean;

    // Normalize to roughly 0-1 range (CV > 1 is quite spiky)
    return std::min(1.0, cv);
}

double MetricsCollector::computeVelocityBimodality(
    std::vector<double> const& vx2s,
    std::vector<double> const& vy2s) const {
    // Velocity Bimodality: Detects "half left, half right" pattern
    //
    // At the boom moment, pendulums diverge into TWO distinct groups
    // moving in opposite directions. This is the visual "explosion".
    //
    // Algorithm:
    // 1. Compute principal direction (mean velocity direction)
    // 2. Project all velocities onto this axis
    // 3. Measure how bimodal the projection is (two peaks on opposite sides)
    //
    // Bimodality metric: |mean of positive projections| + |mean of negative|
    // divided by total mean, minus 1 for centering.
    //
    // Expected behavior:
    // - Start: all same direction → all positive (or negative) → LOW bimodality
    // - Boom: half +, half - → HIGH bimodality
    // - Chaos: uniform random → cancels out → moderate bimodality

    if (vx2s.size() < 2 || vx2s.size() != vy2s.size()) return 0.0;

    // Find principal direction (mean velocity)
    double mean_vx = 0.0, mean_vy = 0.0;
    for (size_t i = 0; i < vx2s.size(); ++i) {
        mean_vx += vx2s[i];
        mean_vy += vy2s[i];
    }
    double n = static_cast<double>(vx2s.size());
    mean_vx /= n;
    mean_vy /= n;

    // If mean velocity is near zero, use dominant direction from SVD-like approach
    // For simplicity, use the direction with maximum spread
    double vx_var = 0.0, vy_var = 0.0, vxy_covar = 0.0;
    for (size_t i = 0; i < vx2s.size(); ++i) {
        double dvx = vx2s[i] - mean_vx;
        double dvy = vy2s[i] - mean_vy;
        vx_var += dvx * dvx;
        vy_var += dvy * dvy;
        vxy_covar += dvx * dvy;
    }
    vx_var /= n;
    vy_var /= n;
    vxy_covar /= n;

    // Principal direction from 2D covariance (eigenvector of largest eigenvalue)
    // For 2x2 matrix [[a, b], [b, c]], eigenvalues are (a+c)/2 ± sqrt(((a-c)/2)^2 + b^2)
    double trace = vx_var + vy_var;
    double det = vx_var * vy_var - vxy_covar * vxy_covar;
    double disc = std::sqrt(std::max(0.0, trace * trace / 4.0 - det));
    double lambda1 = trace / 2.0 + disc;  // Larger eigenvalue

    if (lambda1 < 1e-12) return 0.0;  // No variance at all

    // Eigenvector for lambda1: [vxy_covar, lambda1 - vx_var] (or similar)
    double ax, ay;
    if (std::abs(vxy_covar) > 1e-12) {
        ax = vxy_covar;
        ay = lambda1 - vx_var;
    } else if (vx_var > vy_var) {
        ax = 1.0;
        ay = 0.0;
    } else {
        ax = 0.0;
        ay = 1.0;
    }

    // Normalize
    double anorm = std::sqrt(ax * ax + ay * ay);
    if (anorm < 1e-12) return 0.0;
    ax /= anorm;
    ay /= anorm;

    // Project velocities onto principal axis
    std::vector<double> projections;
    projections.reserve(vx2s.size());
    for (size_t i = 0; i < vx2s.size(); ++i) {
        double proj = vx2s[i] * ax + vy2s[i] * ay;
        projections.push_back(proj);
    }

    // Compute bimodality: look at distribution of projections
    // Count positive and negative, compute means of each group
    double pos_sum = 0.0, neg_sum = 0.0;
    int pos_count = 0, neg_count = 0;

    for (double p : projections) {
        if (p > 0) {
            pos_sum += p;
            pos_count++;
        } else {
            neg_sum += p;  // Note: negative values
            neg_count++;
        }
    }

    // Bimodality high when both groups are populated and moving opposite directions
    if (pos_count == 0 || neg_count == 0) return 0.0;

    double pos_mean = pos_sum / pos_count;
    double neg_mean = neg_sum / neg_count;  // This is negative

    // The "gap" between groups relative to overall spread
    double gap = pos_mean - neg_mean;  // Always positive (pos_mean > 0, neg_mean < 0)

    // Overall standard deviation for normalization
    double total_var = 0.0;
    double total_mean = (pos_sum + neg_sum) / n;
    for (double p : projections) {
        double d = p - total_mean;
        total_var += d * d;
    }
    total_var /= n;
    double std_dev = std::sqrt(std::max(1e-12, total_var));

    // Bimodality = gap / (2 * std_dev), roughly in [0, 2] for bimodal
    // Perfect bimodal (two delta functions) would have gap = 2*std_dev
    double bimodality = gap / (2.0 * std_dev);

    // Also factor in balance: most bimodal when 50/50 split
    double balance = 4.0 * (pos_count / n) * (neg_count / n);  // Peak at 0.5/0.5

    // Combine: high when both bimodal AND balanced
    return std::min(1.0, bimodality * balance);
}

double MetricsCollector::computeAngularMomentumSpread(
    std::vector<double> const& th1s,
    std::vector<double> const& th2s,
    std::vector<double> const& w1s,
    std::vector<double> const& w2s,
    double L1, double L2,
    double /*M1*/, double M2) const {
    // Angular Momentum Spread: Circular spread of angular momenta
    //
    // Each pendulum has an angular momentum L = r × p (cross product).
    // For a double pendulum, we compute the total angular momentum about the pivot.
    //
    // The angular momentum has a sign (direction perpendicular to plane),
    // so we can analyze its distribution using circular statistics
    // (treating sign as a direction on a circle).
    //
    // Expected behavior:
    // - Start: all same angular momentum → spread ≈ 0
    // - Boom: some clockwise, some counterclockwise → spread HIGH
    // - Chaos: random distribution → spread high but different pattern
    //
    // This is N-independent (circular statistics normalized by N).

    if (th1s.empty()) return 0.0;

    size_t N = th1s.size();

    // For a double pendulum, total angular momentum about the pivot:
    // L = I1*ω1 + I2*(ω1+ω2) + M2*L1*L2*ω1*cos(θ2) + M2*L1*L2*(ω1+ω2)*cos(θ2)
    //
    // Simplified for analysis: we care about the SIGN and relative magnitude,
    // so we use a simpler approximation based on tip angular momentum:
    // L_tip ≈ M2 * (x2 * vy2 - y2 * vx2)
    //
    // This gives the angular momentum of the tip about the pivot.

    std::vector<double> angular_momenta;
    angular_momenta.reserve(N);

    for (size_t i = 0; i < N; ++i) {
        double th1 = th1s[i];
        double th2 = th2s[i];
        double w1 = w1s[i];
        double w2 = w2s[i];

        // Tip position
        double x2 = L1 * std::sin(th1) + L2 * std::sin(th1 + th2);
        double y2 = L1 * std::cos(th1) + L2 * std::cos(th1 + th2);

        // Tip velocity
        double th12 = th1 + th2;
        double w12 = w1 + w2;
        double vx2 = L1 * w1 * std::cos(th1) + L2 * w12 * std::cos(th12);
        double vy2 = -L1 * w1 * std::sin(th1) - L2 * w12 * std::sin(th12);

        // Angular momentum about pivot: L = r × v (2D cross product = scalar)
        double L_z = M2 * (x2 * vy2 - y2 * vx2);
        angular_momenta.push_back(L_z);
    }

    // Use angular momentum sign/magnitude to compute spread
    // Convert to "angle" by mapping positive L to 0, negative to π
    // This captures the "clockwise vs counterclockwise" distinction

    // First, find the scale (max absolute angular momentum)
    double max_abs_L = 0.0;
    for (double L : angular_momenta) {
        max_abs_L = std::max(max_abs_L, std::abs(L));
    }

    if (max_abs_L < 1e-12) return 0.0;  // All stationary

    // Count positive and negative angular momenta
    int pos_count = 0, neg_count = 0;
    for (double L : angular_momenta) {
        if (L > 0) pos_count++;
        else neg_count++;
    }

    // Bimodality: peak when 50/50 split
    double balance = 4.0 * (pos_count / static_cast<double>(N)) *
                          (neg_count / static_cast<double>(N));

    // Also compute variance of magnitudes
    double sum_L = 0.0, sum_L2 = 0.0;
    for (double L : angular_momenta) {
        sum_L += std::abs(L);
        sum_L2 += L * L;
    }
    double mean_abs_L = sum_L / N;
    double var_L = sum_L2 / N - (sum_L / N) * (sum_L / N);
    double cv = (mean_abs_L > 1e-12) ? std::sqrt(std::max(0.0, var_L)) / mean_abs_L : 0.0;

    // Combine balance and variance spread
    // High when: half clockwise/half counter + varied magnitudes
    double magnitude_spread = std::min(1.0, cv);

    return balance * 0.7 + magnitude_spread * 0.3;
}

double MetricsCollector::computeAccelerationDispersion(
    std::vector<double> const& th1s,
    std::vector<double> const& th2s,
    std::vector<double> const& w1s,
    std::vector<double> const& w2s,
    double L1, double L2, double G) const {
    // Acceleration Dispersion: How spread out are tip ACCELERATIONS?
    //
    // Acceleration captures the instantaneous force pattern.
    // At the boom moment, some pendulums are accelerating left, others right.
    //
    // We use circular statistics on acceleration direction angles,
    // same as velocity_dispersion but for acceleration.
    //
    // Acceleration is computed from the equations of motion (derived from Lagrangian).

    if (th1s.empty()) return 0.0;

    size_t N = th1s.size();

    // For double pendulum, the angular accelerations are:
    // α1 = f1(θ1, θ2, ω1, ω2, g, L1, L2, M1, M2)
    // α2 = f2(θ1, θ2, ω1, ω2, g, L1, L2, M1, M2)
    //
    // The full equations are complex. For this metric, we use a simplified
    // approach: compute the gravitational torque contribution which dominates
    // at the boom moment (when velocities are changing direction).

    std::vector<double> ax2s, ay2s;
    ax2s.reserve(N);
    ay2s.reserve(N);

    // Simplified: use gravitational acceleration component
    // Real tip acceleration would require full EOM, but direction is what matters
    for (size_t i = 0; i < N; ++i) {
        double th1 = th1s[i];
        double th2 = th2s[i];
        double w1 = w1s[i];
        double w2 = w2s[i];

        // Centripetal and gravitational contributions to tip acceleration
        // ax2 ≈ -L1*ω1²*sin(θ1) - L2*(ω1+ω2)²*sin(θ1+θ2) + gravity_term
        // ay2 ≈ -L1*ω1²*cos(θ1) - L2*(ω1+ω2)²*cos(θ1+θ2) + gravity_term

        double th12 = th1 + th2;
        double w12 = w1 + w2;

        // Centripetal acceleration (always toward pivot)
        double ax_cent = -L1 * w1 * w1 * std::sin(th1) - L2 * w12 * w12 * std::sin(th12);
        double ay_cent = -L1 * w1 * w1 * std::cos(th1) - L2 * w12 * w12 * std::cos(th12);

        // Tangential acceleration from gravity (simplified)
        // This is an approximation - full EOM would be more accurate
        double ax_grav = -G * std::sin(th1) * 0.5;  // Rough estimate
        double ay_grav = G * (1.0 - std::cos(th1)) * 0.5;

        ax2s.push_back(ax_cent + ax_grav);
        ay2s.push_back(ay_cent + ay_grav);
    }

    // Compute circular dispersion of acceleration directions
    double cos_sum = 0.0, sin_sum = 0.0;

    for (size_t i = 0; i < N; ++i) {
        double a_mag = std::sqrt(ax2s[i] * ax2s[i] + ay2s[i] * ay2s[i]);
        if (a_mag < 1e-12) continue;

        double a_angle = std::atan2(ay2s[i], ax2s[i]);
        cos_sum += std::cos(a_angle);
        sin_sum += std::sin(a_angle);
    }

    double n = static_cast<double>(N);
    double cos_mean = cos_sum / n;
    double sin_mean = sin_sum / n;

    // Mean resultant length R
    double R = std::sqrt(cos_mean * cos_mean + sin_mean * sin_mean);

    // Dispersion = 1 - R
    return 1.0 - R;
}

} // namespace metrics
