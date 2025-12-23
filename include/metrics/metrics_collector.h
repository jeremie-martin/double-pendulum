#pragma once

#include "metrics/metric_series.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations for pendulum types
class Pendulum;
struct PendulumState;

namespace metrics {

// Metric type classification
enum class MetricType {
    Physics,  // Computed from pendulum state (variance, spread, causticness)
    GPU       // Computed from rendered frame (brightness, coverage)
};

// Bundle for GPU metrics (simplified - removed unused metrics)
struct GPUMetricsBundle {
    float max_value = 0.0f;
    float brightness = 0.0f;
    float coverage = 0.0f;
};

// Spread metrics (computed from angle1 distribution)
struct SpreadMetrics {
    double spread_ratio = 0.0;     // Fraction above horizontal (|angle1| > π/2)
    double circular_spread = 0.0;  // 1 - mean resultant length (0=concentrated,
                                   // 1=uniform)
    double angular_range = 0.0;    // Normalized angular coverage
    double angle1_mean = 0.0;
    double angle1_variance = 0.0;
};

// Snapshot of current metric values for GUI display
struct MetricSnapshot {
    std::string name;
    double current = 0.0;
    double derivative = 0.0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    MetricType type = MetricType::Physics;
};

// Central hub for all metrics - single source of truth
class MetricsCollector {
public:
    MetricsCollector();
    ~MetricsCollector();

    // Metric registration (call during initialization)
    void registerMetric(std::string const& name, MetricType type);
    void registerStandardMetrics();  // Registers all physics metrics
    void registerGPUMetrics();       // Registers all GPU metrics

    // Frame-by-frame updates
    void beginFrame(int frame_number);
    void setMetric(std::string const& name, double value);
    void setGPUMetrics(GPUMetricsBundle const& bundle);
    void updateGPUMetricsAtFrame(GPUMetricsBundle const& bundle, int frame);
    void endFrame();

    // Physics update from pendulum data
    void updateFromPendulums(std::vector<Pendulum> const& pendulums);

    // Update from angle vectors (for cases where we only have angles)
    void updateFromAngles(std::vector<double> const& angle1s,
                          std::vector<double> const& angle2s);

    // Reset all state
    void reset();

    // Access metrics by name
    MetricSeries<double> const* getMetric(std::string const& name) const;
    MetricSeries<double>* getMetricMutable(std::string const& name);

    // Get all metric names
    std::vector<std::string> getMetricNames() const;
    std::vector<std::string> getMetricNames(MetricType type) const;

    // Get metric type
    MetricType getMetricType(std::string const& name) const;

    // Bulk accessor for GUI
    std::vector<MetricSnapshot> getSnapshot() const;

    // Frame indexing
    int currentFrame() const { return current_frame_; }
    int frameCount() const;

    // Time conversion helper
    double frameToSeconds(int frame, double duration, int total_frames) const;

    // Spread metrics access (computed alongside variance)
    SpreadMetrics const& getCurrentSpread() const { return current_spread_; }
    std::vector<SpreadMetrics> const& getSpreadHistory() const {
        return spread_history_;
    }

    // Convenience accessors for common metrics
    double getVariance() const;
    double getSpreadRatio() const;  // Legacy: fraction above horizontal
    double getUniformity() const;   // Preferred: circular spread (0=concentrated, 1=uniform)
    double getBrightness() const;
    double getCoverage() const;

    // Check if a metric exists
    bool hasMetric(std::string const& name) const;

    // Export functionality (basic - full implementation in metrics_export.h)
    void exportCSV(std::string const& path,
                   std::vector<std::string> const& columns = {}) const;

private:
    std::unordered_map<std::string, MetricSeries<double>> metrics_;
    std::unordered_map<std::string, MetricType> metric_types_;

    int current_frame_ = -1;
    SpreadMetrics current_spread_;
    std::vector<SpreadMetrics> spread_history_;

    // Compute spread metrics from angle1 values
    SpreadMetrics computeSpread(std::vector<double> const& angle1s) const;

    // Compute variance from angle values
    double computeVariance(std::vector<double> const& angles) const;

    // Compute circular mean and resultant length
    std::pair<double, double>
    computeCircularStats(std::vector<double> const& angles) const;

    // Compute angular causticness from pendulum angles (physics-based)
    // Measures: sector coverage × density concentration
    double computeAngularCausticness(std::vector<double> const& angle1s,
                                     std::vector<double> const& angle2s) const;
};

// Standard metric names (use these constants for consistency)
namespace MetricNames {
// Physics metrics
constexpr const char* Variance = "variance";
constexpr const char* SpreadRatio = "spread_ratio";
constexpr const char* CircularSpread = "circular_spread";
constexpr const char* AngularRange = "angular_range";
constexpr const char* TotalEnergy = "total_energy";
constexpr const char* AngularCausticness = "angular_causticness";

// GPU metrics (simplified)
constexpr const char* MaxValue = "max_value";
constexpr const char* Brightness = "brightness";
constexpr const char* Coverage = "coverage";
} // namespace MetricNames

} // namespace metrics
