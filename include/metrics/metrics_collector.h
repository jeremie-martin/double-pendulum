#pragma once

#include "config.h"
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
namespace simulation_data { struct PackedState; }

namespace metrics {

// Metric type classification
enum class MetricType {
    Physics,  // Computed from pendulum state (variance, spread, causticness)
    GPU       // Computed from rendered frame (brightness, coverage)
};

// Bundle for GPU metrics from rendered frames
//
// SIMPLIFIED from original version. Removed fields that were computed but unused:
//   - edge_energy: Was intended for detecting visual sharpness, but wasn't
//     correlated with perceived quality. Removed to reduce GPU compute.
//   - contrast: Similar to brightness in practice. Not useful for filtering.
//   - color_variance: Only relevant for multi-color schemes, rarely used.
//
// If you need these metrics back, add them to GLRenderer::computeMetrics()
// and the corresponding accessors, then add fields here.
struct GPUMetricsBundle {
    float max_value = 0.0f;   // Peak pixel intensity (before post-processing)
    float brightness = 0.0f;  // Mean pixel intensity (0-1 range)
    float coverage = 0.0f;    // Fraction of non-zero pixels (0-1 range)
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

    // Per-metric configuration (runtime configurable)
    void setMetricConfig(std::string const& name, MetricConfig const& config);
    void setAllMetricConfigs(std::unordered_map<std::string, MetricConfig> const& configs);
    MetricConfig const* getMetricConfig(std::string const& name) const;

    // Helper to get typed params for a specific metric (returns defaults if not configured)
    template<typename T>
    T getMetricParams(std::string const& name) const {
        if (auto const* mc = getMetricConfig(name)) {
            if (auto const* p = std::get_if<T>(&mc->params)) {
                return *p;
            }
        }
        // Return default-constructed params
        return T{};
    }

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

    // Update from full pendulum states (enables position-based metrics)
    void updateFromStates(std::vector<PendulumState> const& states);

    // Update from packed states (zero-copy from simulation_data::Reader)
    void updateFromPackedStates(simulation_data::PackedState const* states, size_t count);

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
    std::unordered_map<std::string, MetricConfig> metric_configs_;  // Per-metric parameters

    int current_frame_ = -1;
    SpreadMetrics current_spread_;
    std::vector<SpreadMetrics> spread_history_;

    // Reusable buffers to avoid allocations in hot path
    mutable std::vector<double> angle1_buf_;
    mutable std::vector<double> angle2_buf_;
    mutable std::vector<double> x2_buf_;
    mutable std::vector<double> y2_buf_;

    // Maximum spread_history size (0 = unlimited)
    // For long-running GUI, keeps memory bounded (~40 bytes per entry)
    static constexpr size_t MAX_SPREAD_HISTORY = 10000;  // ~3 minutes at 60fps

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

    // Compute tip causticness using geometrically correct tip angle atan2(x2, y2)
    double computeTipCausticness(std::vector<double> const& x2s,
                                 std::vector<double> const& y2s) const;

    // Compute spatial concentration from 2D histogram of tip positions
    // Returns: coverage × gini on 2D histogram
    double computeSpatialConcentration(std::vector<double> const& x2s,
                                       std::vector<double> const& y2s) const;

    // Helper: compute causticness from any angle vector (shared by angular and tip)
    double computeCausticnessFromAngles(std::vector<double> const& angles,
                                        SectorMetricParams const& params) const;

    // Alternative: CV-based causticness (coefficient of variation instead of Gini)
    double computeCVCausticness(std::vector<double> const& angle1s,
                                std::vector<double> const& angle2s) const;

    // Organization causticness: (1 - R1*R2) × coverage
    // High when spread out but not fully random
    double computeOrganizationCausticness(std::vector<double> const& angle1s,
                                          std::vector<double> const& angle2s) const;

    // Fold causticness: leverages natural ordering of pendulums
    // Measures CV of adjacent-pair distances × spatial spread
    double computeFoldCausticness(std::vector<double> const& x2s,
                                  std::vector<double> const& y2s) const;

    // === New paradigm metrics (local coherence based) ===

    // Trajectory smoothness: how predictable is pos[i+1] from pos[i]?
    // High when curves are smooth (start, caustic), low in chaos
    double computeTrajectorySmoothness(std::vector<double> const& x2s,
                                       std::vector<double> const& y2s) const;

    // Curvature: mean curvature of the θ→(x,y) parametric curve
    // Peaks at folds where the curve bends sharply
    double computeCurvature(std::vector<double> const& x2s,
                            std::vector<double> const& y2s) const;

    // True folds: count of trajectory crossings (pos[i] ≈ pos[j] for non-adjacent i,j)
    // Directly detects caustic envelope intersections
    double computeTrueFolds(std::vector<double> const& x2s,
                            std::vector<double> const& y2s) const;

    // Local coherence: are index-neighbors also spatial-neighbors?
    // High at caustics (local structure), low in chaos (random)
    double computeLocalCoherence(std::vector<double> const& x2s,
                                 std::vector<double> const& y2s) const;

    // === VELOCITY-BASED METRICS ===
    // These require angular velocities (omega1, omega2) and use tip velocities
    // to detect the "boom moment" characterized by opposing motion

    // Compute tip velocities from angles and angular velocities
    // Returns (vx2, vy2) for each pendulum
    void computeTipVelocities(std::vector<double> const& th1s,
                              std::vector<double> const& th2s,
                              std::vector<double> const& w1s,
                              std::vector<double> const& w2s,
                              double L1, double L2,
                              std::vector<double>& vx2s,
                              std::vector<double>& vy2s) const;

    // Velocity dispersion: how spread out are velocity directions?
    // Uses circular statistics on velocity direction angles
    // 0 = all same direction, 1 = uniformly spread
    double computeVelocityDispersion(std::vector<double> const& vx2s,
                                     std::vector<double> const& vy2s) const;

    // Speed variance: normalized variance of tip speeds
    // High when some pendulums fast, others slow
    double computeSpeedVariance(std::vector<double> const& vx2s,
                                std::vector<double> const& vy2s) const;

    // Velocity bimodality: detects "half left, half right" pattern
    // High when two distinct groups moving in opposite directions
    double computeVelocityBimodality(std::vector<double> const& vx2s,
                                     std::vector<double> const& vy2s) const;

    // Angular momentum spread: circular spread of L = r × p for each pendulum
    // High when angular momenta point in different directions
    double computeAngularMomentumSpread(std::vector<double> const& th1s,
                                        std::vector<double> const& th2s,
                                        std::vector<double> const& w1s,
                                        std::vector<double> const& w2s,
                                        double L1, double L2,
                                        double M1, double M2) const;

    // Acceleration dispersion: how spread out are tip accelerations?
    // Computed from angular accelerations (derived from ω changes)
    double computeAccelerationDispersion(std::vector<double> const& th1s,
                                         std::vector<double> const& th2s,
                                         std::vector<double> const& w1s,
                                         std::vector<double> const& w2s,
                                         double L1, double L2, double G) const;

};

// Standard metric names (use these constants for consistency)
namespace MetricNames {
// Physics metrics (angle-based)
constexpr const char* Variance = "variance";
constexpr const char* SpreadRatio = "spread_ratio";
constexpr const char* CircularSpread = "circular_spread";
constexpr const char* AngularRange = "angular_range";
constexpr const char* TotalEnergy = "total_energy";
constexpr const char* AngularCausticness = "angular_causticness";

// Caustic metrics - per-arm causticness (coverage × gini on angle distribution)
// All use the same low→high→low pattern as angular_causticness
constexpr const char* R1 = "r1_concentration";  // First arm causticness (angle1 only)
constexpr const char* R2 = "r2_concentration";  // Second arm causticness (angle2 only)
constexpr const char* JointConcentration = "joint_concentration";  // R1 × R2

// Caustic metrics - position-based (use tip x2,y2 coordinates)
constexpr const char* TipCausticness = "tip_causticness";  // Causticness using atan2(x2,y2)
constexpr const char* SpatialConcentration = "spatial_concentration";  // 2D coverage × gini

// Alternative caustic metrics (experimental)
constexpr const char* CVCausticness = "cv_causticness";  // CV instead of Gini on sectors
constexpr const char* OrganizationCausticness = "organization_causticness";  // (1-R1*R2) × coverage
constexpr const char* FoldCausticness = "fold_causticness";  // Adjacent-pair distance CV × spread

// New paradigm metrics (local coherence based)
constexpr const char* TrajectorySmoothness = "trajectory_smoothness";  // Predictability of pos[i+1] from pos[i]
constexpr const char* Curvature = "curvature";  // Mean curvature of θ→(x,y) mapping
constexpr const char* TrueFolds = "true_folds";  // Count of actual trajectory crossings
constexpr const char* LocalCoherence = "local_coherence";  // Neighbor distance vs random distance

// Velocity-based metrics (for boom detection)
constexpr const char* VelocityDispersion = "velocity_dispersion";  // How spread out velocity directions are
constexpr const char* SpeedVariance = "speed_variance";            // Variance of tip speeds
constexpr const char* VelocityBimodality = "velocity_bimodality";  // Two groups going opposite directions
constexpr const char* AngularMomentumSpread = "angular_momentum_spread";  // Spread of angular momenta
constexpr const char* AccelerationDispersion = "acceleration_dispersion";  // How spread out accelerations are

// GPU metrics (simplified)
constexpr const char* MaxValue = "max_value";
constexpr const char* Brightness = "brightness";
constexpr const char* Coverage = "coverage";
} // namespace MetricNames

} // namespace metrics
