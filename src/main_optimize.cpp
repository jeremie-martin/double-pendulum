// Metric optimization tool for double pendulum visualization
//
// Performs grid search over metric parameters to find optimal settings
// for boom and peak detection based on annotated ground truth data.
//
// Usage:
//   ./pendulum-optimize annotations.json [simulation_data.bin ...]
//
// Annotation format (JSON):
// {
//   "version": 1,
//   "annotations": [
//     {
//       "id": "run_20241215_143022",
//       "data_path": "output/run_20241215_143022/simulation_data.bin",
//       "boom_frame": 180,
//       "peak_frame": 245,
//       "notes": "Clean boom, multiple folds at peak"
//     }
//   ]
// }

#include "config.h"
#include "metrics/boom_detection.h"
#include "metrics/metrics_collector.h"
#include "metrics/metrics_init.h"
#include "pendulum.h"
#include "simulation_data.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Annotation entry
struct Annotation {
    std::string id;
    std::string data_path;
    int boom_frame = -1; // Ground truth boom frame (-1 = unknown)
    int peak_frame = -1; // Ground truth peak causticness frame (-1 = unknown)
    std::string notes;
};

// Parameter set being tested
struct ParameterSet {
    ::MetricConfig metric_config;  // Per-metric config (using global MetricConfig, not local one)
    BoomDetectionParams boom;
    int effective_sectors = 0; // Computed for actual N

    // Short description for tables (boom detection only)
    std::string describeShort() const {
        std::ostringstream oss;
        // Shorten metric name for display
        std::string metric_short = boom.metric_name;
        if (metric_short.find("_causticness") != std::string::npos) {
            metric_short = metric_short.substr(0, metric_short.find("_causticness"));
        } else if (metric_short.find("_concentration") != std::string::npos) {
            metric_short = metric_short.substr(0, metric_short.find("_concentration")) + "_conc";
        } else if (metric_short.find("_coherence") != std::string::npos) {
            metric_short = metric_short.substr(0, metric_short.find("_coherence")) + "_coh";
        }
        oss << metric_short << " ";
        switch (boom.method) {
        case BoomDetectionMethod::MaxCausticness:
            oss << "max";
            break;
        case BoomDetectionMethod::FirstPeakPercent:
            oss << "first@" << (int)(boom.peak_percent_threshold * 100) << "%";
            oss << " prom=" << std::fixed << std::setprecision(2) << boom.min_peak_prominence;
            break;
        case BoomDetectionMethod::DerivativePeak:
            oss << "deriv w=" << boom.smoothing_window;
            break;
        case BoomDetectionMethod::ThresholdCrossing:
            oss << "cross@" << (int)(boom.crossing_threshold * 100) << "% x"
                << boom.crossing_confirmation;
            break;
        case BoomDetectionMethod::SecondDerivativePeak:
            oss << "accel w=" << boom.smoothing_window;
            break;
        }
        // Always show offset since it's applied to all methods
        oss << " off=" << std::fixed << std::setprecision(1) << boom.offset_seconds;
        return oss.str();
    }

    // Full description including metric params and effective sectors
    std::string describeFull() const {
        std::ostringstream oss;
        oss << describeShort();
        if (effective_sectors > 0) {
            oss << " [eff_sec=" << effective_sectors << "]";
        }
        return oss.str();
    }

    // Legacy alias
    std::string describe() const { return describeShort(); }
};

// Evaluation results for a parameter set
struct EvaluationResult {
    ParameterSet params;
    double boom_mae = 0.0;       // Mean absolute error for boom detection (frames)
    double boom_stddev = 0.0;    // Standard deviation of errors
    double boom_median = 0.0;    // Median absolute error
    double boom_max = 0.0;       // Maximum absolute error
    double peak_mae = 0.0;       // Mean absolute error for peak detection (frames)
    double combined_score = 0.0; // Combined score (lower is better)
    int samples_evaluated = 0;
    std::vector<int> per_sim_errors; // Error for each simulation (for analysis)
};

// Simple JSON value extraction helpers
namespace {

std::string extractString(std::string const& json, std::string const& key) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return match[1].str();
    }
    return "";
}

int extractInt(std::string const& json, std::string const& key, int default_val = -1) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch match;
    if (std::regex_search(json, match, pattern)) {
        return std::stoi(match[1].str());
    }
    return default_val;
}

} // namespace

// Load annotations from JSON file
std::vector<Annotation> loadAnnotations(std::string const& path) {
    std::vector<Annotation> annotations;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open annotations file: " << path << "\n";
        return annotations;
    }

    // Read entire file
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Find annotations array and parse each object
    std::regex obj_pattern("\\{[^{}]*\"id\"[^{}]*\\}");
    auto obj_begin = std::sregex_iterator(content.begin(), content.end(), obj_pattern);
    auto obj_end = std::sregex_iterator();

    for (auto it = obj_begin; it != obj_end; ++it) {
        std::string obj = it->str();
        Annotation ann;
        ann.id = extractString(obj, "id");
        ann.data_path = extractString(obj, "data_path");
        ann.boom_frame = extractInt(obj, "boom_frame", -1);
        ann.peak_frame = extractInt(obj, "peak_frame", -1);
        ann.notes = extractString(obj, "notes");

        if (!ann.id.empty() || !ann.data_path.empty()) {
            annotations.push_back(ann);
        }
    }

    return annotations;
}

// Pre-loaded simulation data for fast parameter iteration
struct LoadedSimulation {
    std::string id;
    simulation_data::Reader reader;
    double frame_duration = 0.0;
    int boom_frame_truth = -1;
    int peak_frame_truth = -1;

    bool load(Annotation const& ann) {
        if (!reader.open(ann.data_path)) {
            return false;
        }
        id = ann.id;
        boom_frame_truth = ann.boom_frame;
        peak_frame_truth = ann.peak_frame;
        auto const& h = reader.header();
        frame_duration = h.duration_seconds / h.frame_count;
        return true;
    }
};

// Pre-computed metrics for a simulation (computed once, used many times)
struct ComputedMetrics {
    std::string sim_id;
    double frame_duration = 0.0;
    int boom_frame_truth = -1;
    metrics::MetricsCollector collector;
};

// Phase 1: Compute all metrics for a simulation (expensive, done once per metric config)
void computeMetrics(LoadedSimulation const& sim,
                    std::unordered_map<std::string, ::MetricConfig> const& metric_configs,
                    ComputedMetrics& out) {
    auto const& header = sim.reader.header();
    int frame_count = header.frame_count;

    out.sim_id = sim.id;
    out.frame_duration = sim.frame_duration;
    out.boom_frame_truth = sim.boom_frame_truth;

    // Create metrics collector with our per-metric parameters
    out.collector.setAllMetricConfigs(metric_configs);
    out.collector.registerStandardMetrics();

    // Process all frames using zero-copy packed state access
    for (int frame = 0; frame < frame_count; ++frame) {
        auto const* packed = sim.reader.getFramePacked(frame);
        if (!packed)
            break;
        out.collector.beginFrame(frame);
        out.collector.updateFromPackedStates(packed, header.pendulum_count);
        out.collector.endFrame();
    }
}

// Phase 2: Evaluate boom detection on pre-computed metrics (cheap, done many times)
metrics::BoomDetection evaluateBoomDetection(ComputedMetrics const& computed,
                                             BoomDetectionParams const& boom_params) {
    return metrics::findBoomFrame(computed.collector, computed.frame_duration, boom_params);
}

// Compute effective sector count for sector-based metrics
int computeEffectiveSectors(int pendulum_count, SectorMetricParams const& params) {
    return std::max(params.min_sectors,
                    std::min(params.max_sectors, pendulum_count / params.target_per_sector));
}

// Compute effective grid size for spatial metrics
int computeEffectiveGrid(int pendulum_count, GridMetricParams const& params) {
    return std::max(
        params.min_grid,
        std::min(params.max_grid, static_cast<int>(std::sqrt(static_cast<double>(pendulum_count) /
                                                             params.target_per_cell))));
}

// ============================================================================
// PARAMETERIZED METRIC SYSTEM
// Each metric has specific parameters that affect it. We test variations of
// those parameters independently for each metric type.
// ============================================================================

// A parameterized metric: metric name + its specific parameter values (using global MetricConfig)
struct ParameterizedMetric {
    std::string metric_name;
    ::MetricConfig config;  // Uses the global MetricConfig type

    // Human-readable description showing only relevant params
    std::string describe(int N = 0) const {
        std::ostringstream oss;
        std::string short_name = metric_name;
        if (short_name.find("_causticness") != std::string::npos) {
            short_name = short_name.substr(0, short_name.find("_causticness"));
        } else if (short_name.find("_concentration") != std::string::npos) {
            short_name = short_name.substr(0, short_name.find("_concentration")) + "_conc";
        } else if (short_name.find("_coherence") != std::string::npos) {
            short_name = short_name.substr(0, short_name.find("_coherence")) + "_coh";
        } else if (short_name.find("_smoothness") != std::string::npos) {
            short_name = short_name.substr(0, short_name.find("_smoothness")) + "_smooth";
        }
        oss << short_name;

        // Show relevant params based on metric type using std::visit
        std::visit([&](auto const& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, SectorMetricParams>) {
                int eff = N > 0 ? computeEffectiveSectors(N, p) : p.max_sectors;
                oss << " sec=" << eff;
            } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
                SectorMetricParams sp{p.min_sectors, p.max_sectors, p.target_per_sector, {}};
                int eff = N > 0 ? computeEffectiveSectors(N, sp) : p.max_sectors;
                oss << " sec=" << eff << " cvn=" << std::fixed << std::setprecision(2)
                    << p.cv_normalization;
            } else if constexpr (std::is_same_v<T, GridMetricParams>) {
                int eff = N > 0 ? computeEffectiveGrid(N, p) : p.max_grid;
                oss << " grid=" << eff;
            } else if constexpr (std::is_same_v<T, FoldMetricParams>) {
                oss << " rad=" << std::fixed << std::setprecision(1) << p.max_radius
                    << " cvn=" << std::setprecision(2) << p.cv_normalization;
            } else if constexpr (std::is_same_v<T, TrajectoryMetricParams>) {
                oss << " rad=" << std::fixed << std::setprecision(1) << p.max_radius
                    << " spr=" << std::setprecision(2) << p.min_spread_threshold;
            } else if constexpr (std::is_same_v<T, CurvatureMetricParams>) {
                oss << " rad=" << std::fixed << std::setprecision(1) << p.max_radius
                    << " lrn=" << std::setprecision(1) << p.log_ratio_normalization;
            } else if constexpr (std::is_same_v<T, TrueFoldsMetricParams>) {
                oss << " gini=" << std::fixed << std::setprecision(2) << p.gini_chaos_baseline
                    << "/" << p.gini_baseline_divisor;
            } else if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
                oss << " log=" << std::fixed << std::setprecision(1) << p.log_inverse_baseline
                    << "/" << p.log_inverse_divisor;
            }
        }, config.params);
        return oss.str();
    }

    // Unique key for deduplication (params that actually matter for this metric)
    std::string key(int N) const {
        std::ostringstream oss;
        oss << metric_name;

        std::visit([&](auto const& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, SectorMetricParams>) {
                oss << "_sec" << computeEffectiveSectors(N, p);
            } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
                SectorMetricParams sp{p.min_sectors, p.max_sectors, p.target_per_sector, {}};
                oss << "_sec" << computeEffectiveSectors(N, sp) << "_cvn"
                    << static_cast<int>(p.cv_normalization * 100);
            } else if constexpr (std::is_same_v<T, GridMetricParams>) {
                oss << "_grid" << computeEffectiveGrid(N, p);
            } else if constexpr (std::is_same_v<T, FoldMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_cvn"
                    << static_cast<int>(p.cv_normalization * 100);
            } else if constexpr (std::is_same_v<T, TrajectoryMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_spr"
                    << static_cast<int>(p.min_spread_threshold * 1000);
            } else if constexpr (std::is_same_v<T, CurvatureMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_spr"
                    << static_cast<int>(p.min_spread_threshold * 1000) << "_lrn"
                    << static_cast<int>(p.log_ratio_normalization * 10);
            } else if constexpr (std::is_same_v<T, TrueFoldsMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_spr"
                    << static_cast<int>(p.min_spread_threshold * 1000) << "_gb"
                    << static_cast<int>(p.gini_chaos_baseline * 100) << "_gd"
                    << static_cast<int>(p.gini_baseline_divisor * 100);
            } else if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_spr"
                    << static_cast<int>(p.min_spread_threshold * 1000) << "_lb"
                    << static_cast<int>(p.log_inverse_baseline * 10) << "_ld"
                    << static_cast<int>(p.log_inverse_divisor * 10);
            }
        }, config.params);
        return oss.str();
    }
};

// Helper to create SectorMetricParams with effective sector count
SectorMetricParams makeSectorParams(int eff_sec, int N) {
    SectorMetricParams p;
    p.max_sectors = eff_sec;
    p.min_sectors = std::min(8, eff_sec);
    p.target_per_sector = std::max(1, N / (eff_sec * 2));
    return p;
}

// Helper to create CVSectorMetricParams with effective sector count and cv_normalization
CVSectorMetricParams makeCVSectorParams(int eff_sec, double cv_norm, int N) {
    CVSectorMetricParams p;
    p.max_sectors = eff_sec;
    p.min_sectors = std::min(8, eff_sec);
    p.target_per_sector = std::max(1, N / (eff_sec * 2));
    p.cv_normalization = cv_norm;
    return p;
}

// Helper to create GridMetricParams with effective grid size
GridMetricParams makeGridParams(int eff_grid, int N) {
    GridMetricParams p;
    p.max_grid = eff_grid;
    p.min_grid = std::min(4, eff_grid);
    p.target_per_cell = std::max(1, N / (eff_grid * eff_grid * 2));
    return p;
}

// Helper to create a ::MetricConfig with the appropriate variant type
::MetricConfig makeMetricConfig(std::string const& name, MetricParamsVariant const& params) {
    ::MetricConfig config;
    config.name = name;
    config.params = params;
    return config;
}

// Generate unique key for a metric config (for deduplication and grouping)
std::string metricConfigKey(std::string const& metric_name, ::MetricConfig const& config, int N) {
    std::ostringstream oss;
    oss << metric_name;

    std::visit([&](auto const& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, SectorMetricParams>) {
            oss << "_sec" << computeEffectiveSectors(N, p);
        } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
            SectorMetricParams sp{p.min_sectors, p.max_sectors, p.target_per_sector, {}};
            oss << "_sec" << computeEffectiveSectors(N, sp) << "_cvn"
                << static_cast<int>(p.cv_normalization * 100);
        } else if constexpr (std::is_same_v<T, GridMetricParams>) {
            oss << "_grid" << computeEffectiveGrid(N, p);
        } else if constexpr (std::is_same_v<T, FoldMetricParams>) {
            oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_cvn"
                << static_cast<int>(p.cv_normalization * 100);
        } else if constexpr (std::is_same_v<T, TrajectoryMetricParams>) {
            oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_spr"
                << static_cast<int>(p.min_spread_threshold * 1000);
        } else if constexpr (std::is_same_v<T, CurvatureMetricParams>) {
            oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_spr"
                << static_cast<int>(p.min_spread_threshold * 1000) << "_lrn"
                << static_cast<int>(p.log_ratio_normalization * 10);
        } else if constexpr (std::is_same_v<T, TrueFoldsMetricParams>) {
            oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_spr"
                << static_cast<int>(p.min_spread_threshold * 1000) << "_gb"
                << static_cast<int>(p.gini_chaos_baseline * 100) << "_gd"
                << static_cast<int>(p.gini_baseline_divisor * 100);
        } else if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
            oss << "_rad" << static_cast<int>(p.max_radius * 10) << "_spr"
                << static_cast<int>(p.min_spread_threshold * 1000) << "_lb"
                << static_cast<int>(p.log_inverse_baseline * 10) << "_ld"
                << static_cast<int>(p.log_inverse_divisor * 10);
        }
    }, config.params);
    return oss.str();
}

// Generate all parameterized metrics for comprehensive optimization
std::vector<ParameterizedMetric> generateParameterizedMetrics(int N) {
    std::vector<ParameterizedMetric> result;

    // Parameter value ranges for each type
    std::vector<int> sector_values = {8, 16, 24, 32, 48, 64, 80, 96};
    std::vector<int> grid_values = {4, 8, 12, 16, 24, 32};
    std::vector<double> cv_norm_values = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5};
    std::vector<double> max_radius_values = {1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5};
    std::vector<double> min_spread_values = {0.02, 0.04, 0.06, 0.08, 0.1};
    std::vector<double> log_ratio_norm_values = {1.0, 1.5, 2.0, 2.5};
    std::vector<double> gini_baseline_values = {0.1, 0.20, 0.30, 0.40, 0.50};
    std::vector<double> gini_divisor_values = {0.5, 0.60, 0.70, 0.80};
    std::vector<double> log_inv_baseline_values = {0.5, 1.0, 1.5};
    std::vector<double> log_inv_divisor_values = {1.5, 2.0, 2.5, 3.0};

    // ================================================================
    // SECTOR-BASED METRICS (vary effective_sectors only)
    // These metrics use SectorMetricParams
    // ================================================================
    std::vector<std::string> sector_metrics = {
        "angular_causticness", "tip_causticness", "organization_causticness",
        "r1_concentration", "r2_concentration", "joint_concentration"
    };
    for (int eff_sec : sector_values) {
        if (eff_sec > N / 2)
            continue;
        SectorMetricParams params = makeSectorParams(eff_sec, N);
        for (auto const& metric_name : sector_metrics) {
            result.push_back({metric_name, makeMetricConfig(metric_name, params)});
        }
    }

    // ================================================================
    // VARIANCE (no parameterization needed - use default SectorMetricParams)
    // ================================================================
    result.push_back({"variance", makeMetricConfig("variance", SectorMetricParams{})});

    // ================================================================
    // CV_CAUSTICNESS (vary effective_sectors × cv_normalization)
    // Uses CVSectorMetricParams
    // ================================================================
    for (int eff_sec : sector_values) {
        if (eff_sec > N / 2)
            continue;
        for (double cv_norm : cv_norm_values) {
            CVSectorMetricParams params = makeCVSectorParams(eff_sec, cv_norm, N);
            result.push_back({"cv_causticness", makeMetricConfig("cv_causticness", params)});
        }
    }

    // ================================================================
    // SPATIAL_CONCENTRATION (vary effective_grid)
    // Uses GridMetricParams
    // ================================================================
    for (int eff_grid : grid_values) {
        GridMetricParams params = makeGridParams(eff_grid, N);
        result.push_back({"spatial_concentration", makeMetricConfig("spatial_concentration", params)});
    }

    // ================================================================
    // FOLD_CAUSTICNESS (vary max_radius × cv_normalization)
    // Uses FoldMetricParams
    // ================================================================
    for (double max_rad : max_radius_values) {
        for (double cv_norm : cv_norm_values) {
            FoldMetricParams params;
            params.max_radius = max_rad;
            params.cv_normalization = cv_norm;
            result.push_back({"fold_causticness", makeMetricConfig("fold_causticness", params)});
        }
    }

    // ================================================================
    // TRAJECTORY_SMOOTHNESS (vary max_radius × min_spread_threshold)
    // Uses TrajectoryMetricParams
    // ================================================================
    for (double max_rad : max_radius_values) {
        for (double min_spr : min_spread_values) {
            TrajectoryMetricParams params;
            params.max_radius = max_rad;
            params.min_spread_threshold = min_spr;
            result.push_back({"trajectory_smoothness", makeMetricConfig("trajectory_smoothness", params)});
        }
    }

    // ================================================================
    // CURVATURE (vary max_radius × min_spread × log_ratio_normalization)
    // Uses CurvatureMetricParams
    // ================================================================
    for (double max_rad : max_radius_values) {
        for (double min_spr : min_spread_values) {
            for (double log_ratio : log_ratio_norm_values) {
                CurvatureMetricParams params;
                params.max_radius = max_rad;
                params.min_spread_threshold = min_spr;
                params.log_ratio_normalization = log_ratio;
                result.push_back({"curvature", makeMetricConfig("curvature", params)});
            }
        }
    }

    // ================================================================
    // TRUE_FOLDS (vary max_radius × min_spread × gini params)
    // Uses TrueFoldsMetricParams
    // ================================================================
    for (double max_rad : max_radius_values) {
        for (double min_spr : min_spread_values) {
            for (double gini_base : gini_baseline_values) {
                for (double gini_div : gini_divisor_values) {
                    TrueFoldsMetricParams params;
                    params.max_radius = max_rad;
                    params.min_spread_threshold = min_spr;
                    params.gini_chaos_baseline = gini_base;
                    params.gini_baseline_divisor = gini_div;
                    result.push_back({"true_folds", makeMetricConfig("true_folds", params)});
                }
            }
        }
    }

    // ================================================================
    // LOCAL_COHERENCE (vary max_radius × min_spread × log_inverse params)
    // Uses LocalCoherenceMetricParams
    // ================================================================
    for (double max_rad : max_radius_values) {
        for (double min_spr : min_spread_values) {
            for (double log_base : log_inv_baseline_values) {
                for (double log_div : log_inv_divisor_values) {
                    LocalCoherenceMetricParams params;
                    params.max_radius = max_rad;
                    params.min_spread_threshold = min_spr;
                    params.log_inverse_baseline = log_base;
                    params.log_inverse_divisor = log_div;
                    result.push_back({"local_coherence", makeMetricConfig("local_coherence", params)});
                }
            }
        }
    }

    return result;
}

// Deduplicate parameterized metrics by their key
std::vector<ParameterizedMetric>
deduplicateParameterizedMetrics(std::vector<ParameterizedMetric> const& metrics, int N) {
    std::map<std::string, ParameterizedMetric> unique;
    for (auto const& pm : metrics) {
        std::string k = pm.key(N);
        if (unique.find(k) == unique.end()) {
            unique[k] = pm;
        }
    }
    std::vector<ParameterizedMetric> result;
    for (auto const& [k, pm] : unique) {
        result.push_back(pm);
    }
    return result;
}

// Generate unique key for a metric config map (for Phase 1 grouping)
std::string metricConfigMapKey(std::unordered_map<std::string, ::MetricConfig> const& configs, int N) {
    std::ostringstream oss;
    // Sort keys for deterministic ordering
    std::vector<std::string> keys;
    for (auto const& [name, config] : configs) {
        keys.push_back(name);
    }
    std::sort(keys.begin(), keys.end());

    for (auto const& name : keys) {
        auto const& config = configs.at(name);
        oss << metricConfigKey(name, config, N) << ";";
    }
    return oss.str();
}

// Extract unique metric config maps from parameterized metrics (for Phase 1 computation grouping)
// Returns a map from unique key -> metric config map
std::map<std::string, std::unordered_map<std::string, ::MetricConfig>>
extractUniqueMetricConfigMaps(std::vector<ParameterizedMetric> const& metrics, int N) {
    std::map<std::string, std::unordered_map<std::string, ::MetricConfig>> result;

    for (auto const& pm : metrics) {
        // Create a metric config map with just this metric's config
        std::unordered_map<std::string, ::MetricConfig> config_map;
        config_map[pm.metric_name] = pm.config;

        std::string key = metricConfigKey(pm.metric_name, pm.config, N);
        if (result.find(key) == result.end()) {
            result[key] = config_map;
        }
    }
    return result;
}

// Save best parameters to TOML file (using new per-metric format)
void saveBestParams(std::string const& path, EvaluationResult const& best) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not write to " << path << "\n";
        return;
    }

    file << "# Best parameters found by pendulum-optimize\n";
    file << "# Boom MAE: " << std::fixed << std::setprecision(2) << best.boom_mae << " frames\n";
    file << "# Peak MAE: " << std::fixed << std::setprecision(2) << best.peak_mae << " frames\n";
    file << "# Samples evaluated: " << best.samples_evaluated << "\n\n";

    // Output the metric-specific configuration
    std::string const& metric_name = best.params.boom.metric_name;
    file << "[metrics." << metric_name << "]\n";

    // Output parameters based on variant type
    std::visit([&](auto const& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, SectorMetricParams>) {
            file << "min_sectors = " << p.min_sectors << "\n";
            file << "max_sectors = " << p.max_sectors << "\n";
            file << "target_per_sector = " << p.target_per_sector << "\n";
        } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
            file << "min_sectors = " << p.min_sectors << "\n";
            file << "max_sectors = " << p.max_sectors << "\n";
            file << "target_per_sector = " << p.target_per_sector << "\n";
            file << "cv_normalization = " << std::fixed << std::setprecision(2) << p.cv_normalization << "\n";
        } else if constexpr (std::is_same_v<T, GridMetricParams>) {
            file << "min_grid = " << p.min_grid << "\n";
            file << "max_grid = " << p.max_grid << "\n";
            file << "target_per_cell = " << p.target_per_cell << "\n";
        } else if constexpr (std::is_same_v<T, FoldMetricParams>) {
            file << "max_radius = " << std::fixed << std::setprecision(2) << p.max_radius << "\n";
            file << "cv_normalization = " << std::fixed << std::setprecision(2) << p.cv_normalization << "\n";
        } else if constexpr (std::is_same_v<T, TrajectoryMetricParams>) {
            file << "max_radius = " << std::fixed << std::setprecision(2) << p.max_radius << "\n";
            file << "min_spread_threshold = " << std::fixed << std::setprecision(3) << p.min_spread_threshold << "\n";
        } else if constexpr (std::is_same_v<T, CurvatureMetricParams>) {
            file << "max_radius = " << std::fixed << std::setprecision(2) << p.max_radius << "\n";
            file << "min_spread_threshold = " << std::fixed << std::setprecision(3) << p.min_spread_threshold << "\n";
            file << "log_ratio_normalization = " << std::fixed << std::setprecision(2) << p.log_ratio_normalization << "\n";
        } else if constexpr (std::is_same_v<T, TrueFoldsMetricParams>) {
            file << "max_radius = " << std::fixed << std::setprecision(2) << p.max_radius << "\n";
            file << "min_spread_threshold = " << std::fixed << std::setprecision(3) << p.min_spread_threshold << "\n";
            file << "gini_chaos_baseline = " << std::fixed << std::setprecision(2) << p.gini_chaos_baseline << "\n";
            file << "gini_baseline_divisor = " << std::fixed << std::setprecision(2) << p.gini_baseline_divisor << "\n";
        } else if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
            file << "max_radius = " << std::fixed << std::setprecision(2) << p.max_radius << "\n";
            file << "min_spread_threshold = " << std::fixed << std::setprecision(3) << p.min_spread_threshold << "\n";
            file << "log_inverse_baseline = " << std::fixed << std::setprecision(2) << p.log_inverse_baseline << "\n";
            file << "log_inverse_divisor = " << std::fixed << std::setprecision(2) << p.log_inverse_divisor << "\n";
        }
    }, best.params.metric_config.params);

    // Output boom detection parameters embedded in the metric section
    file << "\n[metrics." << metric_name << ".boom]\n";
    switch (best.params.boom.method) {
    case BoomDetectionMethod::MaxCausticness:
        file << "method = \"max_causticness\"\n";
        break;
    case BoomDetectionMethod::FirstPeakPercent:
        file << "method = \"first_peak_percent\"\n";
        break;
    case BoomDetectionMethod::DerivativePeak:
        file << "method = \"derivative_peak\"\n";
        break;
    case BoomDetectionMethod::ThresholdCrossing:
        file << "method = \"threshold_crossing\"\n";
        break;
    case BoomDetectionMethod::SecondDerivativePeak:
        file << "method = \"second_derivative_peak\"\n";
        break;
    }
    file << "offset_seconds = " << std::fixed << std::setprecision(2)
         << best.params.boom.offset_seconds << "\n";
    file << "peak_percent_threshold = " << std::fixed << std::setprecision(2)
         << best.params.boom.peak_percent_threshold << "\n";
    file << "min_peak_prominence = " << std::fixed << std::setprecision(2)
         << best.params.boom.min_peak_prominence << "\n";
    file << "smoothing_window = " << best.params.boom.smoothing_window << "\n";
    file << "crossing_threshold = " << std::fixed << std::setprecision(2)
         << best.params.boom.crossing_threshold << "\n";
    file << "crossing_confirmation = " << best.params.boom.crossing_confirmation << "\n";

    // Output global boom detection settings
    file << "\n[boom_detection]\n";
    file << "active_metric = \"" << metric_name << "\"\n";

    std::cout << "Best parameters saved to: " << path << "\n";
}

void printUsage(char const* prog) {
    std::cerr << "Usage: " << prog << " annotations.json [simulation_data.bin ...]\n\n"
              << "Performs grid search to find optimal metric parameters.\n\n"
              << "If simulation data files are provided on command line, they override\n"
              << "the paths in annotations.json.\n\n"
              << "Annotation JSON format:\n"
              << "{\n"
              << "  \"version\": 1,\n"
              << "  \"annotations\": [\n"
              << "    {\n"
              << "      \"id\": \"run_id\",\n"
              << "      \"data_path\": \"path/to/simulation_data.bin\",\n"
              << "      \"boom_frame\": 180,\n"
              << "      \"peak_frame\": 245\n"
              << "    }\n"
              << "  ]\n"
              << "}\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string annotations_path = argv[1];

    // Load annotations
    auto annotations = loadAnnotations(annotations_path);
    if (annotations.empty()) {
        std::cerr << "No valid annotations found.\n";
        return 1;
    }

    std::cout << "Loaded " << annotations.size() << " annotations\n";

    // Override paths if provided on command line
    if (argc > 2) {
        for (int i = 2; i < argc && i - 2 < static_cast<int>(annotations.size()); ++i) {
            annotations[i - 2].data_path = argv[i];
        }
    }

    // Validate all data files exist
    std::vector<Annotation> valid_annotations;
    for (auto const& ann : annotations) {
        if (std::filesystem::exists(ann.data_path)) {
            if (ann.boom_frame >= 0 || ann.peak_frame >= 0) {
                valid_annotations.push_back(ann);
            } else {
                std::cerr << "Skipping " << ann.id << ": no ground truth frames\n";
            }
        } else {
            std::cerr << "Skipping " << ann.id << ": file not found: " << ann.data_path << "\n";
        }
    }

    if (valid_annotations.empty()) {
        std::cerr << "No valid annotations with existing data files.\n";
        return 1;
    }

    std::cout << "Loading " << valid_annotations.size() << " simulations into memory...\n";

    // Pre-load all simulation data (load once, evaluate many times)
    auto load_start = std::chrono::steady_clock::now();
    std::vector<LoadedSimulation> simulations;
    simulations.reserve(valid_annotations.size());

    size_t total_frames = 0;
    size_t total_pendulums = 0;
    for (auto const& ann : valid_annotations) {
        auto t0 = std::chrono::steady_clock::now();
        LoadedSimulation sim;
        if (sim.load(ann)) {
            auto t1 = std::chrono::steady_clock::now();
            double load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            auto const& h = sim.reader.header();
            total_frames += h.frame_count;
            total_pendulums = h.pendulum_count; // Same for all
            std::cout << "  " << ann.id << ": " << h.frame_count << " frames, " << h.pendulum_count
                      << " pendulums, boom@" << ann.boom_frame << " (" << std::fixed
                      << std::setprecision(0) << load_ms << "ms)\n";
            simulations.push_back(std::move(sim));
        } else {
            std::cerr << "  FAILED: " << ann.data_path << "\n";
        }
    }

    auto load_end = std::chrono::steady_clock::now();
    double load_time = std::chrono::duration<double>(load_end - load_start).count();
    std::cout << "Loaded " << simulations.size() << " simulations in " << std::fixed
              << std::setprecision(2) << load_time << "s"
              << " (" << total_frames << " total frames, " << total_pendulums << " pendulums)\n\n";

    if (simulations.empty()) {
        std::cerr << "No simulations loaded successfully.\n";
        return 1;
    }

    int N = static_cast<int>(total_pendulums);

    // ============================================
    // GENERATE PARAMETERIZED METRICS
    // Each metric has its own set of relevant parameters.
    // We generate all variations for each metric type.
    // ============================================
    auto all_param_metrics = generateParameterizedMetrics(N);
    auto param_metrics = deduplicateParameterizedMetrics(all_param_metrics, N);

    std::cout << "Generated " << all_param_metrics.size() << " parameterized metrics ("
              << param_metrics.size() << " unique after dedup)\n";

    // Count by metric type
    std::map<std::string, int> metric_type_counts;
    for (auto const& pm : param_metrics) {
        metric_type_counts[pm.metric_name]++;
    }
    std::cout << "By metric type:\n";
    for (auto const& [name, count] : metric_type_counts) {
        std::cout << "  " << name << ": " << count << " variations\n";
    }
    std::cout << "\n";

    // Extract unique metric config maps for Phase 1 computation
    auto unique_config_maps = extractUniqueMetricConfigMaps(param_metrics, N);
    std::cout << "Unique metric configurations: " << unique_config_maps.size() << "\n\n";

    // Generate boom method configurations (used in Phase 2)
    std::vector<double> peak_pct_vals;
    for (double x = 0.3; x <= 0.90; x += 0.05) {
        peak_pct_vals.push_back(x);
    }
    std::vector<double> prominence_vals;
    for (double x = 0.01; x <= 0.4; x += 0.02) {
        prominence_vals.push_back(x);
    }

    std::vector<double> offset_vals;
    for (double x = -0.5; x <= 0.5; x += 0.025) {
        offset_vals.push_back(x);
    }
    std::vector<int> smooth_vals = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                    16, 17, 18, 19, 20, 22, 24, 26, 28, 30, 35, 40, 45, 50};
    // ThresholdCrossing parameters
    std::vector<double> crossing_thresh_vals = {0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5,
                                                 0.55, 0.6, 0.65, 0.7, 0.75, 0.8};
    std::vector<int> crossing_confirm_vals = {1, 2, 3, 4, 5, 7, 10};

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) {
        num_threads = 4;
    }

    // Boom method counts:
    // MaxCausticness: offset_vals.size()
    // FirstPeakPercent: peak_pct_vals.size() * offset_vals.size() * prominence_vals.size()
    // DerivativePeak: smooth_vals.size() * offset_vals.size()
    // ThresholdCrossing: crossing_thresh_vals.size() * crossing_confirm_vals.size() * offset_vals.size()
    // SecondDerivativePeak: smooth_vals.size() * offset_vals.size()
    size_t num_boom_methods = offset_vals.size() +
                              (peak_pct_vals.size() * offset_vals.size() * prominence_vals.size()) +
                              (smooth_vals.size() * offset_vals.size()) +
                              (crossing_thresh_vals.size() * crossing_confirm_vals.size() * offset_vals.size()) +
                              (smooth_vals.size() * offset_vals.size());
    size_t total_phase2_evals = param_metrics.size() * num_boom_methods;

    std::cout << "=== Parameterized Metrics Optimization ===\n";
    std::cout << "Phase 1: " << unique_config_maps.size() << " unique metric configs × "
              << simulations.size() << " simulations (expensive)\n";
    std::cout << "Phase 2: " << param_metrics.size() << " parameterized metrics × "
              << num_boom_methods << " boom methods = " << total_phase2_evals << " evaluations\n";
    std::cout << "Threads: " << num_threads << "\n\n";

    auto start_time = std::chrono::steady_clock::now();

    // ============================================
    // PHASE 1: Compute metrics for each unique metric config
    // ============================================
    size_t total_phase1_work = unique_config_maps.size() * simulations.size();
    std::cout << "Phase 1: Computing metrics (" << total_phase1_work << " work items)...\n";

    // Create index from config key to computed metrics
    // computed_by_config[config_key][sim_idx] = ComputedMetrics
    std::map<std::string, std::vector<ComputedMetrics>> computed_by_config;
    for (auto const& [key, config_map] : unique_config_maps) {
        computed_by_config[key].resize(simulations.size());
    }

    // Flatten work items: (config_key, sim_idx)
    std::vector<std::pair<std::string, size_t>> work_items;
    for (auto const& [key, config_map] : unique_config_maps) {
        for (size_t sim = 0; sim < simulations.size(); ++sim) {
            work_items.emplace_back(key, sim);
        }
    }

    std::atomic<size_t> work_idx{0};
    std::atomic<size_t> completed{0};
    std::atomic<bool> done{false};
    std::mutex print_mutex;

    // Progress thread
    std::thread progress_thread([&]() {
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            size_t c = completed.load();
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            double rate = c > 0 ? c / elapsed : 0;
            double eta = rate > 0 ? (total_phase1_work - c) / rate : 0;

            std::lock_guard<std::mutex> lock(print_mutex);
            std::cout << "\r  Progress: " << c << "/" << total_phase1_work << " (" << std::fixed
                      << std::setprecision(1) << (100.0 * c / total_phase1_work) << "%)"
                      << " | " << std::setprecision(1) << elapsed << "s"
                      << " | " << std::setprecision(0) << rate << " items/s"
                      << " | ETA: " << eta << "s     " << std::flush;
        }
    });

    // Worker threads
    std::vector<std::thread> workers;
    std::mutex computed_mutex;
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&]() {
            while (true) {
                size_t idx = work_idx.fetch_add(1);
                if (idx >= work_items.size()) {
                    break;
                }

                auto const& [key, sim] = work_items[idx];
                auto const& config_map = unique_config_maps.at(key);

                ComputedMetrics cm;
                computeMetrics(simulations[sim], config_map, cm);

                {
                    std::lock_guard<std::mutex> lock(computed_mutex);
                    computed_by_config[key][sim] = std::move(cm);
                }

                completed.fetch_add(1);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    done.store(true);
    progress_thread.join();

    auto phase1_done = std::chrono::steady_clock::now();
    double phase1_total = std::chrono::duration<double>(phase1_done - start_time).count();
    std::cout << "\rPhase 1 complete: " << total_phase1_work << " items in " << std::fixed
              << std::setprecision(2) << phase1_total << "s"
              << " (" << std::setprecision(0) << (total_phase1_work / phase1_total)
              << " items/s)                    \n\n";

    // ============================================
    // PHASE 2: Evaluate boom detection for each ParameterizedMetric
    // ============================================
    std::cout << "Phase 2: Evaluating " << total_phase2_evals
              << " (parameterized metric × boom method) combinations...\n";

    std::vector<EvaluationResult> results;
    results.reserve(total_phase2_evals);
    std::mutex results_mutex;

    // For each parameterized metric
    std::atomic<size_t> pm_idx{0};
    std::atomic<size_t> pm_completed{0};
    std::atomic<bool> phase2_done{false};

    // Progress thread for phase 2
    std::thread phase2_progress([&]() {
        while (!phase2_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            size_t c = pm_completed.load();
            std::lock_guard<std::mutex> lock(print_mutex);
            std::cout << "\r  Progress: " << c << "/" << param_metrics.size()
                      << " parameterized metrics evaluated     " << std::flush;
        }
    });

    workers.clear();
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&]() {
            while (true) {
                size_t idx = pm_idx.fetch_add(1);
                if (idx >= param_metrics.size()) {
                    break;
                }

                auto const& pm = param_metrics[idx];
                std::string config_key = metricConfigKey(pm.metric_name, pm.config, N);
                auto const& computed_for_config = computed_by_config.at(config_key);

                // Evaluate all boom methods for this parameterized metric
                auto evaluateBoomMethod = [&](BoomDetectionParams const& boom_params) {
                    std::vector<int> errors;
                    errors.reserve(computed_for_config.size());

                    for (auto const& computed : computed_for_config) {
                        auto boom = evaluateBoomDetection(computed, boom_params);

                        if (computed.boom_frame_truth >= 0 && boom.frame >= 0) {
                            int error = std::abs(boom.frame - computed.boom_frame_truth);
                            errors.push_back(error);
                        }
                    }

                    EvaluationResult result;
                    result.params.metric_config = pm.config;
                    result.params.boom = boom_params;
                    // Compute effective sectors if applicable
                    result.params.effective_sectors = std::visit([&](auto const& p) -> int {
                        using T = std::decay_t<decltype(p)>;
                        if constexpr (std::is_same_v<T, SectorMetricParams>) {
                            return computeEffectiveSectors(N, p);
                        } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
                            SectorMetricParams sp{p.min_sectors, p.max_sectors, p.target_per_sector, {}};
                            return computeEffectiveSectors(N, sp);
                        } else {
                            return 0;  // Non-sector metrics
                        }
                    }, pm.config.params);
                    result.samples_evaluated = static_cast<int>(errors.size());
                    result.per_sim_errors = errors;

                    if (!errors.empty()) {
                        // Compute MAE
                        double sum = 0.0;
                        for (int e : errors) {
                            sum += e;
                        }
                        result.boom_mae = sum / errors.size();

                        // Compute stddev
                        double sq_sum = 0.0;
                        for (int e : errors) {
                            double diff = e - result.boom_mae;
                            sq_sum += diff * diff;
                        }
                        result.boom_stddev = std::sqrt(sq_sum / errors.size());

                        // Compute median
                        std::vector<int> sorted_errors = errors;
                        std::sort(sorted_errors.begin(), sorted_errors.end());
                        size_t mid = sorted_errors.size() / 2;
                        if (sorted_errors.size() % 2 == 0) {
                            result.boom_median =
                                (sorted_errors[mid - 1] + sorted_errors[mid]) / 2.0;
                        } else {
                            result.boom_median = sorted_errors[mid];
                        }

                        // Max error
                        result.boom_max = *std::max_element(errors.begin(), errors.end());
                    } else {
                        result.boom_mae = 1e9;
                        result.boom_stddev = 0;
                        result.boom_median = 1e9;
                        result.boom_max = 1e9;
                    }

                    result.peak_mae = 1e9;
                    result.combined_score = result.boom_mae;

                    std::lock_guard<std::mutex> lock(results_mutex);
                    results.push_back(result);
                };

                // MaxCausticness: vary offset
                for (double offset : offset_vals) {
                    BoomDetectionParams bp;
                    bp.metric_name = pm.metric_name;
                    bp.method = BoomDetectionMethod::MaxCausticness;
                    bp.offset_seconds = offset;
                    evaluateBoomMethod(bp);
                }

                // FirstPeakPercent: vary peak_pct × offset × min_peak_prominence
                for (double pct : peak_pct_vals) {
                    for (double offset : offset_vals) {
                        for (double prominence : prominence_vals) {
                            BoomDetectionParams bp;
                            bp.metric_name = pm.metric_name;
                            bp.method = BoomDetectionMethod::FirstPeakPercent;
                            bp.peak_percent_threshold = pct;
                            bp.offset_seconds = offset;
                            bp.min_peak_prominence = prominence;
                            evaluateBoomMethod(bp);
                        }
                    }
                }

                // DerivativePeak: vary smoothing × offset
                for (int smooth : smooth_vals) {
                    for (double offset : offset_vals) {
                        BoomDetectionParams bp;
                        bp.metric_name = pm.metric_name;
                        bp.method = BoomDetectionMethod::DerivativePeak;
                        bp.smoothing_window = smooth;
                        bp.offset_seconds = offset;
                        evaluateBoomMethod(bp);
                    }
                }

                // ThresholdCrossing: vary threshold × confirmation × offset
                for (double thresh : crossing_thresh_vals) {
                    for (int confirm : crossing_confirm_vals) {
                        for (double offset : offset_vals) {
                            BoomDetectionParams bp;
                            bp.metric_name = pm.metric_name;
                            bp.method = BoomDetectionMethod::ThresholdCrossing;
                            bp.crossing_threshold = thresh;
                            bp.crossing_confirmation = confirm;
                            bp.offset_seconds = offset;
                            evaluateBoomMethod(bp);
                        }
                    }
                }

                // SecondDerivativePeak: vary smoothing × offset
                for (int smooth : smooth_vals) {
                    for (double offset : offset_vals) {
                        BoomDetectionParams bp;
                        bp.metric_name = pm.metric_name;
                        bp.method = BoomDetectionMethod::SecondDerivativePeak;
                        bp.smoothing_window = smooth;
                        bp.offset_seconds = offset;
                        evaluateBoomMethod(bp);
                    }
                }

                pm_completed.fetch_add(1);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    phase2_done.store(true);
    phase2_progress.join();

    auto phase2_end = std::chrono::steady_clock::now();
    double phase2_total = std::chrono::duration<double>(phase2_end - phase1_done).count();
    double total_secs = std::chrono::duration<double>(phase2_end - start_time).count();

    std::cout << "\nPhase 2 complete: " << std::fixed << std::setprecision(2) << phase2_total
              << "s\n";
    std::cout << "Total time: " << total_secs << "s for " << results.size() << " evaluations"
              << " (" << std::setprecision(0) << (results.size() / total_secs) << " evals/sec)\n\n";

    // Sort by combined score
    std::sort(results.begin(), results.end(),
              [](auto const& a, auto const& b) { return a.combined_score < b.combined_score; });

    // ========================================
    // RESULTS ANALYSIS
    // ========================================
    std::cout << std::string(100, '=') << "\n";
    std::cout << "OPTIMIZATION RESULTS\n";
    std::cout << std::string(100, '=') << "\n\n";

    // ----------------------------------------
    // Top 15 configurations (with full details)
    // ----------------------------------------
    std::cout << "TOP 15 CONFIGURATIONS (by Mean Absolute Error)\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << std::setw(4) << "Rank" << std::setw(8) << "MAE" << std::setw(8) << "Median"
              << std::setw(8) << "StdDev" << std::setw(8) << "Max"
              << "  Configuration\n";
    std::cout << std::string(100, '-') << "\n";

    for (size_t i = 0; i < std::min(results.size(), size_t(15)); ++i) {
        auto const& r = results[i];
        std::cout << std::setw(4) << (i + 1) << std::setw(8) << std::fixed << std::setprecision(1)
                  << r.boom_mae << std::setw(8) << std::setprecision(1) << r.boom_median
                  << std::setw(8) << std::setprecision(1) << r.boom_stddev << std::setw(8)
                  << std::setprecision(0) << r.boom_max << "  " << r.params.describeFull() << "\n";
    }
    std::cout << std::string(100, '-') << "\n\n";

    // ----------------------------------------
    // Best configuration per metric type
    // ----------------------------------------
    std::cout << "BEST CONFIGURATION PER METRIC TYPE\n";
    std::cout << std::string(100, '-') << "\n";

    std::vector<std::string> metric_types = {"angular_causticness",
                                             "tip_causticness",
                                             "organization_causticness",
                                             "cv_causticness",
                                             "spatial_concentration",
                                             "fold_causticness",
                                             "trajectory_smoothness",
                                             "curvature",
                                             "true_folds",
                                             "local_coherence"};

    for (auto const& metric_type : metric_types) {
        // Find best result for this metric
        EvaluationResult const* best = nullptr;
        for (auto const& r : results) {
            if (r.params.boom.metric_name == metric_type) {
                if (!best || r.boom_mae < best->boom_mae) {
                    best = &r;
                }
            }
        }
        if (best) {
            // Shorten metric name
            std::string short_name = metric_type;
            if (short_name.length() > 20) {
                short_name = short_name.substr(0, 17) + "...";
            }
            std::cout << "  " << std::left << std::setw(22) << short_name << std::right
                      << " MAE=" << std::setw(6) << std::fixed << std::setprecision(1)
                      << best->boom_mae << " | " << best->params.describeFull() << "\n";
        }
    }
    std::cout << std::string(100, '-') << "\n\n";

    // ----------------------------------------
    // Best configuration per method type
    // ----------------------------------------
    std::cout << "BEST CONFIGURATION PER METHOD TYPE\n";
    std::cout << std::string(100, '-') << "\n";

    std::vector<std::pair<BoomDetectionMethod, std::string>> method_types = {
        {BoomDetectionMethod::MaxCausticness, "MaxCausticness"},
        {BoomDetectionMethod::FirstPeakPercent, "FirstPeakPercent"},
        {BoomDetectionMethod::DerivativePeak, "DerivativePeak"}};

    for (auto const& [method, method_name] : method_types) {
        EvaluationResult const* best = nullptr;
        for (auto const& r : results) {
            if (r.params.boom.method == method) {
                if (!best || r.boom_mae < best->boom_mae) {
                    best = &r;
                }
            }
        }
        if (best) {
            std::cout << "  " << std::left << std::setw(18) << method_name << std::right
                      << " MAE=" << std::setw(6) << std::fixed << std::setprecision(1)
                      << best->boom_mae << " | " << best->params.describeFull() << "\n";
        }
    }
    std::cout << std::string(100, '-') << "\n\n";

    // ----------------------------------------
    // Best effective sector counts (aggregated across all boom methods)
    // ----------------------------------------
    std::cout << "BEST EFFECTIVE SECTOR COUNTS (aggregated across boom methods)\n";
    std::cout << std::string(100, '-') << "\n";

    // Group results by effective_sectors and find average MAE
    std::map<int, std::vector<double>> eff_sector_maes;
    for (auto const& r : results) {
        eff_sector_maes[r.params.effective_sectors].push_back(r.boom_mae);
    }

    // Compute average MAE for each effective sector count
    std::vector<std::pair<int, double>> eff_sector_avg;
    for (auto const& [eff_sec, maes] : eff_sector_maes) {
        double sum = 0;
        for (double m : maes) {
            sum += m;
        }
        eff_sector_avg.push_back({eff_sec, sum / maes.size()});
    }

    // Sort by average MAE
    std::sort(eff_sector_avg.begin(), eff_sector_avg.end(),
              [](auto const& a, auto const& b) { return a.second < b.second; });

    // Show all (there shouldn't be too many)
    std::cout << "  " << std::setw(15) << "Eff Sectors" << std::setw(12) << "Avg MAE"
              << std::setw(12) << "Best MAE" << std::setw(10) << "Count" << "\n";
    for (auto const& [eff_sec, avg_mae] : eff_sector_avg) {
        // Find best MAE for this effective sector count
        double best_mae = 1e9;
        for (auto const& r : results) {
            if (r.params.effective_sectors == eff_sec) {
                best_mae = std::min(best_mae, r.boom_mae);
            }
        }

        std::cout << "  " << std::setw(15) << eff_sec << std::setw(12) << std::fixed
                  << std::setprecision(1) << avg_mae << std::setw(12) << std::setprecision(1)
                  << best_mae << std::setw(10) << eff_sector_maes[eff_sec].size() << "\n";
    }
    std::cout << std::string(100, '-') << "\n\n";

    // ----------------------------------------
    // Winner details
    // ----------------------------------------
    if (!results.empty()) {
        auto const& winner = results[0];
        std::cout << "WINNER DETAILS\n";
        std::cout << std::string(100, '-') << "\n";
        std::cout << "  Metric:           " << winner.params.boom.metric_name << "\n";
        std::cout << "  Method:           ";
        switch (winner.params.boom.method) {
        case BoomDetectionMethod::MaxCausticness:
            std::cout << "MaxCausticness (offset=" << winner.params.boom.offset_seconds << "s)\n";
            break;
        case BoomDetectionMethod::FirstPeakPercent:
            std::cout << "FirstPeakPercent (threshold="
                      << (int)(winner.params.boom.peak_percent_threshold * 100) << "%)\n";
            break;
        case BoomDetectionMethod::DerivativePeak:
            std::cout << "DerivativePeak (window=" << winner.params.boom.smoothing_window << ")\n";
            break;
        case BoomDetectionMethod::ThresholdCrossing:
            std::cout << "ThresholdCrossing (thresh="
                      << (int)(winner.params.boom.crossing_threshold * 100) << "%, confirm="
                      << winner.params.boom.crossing_confirmation << ")\n";
            break;
        case BoomDetectionMethod::SecondDerivativePeak:
            std::cout << "SecondDerivativePeak (window=" << winner.params.boom.smoothing_window << ")\n";
            break;
        }
        std::cout << "  Effective Sectors: " << winner.params.effective_sectors << "\n";
        // Print metric-specific parameters
        std::cout << "  Params:           ";
        std::visit([&](auto const& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, SectorMetricParams>) {
                std::cout << "min_sectors=" << p.min_sectors
                          << ", max_sectors=" << p.max_sectors
                          << ", target=" << p.target_per_sector;
            } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
                std::cout << "min_sectors=" << p.min_sectors
                          << ", max_sectors=" << p.max_sectors
                          << ", target=" << p.target_per_sector
                          << ", cv_norm=" << p.cv_normalization;
            } else if constexpr (std::is_same_v<T, GridMetricParams>) {
                std::cout << "min_grid=" << p.min_grid
                          << ", max_grid=" << p.max_grid
                          << ", target=" << p.target_per_cell;
            } else if constexpr (std::is_same_v<T, FoldMetricParams>) {
                std::cout << "max_radius=" << p.max_radius
                          << ", cv_norm=" << p.cv_normalization;
            } else if constexpr (std::is_same_v<T, TrajectoryMetricParams>) {
                std::cout << "max_radius=" << p.max_radius
                          << ", min_spread=" << p.min_spread_threshold;
            } else if constexpr (std::is_same_v<T, CurvatureMetricParams>) {
                std::cout << "max_radius=" << p.max_radius
                          << ", min_spread=" << p.min_spread_threshold
                          << ", log_ratio_norm=" << p.log_ratio_normalization;
            } else if constexpr (std::is_same_v<T, TrueFoldsMetricParams>) {
                std::cout << "max_radius=" << p.max_radius
                          << ", min_spread=" << p.min_spread_threshold
                          << ", gini_baseline=" << p.gini_chaos_baseline
                          << ", gini_divisor=" << p.gini_baseline_divisor;
            } else if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
                std::cout << "max_radius=" << p.max_radius
                          << ", min_spread=" << p.min_spread_threshold
                          << ", log_baseline=" << p.log_inverse_baseline
                          << ", log_divisor=" << p.log_inverse_divisor;
            }
        }, winner.params.metric_config.params);
        std::cout << "\n\n";
        std::cout << "  Statistics:\n";
        std::cout << "    Mean Absolute Error:   " << std::fixed << std::setprecision(2)
                  << winner.boom_mae << " frames\n";
        std::cout << "    Median Absolute Error: " << std::setprecision(2) << winner.boom_median
                  << " frames\n";
        std::cout << "    Std Deviation:         " << std::setprecision(2) << winner.boom_stddev
                  << " frames\n";
        std::cout << "    Max Error:             " << std::setprecision(0) << winner.boom_max
                  << " frames\n";
        std::cout << "    Samples:               " << winner.samples_evaluated << "\n";

        // Per-simulation breakdown if we have the data
        if (!winner.per_sim_errors.empty() && winner.per_sim_errors.size() == simulations.size()) {
            std::cout << "\n  Per-Simulation Errors:\n";
            for (size_t i = 0; i < simulations.size(); ++i) {
                std::cout << "    " << std::left << std::setw(30) << simulations[i].id << std::right
                          << " error=" << std::setw(4) << winner.per_sim_errors[i] << " frames"
                          << " (truth=" << simulations[i].boom_frame_truth << ")\n";
            }
        }
        std::cout << std::string(100, '-') << "\n\n";

        // Save best parameters
        saveBestParams("best_params.toml", winner);
    }

    std::cout << std::string(100, '=') << "\n";

    return 0;
}
