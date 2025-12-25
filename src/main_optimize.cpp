// Metric optimization tool for double pendulum visualization
//
// Performs grid search over metric parameters to find optimal settings
// for boom and peak detection based on annotated ground truth data.
//
// Usage:
//   ./pendulum-optimize annotations.json [options] [simulation_data.bin ...]
//
// Options:
//   --grid-steps <N>   Grid resolution per dimension (default: 8, use 3-4 for quick tests)
//   --save-cache <dir> Save Phase 1 computed metrics to cache directory
//   --load-cache <dir> Load Phase 1 metrics from cache (skip computation)
//   --output <file>    Output file for best parameters (default: best_params.toml)
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
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// GRID SEARCH PARAMETER SYSTEM
// ============================================================================

// A single parameter dimension in the search grid
struct ParamDim {
    std::string name;
    double min_val;
    double max_val;
    int snap_multiple = 1;  // For integer params (sectors), snap to multiples
    bool is_integer = false;

    // Generate values for this dimension given step count
    std::vector<double> generate(int steps, int N = 0) const {
        // For integer params, clamp max based on N if needed
        double effective_max = max_val;
        if (name == "sectors" && N > 0) {
            effective_max = std::min(max_val, static_cast<double>(N / 2));
        } else if (name == "grid" && N > 0) {
            effective_max = std::min(max_val, std::floor(std::sqrt(static_cast<double>(N))));
        }
        effective_max = std::max(min_val, effective_max);

        std::set<double> unique;
        if (steps <= 1) {
            unique.insert(min_val);
        } else {
            for (int i = 0; i < steps; ++i) {
                double t = static_cast<double>(i) / (steps - 1);
                double v = min_val + t * (effective_max - min_val);
                if (is_integer || snap_multiple > 1) {
                    v = std::round(v / snap_multiple) * snap_multiple;
                    v = std::clamp(v, min_val, effective_max);
                }
                unique.insert(v);
            }
        }
        return std::vector<double>(unique.begin(), unique.end());
    }
};

// Generate Cartesian product of all dimensions
std::vector<std::vector<double>> cartesianProduct(
    std::vector<std::vector<double>> const& dimensions) {
    if (dimensions.empty()) return {{}};

    std::vector<std::vector<double>> result;
    std::vector<size_t> indices(dimensions.size(), 0);

    while (true) {
        // Build current combination
        std::vector<double> combo;
        for (size_t i = 0; i < dimensions.size(); ++i) {
            combo.push_back(dimensions[i][indices[i]]);
        }
        result.push_back(combo);

        // Increment indices (like an odometer)
        size_t dim = dimensions.size() - 1;
        while (true) {
            indices[dim]++;
            if (indices[dim] < dimensions[dim].size()) break;
            indices[dim] = 0;
            if (dim == 0) return result;  // All done
            dim--;
        }
    }
}

// Metric schema: defines what parameters a metric uses
struct MetricSchema {
    std::string name;
    std::vector<ParamDim> dims;
    std::function<::MetricConfig(std::vector<double> const&, int N)> make_config;
};

// Helper to create sector params from effective sector count
SectorMetricParams makeSectorParams(int eff_sec, int N) {
    SectorMetricParams p;
    p.max_sectors = eff_sec;
    p.min_sectors = std::min(8, eff_sec);
    p.target_per_sector = std::max(1, N / (eff_sec * 2));
    return p;
}

// Helper to create grid params from effective grid size
GridMetricParams makeGridParams(int eff_grid, int N) {
    GridMetricParams p;
    p.max_grid = eff_grid;
    p.min_grid = std::min(4, eff_grid);
    p.target_per_cell = std::max(1, N / (eff_grid * eff_grid * 2));
    return p;
}

// Build all metric schemas
std::vector<MetricSchema> buildMetricSchemas() {
    std::vector<MetricSchema> schemas;

    // Sector-based metrics (angular_causticness, tip_causticness, etc.)
    auto sector_dim = ParamDim{"sectors", 8, 128, 2, true};
    auto make_sector_config = [](std::string const& metric_name) {
        return [metric_name](std::vector<double> const& vals, int N) -> ::MetricConfig {
            int eff_sec = static_cast<int>(vals[0]);
            auto params = makeSectorParams(eff_sec, N);
            ::MetricConfig cfg;
            cfg.name = metric_name;
            cfg.params = params;
            return cfg;
        };
    };

    for (auto const& name : {"angular_causticness", "tip_causticness", "organization_causticness",
                             "r1_concentration", "r2_concentration", "joint_concentration"}) {
        schemas.push_back({name, {sector_dim}, make_sector_config(name)});
    }

    // Variance (no real parameters, but we include it)
    schemas.push_back({"variance", {},
        [](std::vector<double> const&, int) -> ::MetricConfig {
            ::MetricConfig cfg;
            cfg.name = "variance";
            cfg.params = SectorMetricParams{};
            return cfg;
        }});

    // CV causticness: sectors × cv_normalization
    schemas.push_back({"cv_causticness",
        {sector_dim, {"cv_norm", 0.5, 3.0, 1, false}},
        [](std::vector<double> const& vals, int N) -> ::MetricConfig {
            int eff_sec = static_cast<int>(vals[0]);
            double cv_norm = vals[1];
            CVSectorMetricParams params;
            params.max_sectors = eff_sec;
            params.min_sectors = std::min(8, eff_sec);
            params.target_per_sector = std::max(1, N / (eff_sec * 2));
            params.cv_normalization = cv_norm;
            ::MetricConfig cfg;
            cfg.name = "cv_causticness";
            cfg.params = params;
            return cfg;
        }});

    // Spatial concentration: grid
    schemas.push_back({"spatial_concentration",
        {{"grid", 4, 64, 1, true}},
        [](std::vector<double> const& vals, int N) -> ::MetricConfig {
            int eff_grid = static_cast<int>(vals[0]);
            auto params = makeGridParams(eff_grid, N);
            ::MetricConfig cfg;
            cfg.name = "spatial_concentration";
            cfg.params = params;
            return cfg;
        }});

    // Fold causticness: max_radius × cv_normalization
    schemas.push_back({"fold_causticness",
        {{"max_radius", 1.0, 2.5, 1, false}, {"cv_norm", 0.5, 3.0, 1, false}},
        [](std::vector<double> const& vals, int) -> ::MetricConfig {
            FoldMetricParams params;
            params.max_radius = vals[0];
            params.cv_normalization = vals[1];
            ::MetricConfig cfg;
            cfg.name = "fold_causticness";
            cfg.params = params;
            return cfg;
        }});

    // Trajectory smoothness: max_radius × min_spread
    schemas.push_back({"trajectory_smoothness",
        {{"max_radius", 1.0, 2.5, 1, false}, {"min_spread", 0.01, 0.1, 1, false}},
        [](std::vector<double> const& vals, int) -> ::MetricConfig {
            TrajectoryMetricParams params;
            params.max_radius = vals[0];
            params.min_spread_threshold = vals[1];
            ::MetricConfig cfg;
            cfg.name = "trajectory_smoothness";
            cfg.params = params;
            return cfg;
        }});

    // Curvature: max_radius × min_spread × log_ratio_normalization
    schemas.push_back({"curvature",
        {{"max_radius", 1.0, 2.5, 1, false},
         {"min_spread", 0.01, 0.1, 1, false},
         {"log_ratio_norm", 1.0, 2.5, 1, false}},
        [](std::vector<double> const& vals, int) -> ::MetricConfig {
            CurvatureMetricParams params;
            params.max_radius = vals[0];
            params.min_spread_threshold = vals[1];
            params.log_ratio_normalization = vals[2];
            ::MetricConfig cfg;
            cfg.name = "curvature";
            cfg.params = params;
            return cfg;
        }});

    // True folds: max_radius × min_spread × gini_baseline × gini_divisor
    schemas.push_back({"true_folds",
        {{"max_radius", 1.0, 2.5, 1, false},
         {"min_spread", 0.01, 0.1, 1, false},
         {"gini_baseline", 0.1, 0.5, 1, false},
         {"gini_divisor", 0.5, 0.8, 1, false}},
        [](std::vector<double> const& vals, int) -> ::MetricConfig {
            TrueFoldsMetricParams params;
            params.max_radius = vals[0];
            params.min_spread_threshold = vals[1];
            params.gini_chaos_baseline = vals[2];
            params.gini_baseline_divisor = vals[3];
            ::MetricConfig cfg;
            cfg.name = "true_folds";
            cfg.params = params;
            return cfg;
        }});

    // Local coherence: max_radius × min_spread × log_baseline × log_divisor
    schemas.push_back({"local_coherence",
        {{"max_radius", 1.0, 2.5, 1, false},
         {"min_spread", 0.01, 0.1, 1, false},
         {"log_baseline", 0.5, 1.5, 1, false},
         {"log_divisor", 1.5, 3.0, 1, false}},
        [](std::vector<double> const& vals, int) -> ::MetricConfig {
            LocalCoherenceMetricParams params;
            params.max_radius = vals[0];
            params.min_spread_threshold = vals[1];
            params.log_inverse_baseline = vals[2];
            params.log_inverse_divisor = vals[3];
            ::MetricConfig cfg;
            cfg.name = "local_coherence";
            cfg.params = params;
            return cfg;
        }});

    return schemas;
}

// A parameterized metric: metric name + config (generated from schema)
struct ParameterizedMetric {
    std::string metric_name;
    ::MetricConfig config;

    // Generate unique key for deduplication
    std::string key() const {
        std::ostringstream oss;
        oss << metric_name;
        std::visit([&](auto const& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, SectorMetricParams>) {
                oss << "_sec" << p.max_sectors;
            } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
                oss << "_sec" << p.max_sectors << "_cvn" << static_cast<int>(p.cv_normalization * 100);
            } else if constexpr (std::is_same_v<T, GridMetricParams>) {
                oss << "_grid" << p.max_grid;
            } else if constexpr (std::is_same_v<T, FoldMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 100)
                    << "_cvn" << static_cast<int>(p.cv_normalization * 100);
            } else if constexpr (std::is_same_v<T, TrajectoryMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 100)
                    << "_spr" << static_cast<int>(p.min_spread_threshold * 1000);
            } else if constexpr (std::is_same_v<T, CurvatureMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 100)
                    << "_spr" << static_cast<int>(p.min_spread_threshold * 1000)
                    << "_lrn" << static_cast<int>(p.log_ratio_normalization * 100);
            } else if constexpr (std::is_same_v<T, TrueFoldsMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 100)
                    << "_spr" << static_cast<int>(p.min_spread_threshold * 1000)
                    << "_gb" << static_cast<int>(p.gini_chaos_baseline * 100)
                    << "_gd" << static_cast<int>(p.gini_baseline_divisor * 100);
            } else if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
                oss << "_rad" << static_cast<int>(p.max_radius * 100)
                    << "_spr" << static_cast<int>(p.min_spread_threshold * 1000)
                    << "_lb" << static_cast<int>(p.log_inverse_baseline * 100)
                    << "_ld" << static_cast<int>(p.log_inverse_divisor * 100);
            }
        }, config.params);
        return oss.str();
    }

    // Human-readable description
    std::string describe() const {
        std::ostringstream oss;
        std::string short_name = metric_name;
        // Shorten common suffixes
        for (auto const& suffix : {"_causticness", "_concentration", "_coherence", "_smoothness"}) {
            auto pos = short_name.find(suffix);
            if (pos != std::string::npos) {
                short_name = short_name.substr(0, pos);
                break;
            }
        }
        oss << short_name;

        std::visit([&](auto const& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, SectorMetricParams>) {
                oss << " sec=" << p.max_sectors;
            } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
                oss << " sec=" << p.max_sectors << " cvn=" << std::fixed << std::setprecision(2) << p.cv_normalization;
            } else if constexpr (std::is_same_v<T, GridMetricParams>) {
                oss << " grid=" << p.max_grid;
            } else if constexpr (std::is_same_v<T, FoldMetricParams>) {
                oss << " rad=" << std::fixed << std::setprecision(2) << p.max_radius
                    << " cvn=" << p.cv_normalization;
            } else if constexpr (std::is_same_v<T, TrajectoryMetricParams>) {
                oss << " rad=" << std::fixed << std::setprecision(2) << p.max_radius
                    << " spr=" << p.min_spread_threshold;
            } else if constexpr (std::is_same_v<T, CurvatureMetricParams>) {
                oss << " rad=" << std::fixed << std::setprecision(2) << p.max_radius
                    << " lrn=" << p.log_ratio_normalization;
            } else if constexpr (std::is_same_v<T, TrueFoldsMetricParams>) {
                oss << " gini=" << std::fixed << std::setprecision(2) << p.gini_chaos_baseline
                    << "/" << p.gini_baseline_divisor;
            } else if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
                oss << " log=" << std::fixed << std::setprecision(2) << p.log_inverse_baseline
                    << "/" << p.log_inverse_divisor;
            }
        }, config.params);
        return oss.str();
    }
};

// Generate all parameterized metrics from schemas
std::vector<ParameterizedMetric> generateParameterizedMetrics(
    std::vector<MetricSchema> const& schemas, int grid_steps, int N) {

    std::vector<ParameterizedMetric> result;
    std::set<std::string> seen_keys;

    for (auto const& schema : schemas) {
        if (schema.dims.empty()) {
            // No parameters (e.g., variance)
            ParameterizedMetric pm;
            pm.metric_name = schema.name;
            pm.config = schema.make_config({}, N);
            auto k = pm.key();
            if (seen_keys.insert(k).second) {
                result.push_back(pm);
            }
        } else {
            // Generate grid for each dimension
            std::vector<std::vector<double>> dim_values;
            for (auto const& dim : schema.dims) {
                dim_values.push_back(dim.generate(grid_steps, N));
            }

            // Cartesian product
            auto combos = cartesianProduct(dim_values);
            for (auto const& combo : combos) {
                ParameterizedMetric pm;
                pm.metric_name = schema.name;
                pm.config = schema.make_config(combo, N);
                auto k = pm.key();
                if (seen_keys.insert(k).second) {
                    result.push_back(pm);
                }
            }
        }
    }

    return result;
}

// ============================================================================
// ANNOTATION AND SIMULATION DATA
// ============================================================================

struct Annotation {
    std::string id;
    std::string data_path;
    int boom_frame = -1;
    int peak_frame = -1;
    std::string notes;
};

// Simple JSON extraction helpers
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
}  // namespace

std::vector<Annotation> loadAnnotations(std::string const& path) {
    std::vector<Annotation> annotations;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open annotations file: " << path << "\n";
        return annotations;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

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

struct LoadedSimulation {
    std::string id;
    simulation_data::Reader reader;
    double frame_duration = 0.0;
    int boom_frame_truth = -1;
    int peak_frame_truth = -1;

    bool load(Annotation const& ann) {
        if (!reader.open(ann.data_path)) return false;
        id = ann.id;
        boom_frame_truth = ann.boom_frame;
        peak_frame_truth = ann.peak_frame;
        auto const& h = reader.header();
        frame_duration = h.duration_seconds / h.frame_count;
        return true;
    }
};

// ============================================================================
// EVALUATION RESULT
// ============================================================================

struct ParameterSet {
    ::MetricConfig metric_config;
    BoomDetectionParams boom;
    int effective_sectors = 0;

    std::string describeShort() const {
        std::ostringstream oss;
        std::string metric_short = boom.metric_name;
        for (auto const& suffix : {"_causticness", "_concentration", "_coherence"}) {
            auto pos = metric_short.find(suffix);
            if (pos != std::string::npos) {
                metric_short = metric_short.substr(0, pos);
                break;
            }
        }
        oss << metric_short << " ";
        switch (boom.method) {
        case BoomDetectionMethod::MaxCausticness:
            oss << "max";
            break;
        case BoomDetectionMethod::FirstPeakPercent:
            oss << "first@" << static_cast<int>(boom.peak_percent_threshold * 100) << "%"
                << " prom=" << std::fixed << std::setprecision(2) << boom.min_peak_prominence;
            break;
        case BoomDetectionMethod::DerivativePeak:
            oss << "deriv w=" << boom.smoothing_window;
            break;
        case BoomDetectionMethod::ThresholdCrossing:
            oss << "cross@" << static_cast<int>(boom.crossing_threshold * 100) << "% x"
                << boom.crossing_confirmation;
            break;
        case BoomDetectionMethod::SecondDerivativePeak:
            oss << "accel w=" << boom.smoothing_window;
            break;
        }
        oss << " off=" << std::fixed << std::setprecision(2) << boom.offset_seconds;
        return oss.str();
    }

    std::string describeFull() const {
        std::ostringstream oss;
        oss << describeShort();
        if (effective_sectors > 0) {
            oss << " [eff_sec=" << effective_sectors << "]";
        }
        return oss.str();
    }
};

struct EvaluationResult {
    ParameterSet params;
    double boom_mae = 0.0;
    double boom_stddev = 0.0;
    double boom_median = 0.0;
    double boom_max = 0.0;
    double peak_mae = 0.0;
    double combined_score = 0.0;
    int samples_evaluated = 0;
    std::vector<int> per_sim_errors;
};

// ============================================================================
// BOOM DETECTION PARAMETER GENERATION
// ============================================================================

struct BoomMethodGrid {
    std::vector<double> offset_vals;
    std::vector<double> peak_pct_vals;
    std::vector<double> prominence_vals;
    std::vector<int> smooth_vals;
    std::vector<double> crossing_thresh_vals;
    std::vector<int> crossing_confirm_vals;

    static BoomMethodGrid create(int steps) {
        BoomMethodGrid g;

        // Offset: always use full range with good granularity
        for (double x = -0.5; x <= 0.5001; x += 1.0 / steps) {
            g.offset_vals.push_back(x);
        }

        // Peak percent threshold
        double pct_step = 0.6 / std::max(1, steps - 1);
        for (double x = 0.3; x <= 0.9001; x += pct_step) {
            g.peak_pct_vals.push_back(x);
        }

        // Prominence
        double prom_step = 0.4 / std::max(1, steps - 1);
        for (double x = 0.01; x <= 0.4001; x += prom_step) {
            g.prominence_vals.push_back(x);
        }

        // Smoothing window (integer)
        std::set<int> smooth_set;
        for (int i = 0; i < steps; ++i) {
            double t = static_cast<double>(i) / std::max(1, steps - 1);
            int v = static_cast<int>(std::round(1 + t * 49));  // 1-50
            smooth_set.insert(v);
        }
        g.smooth_vals = std::vector<int>(smooth_set.begin(), smooth_set.end());

        // Crossing threshold
        double thresh_step = 0.7 / std::max(1, steps - 1);
        for (double x = 0.1; x <= 0.8001; x += thresh_step) {
            g.crossing_thresh_vals.push_back(x);
        }

        // Crossing confirmation (integer)
        std::set<int> confirm_set;
        for (int i = 0; i < std::min(steps, 7); ++i) {
            double t = static_cast<double>(i) / std::max(1, std::min(steps, 7) - 1);
            int v = static_cast<int>(std::round(1 + t * 9));  // 1-10
            confirm_set.insert(v);
        }
        g.crossing_confirm_vals = std::vector<int>(confirm_set.begin(), confirm_set.end());

        return g;
    }

    size_t totalMethods() const {
        return offset_vals.size() +
               (peak_pct_vals.size() * offset_vals.size() * prominence_vals.size()) +
               (smooth_vals.size() * offset_vals.size()) +
               (crossing_thresh_vals.size() * crossing_confirm_vals.size() * offset_vals.size()) +
               (smooth_vals.size() * offset_vals.size());
    }
};

// ============================================================================
// STREAMING EVALUATION (memory-efficient)
// ============================================================================

// Compute metrics for a single parameterized metric across all simulations
struct ComputedMetricsForConfig {
    std::string metric_name;
    std::vector<double> frame_durations;
    std::vector<int> boom_frame_truths;
    std::vector<metrics::MetricsCollector> collectors;
};

void computeMetricsForConfig(
    ParameterizedMetric const& pm,
    std::vector<LoadedSimulation> const& simulations,
    ComputedMetricsForConfig& out) {

    out.metric_name = pm.metric_name;
    out.frame_durations.resize(simulations.size());
    out.boom_frame_truths.resize(simulations.size());
    out.collectors.resize(simulations.size());

    std::unordered_map<std::string, ::MetricConfig> config_map;
    config_map[pm.metric_name] = pm.config;

    for (size_t i = 0; i < simulations.size(); ++i) {
        auto const& sim = simulations[i];
        auto const& header = sim.reader.header();

        out.frame_durations[i] = sim.frame_duration;
        out.boom_frame_truths[i] = sim.boom_frame_truth;

        out.collectors[i].setAllMetricConfigs(config_map);
        out.collectors[i].registerStandardMetrics();

        for (int frame = 0; frame < header.frame_count; ++frame) {
            auto const* packed = sim.reader.getFramePacked(frame);
            if (!packed) break;
            out.collectors[i].beginFrame(frame);
            out.collectors[i].updateFromPackedStates(packed, header.pendulum_count);
            out.collectors[i].endFrame();
        }
    }
}

// Evaluate a single boom method configuration
EvaluationResult evaluateBoomMethod(
    ParameterizedMetric const& pm,
    ComputedMetricsForConfig const& computed,
    BoomDetectionParams const& boom_params,
    int N) {

    std::vector<int> errors;
    errors.reserve(computed.collectors.size());

    for (size_t i = 0; i < computed.collectors.size(); ++i) {
        auto boom = metrics::findBoomFrame(
            computed.collectors[i], computed.frame_durations[i], boom_params);

        if (computed.boom_frame_truths[i] >= 0 && boom.frame >= 0) {
            int error = std::abs(boom.frame - computed.boom_frame_truths[i]);
            errors.push_back(error);
        }
    }

    EvaluationResult result;
    result.params.metric_config = pm.config;
    result.params.boom = boom_params;

    // Compute effective sectors if applicable
    result.params.effective_sectors = std::visit(
        [&](auto const& p) -> int {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, SectorMetricParams>) {
                return p.max_sectors;
            } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
                return p.max_sectors;
            } else {
                return 0;
            }
        },
        pm.config.params);

    result.samples_evaluated = static_cast<int>(errors.size());
    result.per_sim_errors = errors;

    if (!errors.empty()) {
        double sum = 0.0;
        for (int e : errors) sum += e;
        result.boom_mae = sum / errors.size();

        double sq_sum = 0.0;
        for (int e : errors) {
            double diff = e - result.boom_mae;
            sq_sum += diff * diff;
        }
        result.boom_stddev = std::sqrt(sq_sum / errors.size());

        std::vector<int> sorted_errors = errors;
        std::sort(sorted_errors.begin(), sorted_errors.end());
        size_t mid = sorted_errors.size() / 2;
        if (sorted_errors.size() % 2 == 0) {
            result.boom_median = (sorted_errors[mid - 1] + sorted_errors[mid]) / 2.0;
        } else {
            result.boom_median = sorted_errors[mid];
        }
        result.boom_max = *std::max_element(errors.begin(), errors.end());
    } else {
        result.boom_mae = 1e9;
        result.boom_median = 1e9;
        result.boom_max = 1e9;
    }

    result.peak_mae = 1e9;
    result.combined_score = result.boom_mae;

    return result;
}

// ============================================================================
// OUTPUT HELPERS
// ============================================================================

void writeMetricParams(std::ofstream& file, MetricParamsVariant const& params) {
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
    }, params);
}

void writeBoomParams(std::ofstream& file, BoomDetectionParams const& boom) {
    switch (boom.method) {
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
    file << "offset_seconds = " << std::fixed << std::setprecision(2) << boom.offset_seconds << "\n";
    file << "peak_percent_threshold = " << std::fixed << std::setprecision(2) << boom.peak_percent_threshold << "\n";
    file << "min_peak_prominence = " << std::fixed << std::setprecision(2) << boom.min_peak_prominence << "\n";
    file << "smoothing_window = " << boom.smoothing_window << "\n";
    file << "crossing_threshold = " << std::fixed << std::setprecision(2) << boom.crossing_threshold << "\n";
    file << "crossing_confirmation = " << boom.crossing_confirmation << "\n";
}

void saveAllBestParams(std::string const& path, std::vector<EvaluationResult> const& results,
                       EvaluationResult const& global_best) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not write to " << path << "\n";
        return;
    }

    // Find best result for each metric type
    std::map<std::string, EvaluationResult const*> best_per_metric;
    for (auto const& r : results) {
        std::string const& metric = r.params.boom.metric_name;
        if (best_per_metric.find(metric) == best_per_metric.end() ||
            r.boom_mae < best_per_metric[metric]->boom_mae) {
            best_per_metric[metric] = &r;
        }
    }

    file << "# Best parameters found by pendulum-optimize\n";
    file << "# Global best: " << global_best.params.boom.metric_name
         << " with MAE=" << std::fixed << std::setprecision(2) << global_best.boom_mae << " frames\n";
    file << "# Samples evaluated: " << global_best.samples_evaluated << "\n";
    file << "# This file contains best parameters for ALL metrics.\n";
    file << "# The [boom_detection] section at the end specifies which metric to use.\n\n";

    std::vector<std::pair<std::string, EvaluationResult const*>> sorted_metrics(
        best_per_metric.begin(), best_per_metric.end());
    std::sort(sorted_metrics.begin(), sorted_metrics.end(),
              [](auto const& a, auto const& b) { return a.second->boom_mae < b.second->boom_mae; });

    for (auto const& [metric_name, best] : sorted_metrics) {
        file << "# " << metric_name << ": MAE=" << std::fixed << std::setprecision(2)
             << best->boom_mae << " frames\n";
        file << "[metrics." << metric_name << "]\n";
        writeMetricParams(file, best->params.metric_config.params);
        file << "\n[metrics." << metric_name << ".boom]\n";
        writeBoomParams(file, best->params.boom);
        file << "\n";
    }

    file << "[boom_detection]\n";
    file << "active_metric = \"" << global_best.params.boom.metric_name << "\"\n";

    std::cout << "Best parameters for " << best_per_metric.size() << " metrics saved to: " << path << "\n";
}

// ============================================================================
// MAIN
// ============================================================================

void printUsage(char const* prog) {
    std::cerr
        << "Usage: " << prog << " annotations.json [options] [simulation_data.bin ...]\n\n"
        << "Performs grid search to find optimal metric parameters.\n\n"
        << "Options:\n"
        << "  --grid-steps <N>   Grid resolution per dimension (default: 8)\n"
        << "                     Use 3-4 for quick tests, 12-16 for thorough search\n"
        << "  --output <file>    Output file for best parameters (default: best_params.toml)\n"
        << "  --help             Show this help message\n\n"
        << "If simulation data files are provided on command line, they override\n"
        << "the paths in annotations.json.\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // Parse command line options
    std::string annotations_path;
    std::string output_file = "best_params.toml";
    int grid_steps = 8;  // Default resolution
    std::vector<std::string> data_paths;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--grid-steps" && i + 1 < argc) {
            grid_steps = std::stoi(argv[++i]);
            if (grid_steps < 1) grid_steps = 1;
            if (grid_steps > 64) grid_steps = 64;
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        } else if (annotations_path.empty()) {
            annotations_path = arg;
        } else {
            data_paths.push_back(arg);
        }
    }

    if (annotations_path.empty()) {
        std::cerr << "Error: annotations.json path required\n";
        printUsage(argv[0]);
        return 1;
    }

    // Load annotations
    auto annotations = loadAnnotations(annotations_path);
    if (annotations.empty()) {
        std::cerr << "No valid annotations found.\n";
        return 1;
    }
    std::cout << "Loaded " << annotations.size() << " annotations\n";

    // Override paths if provided
    for (size_t i = 0; i < data_paths.size() && i < annotations.size(); ++i) {
        annotations[i].data_path = data_paths[i];
    }

    // Validate and load simulations
    std::vector<LoadedSimulation> simulations;
    size_t total_frames = 0;
    size_t total_pendulums = 0;

    std::cout << "Loading simulations...\n";
    for (auto const& ann : annotations) {
        if (!std::filesystem::exists(ann.data_path)) {
            std::cerr << "  Skipping " << ann.id << ": file not found: " << ann.data_path << "\n";
            continue;
        }
        if (ann.boom_frame < 0 && ann.peak_frame < 0) {
            std::cerr << "  Skipping " << ann.id << ": no ground truth frames\n";
            continue;
        }

        LoadedSimulation sim;
        if (sim.load(ann)) {
            auto const& h = sim.reader.header();
            total_frames += h.frame_count;
            total_pendulums = h.pendulum_count;
            std::cout << "  " << ann.id << ": " << h.frame_count << " frames, "
                      << h.pendulum_count << " pendulums, boom@" << ann.boom_frame << "\n";
            simulations.push_back(std::move(sim));
        } else {
            std::cerr << "  FAILED: " << ann.data_path << "\n";
        }
    }

    if (simulations.empty()) {
        std::cerr << "No simulations loaded successfully.\n";
        return 1;
    }

    int N = static_cast<int>(total_pendulums);

    // Build metric schemas and generate configurations
    auto schemas = buildMetricSchemas();
    auto param_metrics = generateParameterizedMetrics(schemas, grid_steps, N);

    std::cout << "\n=== Grid Search Configuration ===\n";
    std::cout << "Grid steps: " << grid_steps << " per dimension\n";
    std::cout << "Simulations: " << simulations.size() << " (" << total_frames << " total frames)\n";
    std::cout << "Pendulums: " << N << "\n\n";

    // Count by metric type
    std::map<std::string, int> metric_counts;
    for (auto const& pm : param_metrics) {
        metric_counts[pm.metric_name]++;
    }
    std::cout << "Metric configurations (" << param_metrics.size() << " total):\n";
    for (auto const& [name, count] : metric_counts) {
        std::cout << "  " << name << ": " << count << "\n";
    }

    // Generate boom method grid
    auto boom_grid = BoomMethodGrid::create(grid_steps);
    size_t total_evals = param_metrics.size() * boom_grid.totalMethods();

    std::cout << "\nBoom detection methods: " << boom_grid.totalMethods() << "\n";
    std::cout << "Total evaluations: " << total_evals << "\n\n";

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    std::cout << "Threads: " << num_threads << "\n\n";

    // ============================================
    // STREAMING EVALUATION
    // Process one metric config at a time to save memory
    // ============================================

    auto start_time = std::chrono::steady_clock::now();
    std::vector<EvaluationResult> results;
    std::mutex results_mutex;
    std::mutex print_mutex;

    std::atomic<size_t> metrics_completed{0};
    std::atomic<bool> done{false};

    // Progress thread
    std::thread progress_thread([&]() {
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            size_t c = metrics_completed.load();
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            double rate = c > 0 ? c / elapsed : 0;
            double eta = rate > 0 ? (param_metrics.size() - c) / rate : 0;

            std::lock_guard<std::mutex> lock(print_mutex);
            std::cout << "\rProgress: " << c << "/" << param_metrics.size()
                      << " metrics (" << std::fixed << std::setprecision(1)
                      << (100.0 * c / param_metrics.size()) << "%)"
                      << " | " << std::setprecision(1) << elapsed << "s"
                      << " | ETA: " << std::setprecision(0) << eta << "s     " << std::flush;
        }
    });

    // Worker function for a single metric config
    auto processMetricConfig = [&](size_t idx) {
        auto const& pm = param_metrics[idx];

        // Phase 1: Compute metrics for this config
        ComputedMetricsForConfig computed;
        computeMetricsForConfig(pm, simulations, computed);

        // Phase 2: Evaluate all boom methods
        std::vector<EvaluationResult> local_results;

        auto evaluateMethod = [&](BoomDetectionParams const& bp) {
            auto result = evaluateBoomMethod(pm, computed, bp, N);
            local_results.push_back(result);
        };

        // MaxCausticness
        for (double offset : boom_grid.offset_vals) {
            BoomDetectionParams bp;
            bp.metric_name = pm.metric_name;
            bp.method = BoomDetectionMethod::MaxCausticness;
            bp.offset_seconds = offset;
            evaluateMethod(bp);
        }

        // FirstPeakPercent
        for (double pct : boom_grid.peak_pct_vals) {
            for (double offset : boom_grid.offset_vals) {
                for (double prom : boom_grid.prominence_vals) {
                    BoomDetectionParams bp;
                    bp.metric_name = pm.metric_name;
                    bp.method = BoomDetectionMethod::FirstPeakPercent;
                    bp.peak_percent_threshold = pct;
                    bp.offset_seconds = offset;
                    bp.min_peak_prominence = prom;
                    evaluateMethod(bp);
                }
            }
        }

        // DerivativePeak
        for (int smooth : boom_grid.smooth_vals) {
            for (double offset : boom_grid.offset_vals) {
                BoomDetectionParams bp;
                bp.metric_name = pm.metric_name;
                bp.method = BoomDetectionMethod::DerivativePeak;
                bp.smoothing_window = smooth;
                bp.offset_seconds = offset;
                evaluateMethod(bp);
            }
        }

        // ThresholdCrossing
        for (double thresh : boom_grid.crossing_thresh_vals) {
            for (int confirm : boom_grid.crossing_confirm_vals) {
                for (double offset : boom_grid.offset_vals) {
                    BoomDetectionParams bp;
                    bp.metric_name = pm.metric_name;
                    bp.method = BoomDetectionMethod::ThresholdCrossing;
                    bp.crossing_threshold = thresh;
                    bp.crossing_confirmation = confirm;
                    bp.offset_seconds = offset;
                    evaluateMethod(bp);
                }
            }
        }

        // SecondDerivativePeak
        for (int smooth : boom_grid.smooth_vals) {
            for (double offset : boom_grid.offset_vals) {
                BoomDetectionParams bp;
                bp.metric_name = pm.metric_name;
                bp.method = BoomDetectionMethod::SecondDerivativePeak;
                bp.smoothing_window = smooth;
                bp.offset_seconds = offset;
                evaluateMethod(bp);
            }
        }

        // Merge results
        {
            std::lock_guard<std::mutex> lock(results_mutex);
            results.insert(results.end(), local_results.begin(), local_results.end());
        }

        metrics_completed.fetch_add(1);
        // computed goes out of scope here, memory freed
    };

    // Process all metric configs using thread pool
    std::atomic<size_t> work_idx{0};
    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&]() {
            while (true) {
                size_t idx = work_idx.fetch_add(1);
                if (idx >= param_metrics.size()) break;
                processMetricConfig(idx);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    done.store(true);
    progress_thread.join();

    auto end_time = std::chrono::steady_clock::now();
    double total_secs = std::chrono::duration<double>(end_time - start_time).count();

    std::cout << "\nCompleted in " << std::fixed << std::setprecision(2) << total_secs << "s"
              << " (" << std::setprecision(0) << (results.size() / total_secs) << " evals/sec)\n\n";

    // Sort by score
    std::sort(results.begin(), results.end(),
              [](auto const& a, auto const& b) { return a.combined_score < b.combined_score; });

    // ============================================
    // RESULTS
    // ============================================
    std::cout << std::string(100, '=') << "\n";
    std::cout << "OPTIMIZATION RESULTS\n";
    std::cout << std::string(100, '=') << "\n\n";

    // Top 15
    std::cout << "TOP 15 CONFIGURATIONS\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << std::setw(4) << "Rank" << std::setw(8) << "MAE" << std::setw(8) << "Median"
              << std::setw(8) << "StdDev" << std::setw(8) << "Max" << "  Configuration\n";
    std::cout << std::string(100, '-') << "\n";

    for (size_t i = 0; i < std::min(results.size(), size_t(15)); ++i) {
        auto const& r = results[i];
        std::cout << std::setw(4) << (i + 1)
                  << std::setw(8) << std::fixed << std::setprecision(1) << r.boom_mae
                  << std::setw(8) << std::setprecision(1) << r.boom_median
                  << std::setw(8) << std::setprecision(1) << r.boom_stddev
                  << std::setw(8) << std::setprecision(0) << r.boom_max
                  << "  " << r.params.describeFull() << "\n";
    }
    std::cout << std::string(100, '-') << "\n\n";

    // Best per metric
    std::cout << "BEST PER METRIC TYPE\n";
    std::cout << std::string(100, '-') << "\n";
    std::map<std::string, EvaluationResult const*> best_per_metric;
    for (auto const& r : results) {
        std::string const& m = r.params.boom.metric_name;
        if (best_per_metric.find(m) == best_per_metric.end() ||
            r.boom_mae < best_per_metric[m]->boom_mae) {
            best_per_metric[m] = &r;
        }
    }
    std::vector<std::pair<std::string, EvaluationResult const*>> sorted_best(
        best_per_metric.begin(), best_per_metric.end());
    std::sort(sorted_best.begin(), sorted_best.end(),
              [](auto const& a, auto const& b) { return a.second->boom_mae < b.second->boom_mae; });
    for (auto const& [name, best] : sorted_best) {
        std::string short_name = name.length() > 22 ? name.substr(0, 19) + "..." : name;
        std::cout << "  " << std::left << std::setw(22) << short_name << std::right
                  << " MAE=" << std::setw(6) << std::fixed << std::setprecision(1)
                  << best->boom_mae << " | " << best->params.describeFull() << "\n";
    }
    std::cout << std::string(100, '-') << "\n\n";

    // Winner details
    if (!results.empty()) {
        auto const& winner = results[0];
        std::cout << "WINNER\n";
        std::cout << std::string(100, '-') << "\n";
        std::cout << "  Metric: " << winner.params.boom.metric_name << "\n";
        std::cout << "  MAE: " << std::fixed << std::setprecision(2) << winner.boom_mae << " frames\n";
        std::cout << "  Median: " << winner.boom_median << " frames\n";
        std::cout << "  StdDev: " << winner.boom_stddev << " frames\n";
        std::cout << "  Max: " << static_cast<int>(winner.boom_max) << " frames\n";
        std::cout << "  Samples: " << winner.samples_evaluated << "\n";

        if (!winner.per_sim_errors.empty() && winner.per_sim_errors.size() == simulations.size()) {
            std::cout << "\n  Per-simulation errors:\n";
            for (size_t i = 0; i < simulations.size(); ++i) {
                std::cout << "    " << std::left << std::setw(30) << simulations[i].id
                          << std::right << " error=" << std::setw(4) << winner.per_sim_errors[i]
                          << " (truth=" << simulations[i].boom_frame_truth << ")\n";
            }
        }
        std::cout << std::string(100, '-') << "\n\n";

        saveAllBestParams(output_file, results, winner);
    }

    std::cout << std::string(100, '=') << "\n";
    return 0;
}
