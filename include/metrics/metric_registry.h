#pragma once

// Centralized Metric Registry - Single Source of Truth
//
// This file defines all metrics and their metadata in one place.
// When adding a new metric:
//   1. Add entry to METRIC_REGISTRY array below
//   2. Add computation in MetricsCollector::updateFromAngles/States
//
// That's it! CSV export, GUI, and other tools use this registry automatically.

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace metrics {

// ============================================================================
// METRIC CLASSIFICATION ENUMS
// ============================================================================

// Where the metric is computed
enum class MetricSource {
    Physics,  // Computed from pendulum state (angles, positions, velocities)
    GPU       // Computed from rendered frame (brightness, coverage)
};

// Logical grouping for UI organization
enum class MetricCategory {
    Basic,           // variance, spread_ratio, circular_spread, angular_range
    Caustic,         // angular_causticness, tip_causticness, cv, etc.
    LocalCoherence,  // trajectory_smoothness, curvature, true_folds, local_coherence
    Velocity,        // velocity_dispersion, velocity_bimodality, etc.
    GPU,             // brightness, coverage, max_value
    Other            // total_energy
};

// Axis assignment for multi-axis plots
enum class PlotAxis {
    Y1_Large,      // Large scale values (variance, energy)
    Y2_Normalized, // 0-1 range (most metrics)
    Y3_Medium      // Medium scale (spatial_concentration)
};

// Parameter type for metrics with configurable parameters
// Maps to param structs in config.h (SectorMetricParams, etc.)
enum class ParamType {
    None,            // No configurable params
    Sector,          // SectorMetricParams (min_sectors, max_sectors, target_per_sector)
    CVSector,        // CVSectorMetricParams (adds cv_normalization)
    Grid,            // GridMetricParams (min_grid, max_grid, target_per_cell)
    Fold,            // FoldMetricParams (max_radius, cv_normalization)
    Trajectory,      // TrajectoryMetricParams
    Curvature,       // CurvatureMetricParams
    TrueFolds,       // TrueFoldsMetricParams
    LocalCoherence   // LocalCoherenceMetricParams
};

// ============================================================================
// METRIC COLOR (for GUI plotting)
// ============================================================================

struct MetricColor {
    float r, g, b, a;

    // Derivative uses same color with lower alpha
    constexpr MetricColor deriv() const {
        return {r, g, b, 0.4f};
    }
};

// ============================================================================
// METRIC DEFINITION
// ============================================================================

struct MetricDef {
    const char* name;           // Full identifier: "angular_causticness"
    const char* short_name;     // Display name: "Angular"
    const char* description;    // Human-readable description

    MetricSource source;
    MetricCategory category;
    PlotAxis axis;
    ParamType param_type;
    MetricColor color;

    bool default_enabled;       // Shown by default in GUI
    bool supports_boom;         // Can be used for boom detection
    int csv_order;              // Position in CSV output (0 = not in CSV)

    // Comparison for compile-time lookup
    constexpr bool matches(std::string_view other) const {
        return std::string_view(name) == other;
    }
};

// ============================================================================
// THE REGISTRY - Single Source of Truth for All Metrics
// ============================================================================

inline constexpr std::array<MetricDef, 26> METRIC_REGISTRY = {{
    // === BASIC STATISTICS ===
    {"variance", "Var", "Variance of angle2 distribution",
     MetricSource::Physics, MetricCategory::Basic, PlotAxis::Y1_Large,
     ParamType::None, {0.4f, 0.8f, 0.4f, 1.0f}, true, true, 1},

    {"circular_spread", "Spread", "1 - mean resultant length (uniformity)",
     MetricSource::Physics, MetricCategory::Basic, PlotAxis::Y2_Normalized,
     ParamType::None, {1.0f, 0.6f, 0.4f, 1.0f}, false, false, 2},

    {"spread_ratio", "SprdR", "Fraction of pendulums above horizontal",
     MetricSource::Physics, MetricCategory::Basic, PlotAxis::Y2_Normalized,
     ParamType::None, {0.9f, 0.5f, 0.3f, 1.0f}, false, false, 3},

    {"angular_range", "Range", "Normalized angular coverage",
     MetricSource::Physics, MetricCategory::Basic, PlotAxis::Y2_Normalized,
     ParamType::None, {0.7f, 0.5f, 0.3f, 1.0f}, false, false, 4},

    // === CAUSTIC METRICS ===
    {"angular_causticness", "Angular", "Causticness from angle distribution",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y2_Normalized,
     ParamType::Sector, {0.2f, 1.0f, 0.6f, 1.0f}, true, true, 5},

    {"r1_concentration", "R1", "First arm concentration",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y2_Normalized,
     ParamType::Sector, {0.8f, 0.4f, 1.0f, 1.0f}, false, true, 6},

    {"r2_concentration", "R2", "Second arm concentration",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y2_Normalized,
     ParamType::Sector, {1.0f, 0.4f, 0.8f, 1.0f}, false, true, 7},

    {"joint_concentration", "Joint", "R1 * R2 combined concentration",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y2_Normalized,
     ParamType::Sector, {0.4f, 0.8f, 1.0f, 1.0f}, false, true, 8},

    {"tip_causticness", "Tip", "Causticness using atan2(x2, y2)",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y2_Normalized,
     ParamType::Sector, {0.6f, 1.0f, 0.4f, 1.0f}, false, true, 9},

    {"spatial_concentration", "Spatial", "2D coverage x gini on tip positions",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y3_Medium,
     ParamType::Grid, {1.0f, 0.6f, 0.6f, 1.0f}, false, true, 10},

    {"cv_causticness", "CV", "CV-based causticness (coefficient of variation)",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y2_Normalized,
     ParamType::CVSector, {1.0f, 0.5f, 0.0f, 1.0f}, false, true, 11},

    {"organization_causticness", "Org", "(1-R1*R2) x coverage organization",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y2_Normalized,
     ParamType::Sector, {0.5f, 1.0f, 1.0f, 1.0f}, false, true, 12},

    {"fold_causticness", "Fold", "Adjacent-pair distance CV x spread",
     MetricSource::Physics, MetricCategory::Caustic, PlotAxis::Y2_Normalized,
     ParamType::Fold, {1.0f, 1.0f, 0.3f, 1.0f}, false, true, 13},

    // === LOCAL COHERENCE METRICS ===
    {"trajectory_smoothness", "Traj", "Predictability of pos[i+1] from pos[i]",
     MetricSource::Physics, MetricCategory::LocalCoherence, PlotAxis::Y2_Normalized,
     ParamType::Trajectory, {0.3f, 0.9f, 0.3f, 1.0f}, false, true, 14},

    {"curvature", "Curve", "Mean curvature of theta->xy mapping",
     MetricSource::Physics, MetricCategory::LocalCoherence, PlotAxis::Y2_Normalized,
     ParamType::Curvature, {0.9f, 0.3f, 0.9f, 1.0f}, false, true, 15},

    {"true_folds", "Folds", "Count of trajectory crossings",
     MetricSource::Physics, MetricCategory::LocalCoherence, PlotAxis::Y2_Normalized,
     ParamType::TrueFolds, {1.0f, 0.6f, 0.0f, 1.0f}, false, true, 16},

    {"local_coherence", "Local", "Index-neighbors vs spatial-neighbors correlation",
     MetricSource::Physics, MetricCategory::LocalCoherence, PlotAxis::Y2_Normalized,
     ParamType::LocalCoherence, {0.3f, 0.7f, 1.0f, 1.0f}, false, true, 17},

    // === VELOCITY-BASED METRICS ===
    {"velocity_dispersion", "VelDisp", "Velocity direction spread (circular stats)",
     MetricSource::Physics, MetricCategory::Velocity, PlotAxis::Y2_Normalized,
     ParamType::None, {1.0f, 0.3f, 0.3f, 1.0f}, false, true, 18},

    {"speed_variance", "SpdVar", "Normalized variance of tip speeds",
     MetricSource::Physics, MetricCategory::Velocity, PlotAxis::Y2_Normalized,
     ParamType::None, {0.8f, 0.5f, 0.2f, 1.0f}, false, true, 19},

    {"velocity_bimodality", "VelBimod", "Half left / half right pattern detection",
     MetricSource::Physics, MetricCategory::Velocity, PlotAxis::Y2_Normalized,
     ParamType::None, {1.0f, 0.8f, 0.2f, 1.0f}, false, true, 20},

    {"angular_momentum_spread", "AngMom", "Spread of angular momenta directions",
     MetricSource::Physics, MetricCategory::Velocity, PlotAxis::Y2_Normalized,
     ParamType::None, {0.6f, 0.2f, 0.8f, 1.0f}, false, true, 21},

    {"acceleration_dispersion", "AccelDisp", "Tip acceleration direction spread",
     MetricSource::Physics, MetricCategory::Velocity, PlotAxis::Y2_Normalized,
     ParamType::None, {0.2f, 0.8f, 0.6f, 1.0f}, false, true, 22},

    // === GPU METRICS ===
    {"brightness", "Bright", "Mean pixel intensity (0-1)",
     MetricSource::GPU, MetricCategory::GPU, PlotAxis::Y2_Normalized,
     ParamType::None, {0.8f, 0.8f, 0.4f, 1.0f}, false, false, 23},

    {"coverage", "Cover", "Fraction of non-zero pixels",
     MetricSource::GPU, MetricCategory::GPU, PlotAxis::Y2_Normalized,
     ParamType::None, {1.0f, 0.8f, 0.4f, 1.0f}, false, false, 24},

    {"max_value", "MaxVal", "Peak pixel intensity (before post-processing)",
     MetricSource::GPU, MetricCategory::GPU, PlotAxis::Y1_Large,
     ParamType::None, {0.6f, 0.6f, 0.9f, 1.0f}, false, false, 0},  // Not in CSV

    // === OTHER ===
    {"total_energy", "Energy", "Mean total energy per pendulum",
     MetricSource::Physics, MetricCategory::Other, PlotAxis::Y1_Large,
     ParamType::None, {0.4f, 0.6f, 1.0f, 1.0f}, false, false, 25},
}};

// Total metric count
inline constexpr size_t METRIC_COUNT = METRIC_REGISTRY.size();

// ============================================================================
// COMPILE-TIME LOOKUP HELPERS
// ============================================================================

// Find metric by name (returns nullptr if not found)
constexpr MetricDef const* findMetric(std::string_view name) {
    for (auto const& m : METRIC_REGISTRY) {
        if (m.matches(name)) return &m;
    }
    return nullptr;
}

// Get short name for a metric (returns name if not found)
inline std::string_view getShortName(std::string_view name) {
    if (auto const* m = findMetric(name)) {
        return m->short_name;
    }
    return name;
}

// ============================================================================
// RUNTIME QUERY HELPERS
// ============================================================================

// Get CSV columns in canonical order (sorted by csv_order, excludes 0)
inline std::vector<const char*> getCSVColumns() {
    std::vector<std::pair<int, const char*>> ordered;
    for (auto const& m : METRIC_REGISTRY) {
        if (m.csv_order > 0) {
            ordered.emplace_back(m.csv_order, m.name);
        }
    }
    std::sort(ordered.begin(), ordered.end());

    std::vector<const char*> result;
    result.reserve(ordered.size());
    for (auto const& [order, name] : ordered) {
        result.push_back(name);
    }
    return result;
}

// Get all metrics matching a predicate
template<typename Pred>
std::vector<MetricDef const*> filterMetrics(Pred pred) {
    std::vector<MetricDef const*> result;
    for (auto const& m : METRIC_REGISTRY) {
        if (pred(m)) result.push_back(&m);
    }
    return result;
}

// Common filters
inline std::vector<MetricDef const*> getPhysicsMetrics() {
    return filterMetrics([](auto const& m) { return m.source == MetricSource::Physics; });
}

inline std::vector<MetricDef const*> getGPUMetrics() {
    return filterMetrics([](auto const& m) { return m.source == MetricSource::GPU; });
}

inline std::vector<MetricDef const*> getBoomMetrics() {
    return filterMetrics([](auto const& m) { return m.supports_boom; });
}

inline std::vector<MetricDef const*> getDefaultEnabledMetrics() {
    return filterMetrics([](auto const& m) { return m.default_enabled; });
}

inline std::vector<MetricDef const*> getByCategory(MetricCategory cat) {
    return filterMetrics([cat](auto const& m) { return m.category == cat; });
}

// Category name for display
inline const char* categoryName(MetricCategory cat) {
    switch (cat) {
        case MetricCategory::Basic: return "Basic Statistics";
        case MetricCategory::Caustic: return "Causticness";
        case MetricCategory::LocalCoherence: return "Local Coherence";
        case MetricCategory::Velocity: return "Velocity";
        case MetricCategory::GPU: return "GPU";
        case MetricCategory::Other: return "Other";
    }
    return "Unknown";
}

} // namespace metrics

// Note: MetricNames namespace is defined in metrics_collector.h for backward compatibility.
// The registry uses the same string literals, so they are interchangeable.
