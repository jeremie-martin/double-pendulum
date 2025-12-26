// Metric optimization tool for double pendulum visualization
//
// Performs multi-phase optimization to find optimal metric parameters
// and detection methods for prediction targets.
//
// Usage:
//   ./pendulum-optimize annotations.json [options]
//
// Options:
//   --grid-steps <N>     Grid resolution per dimension (default: 8, use 3-4 for quick tests)
//   --primary <target>   Primary target for metric parameter optimization (default: first frame target)
//   --output <file>      Output file for best parameters (default: best_params.toml)
//
// Optimization Phases:
//   Phase 1: Compute metrics for all parameter configurations
//   Phase 2: Optimize primary target to find best metric parameters
//   Phase 3: Optimize secondary targets using primary's metric parameters
//
// Annotation format (JSON v2):
// {
//   "version": 2,
//   "target_defs": {
//     "boom_frame": "frame",
//     "boom_quality": "score"
//   },
//   "annotations": [
//     {
//       "id": "run_20241215_143022",
//       "data_path": "output/run_20241215_143022/simulation_data.bin",
//       "targets": {
//         "boom_frame": 180,
//         "boom_quality": 0.85
//       }
//     }
//   ]
// }

#include "config.h"
#include "metrics/boom_detection.h"
#include "metrics/metrics_collector.h"
#include "metrics/metrics_init.h"
#include "optimize/frame_detector.h"
#include "optimize/prediction_target.h"
#include "optimize/predictor_registry.h"
#include "optimize/score_predictor.h"
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
#include <json.hpp>
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

    // NOTE: The following 3-4 parameter metrics are disabled for now because they
    // create too many configurations (8^3=512 or 8^4=4096 per metric).
    // Uncomment when doing thorough optimization with more time/compute.

#if 0
    // Curvature: max_radius × min_spread × log_ratio_normalization (8^3 = 512 configs)
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

    // True folds: max_radius × min_spread × gini_baseline × gini_divisor (8^4 = 4096 configs)
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

    // Local coherence: max_radius × min_spread × log_baseline × log_divisor (8^4 = 4096 configs)
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
#endif

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
// TARGET DEFINITIONS AND ANNOTATIONS
// ============================================================================

// Target definition from annotations file
struct TargetDef {
    std::string name;
    optimize::PredictionType type = optimize::PredictionType::Frame;

    bool isFrame() const { return type == optimize::PredictionType::Frame; }
    bool isScore() const { return type == optimize::PredictionType::Score; }

    std::string typeString() const {
        return type == optimize::PredictionType::Frame ? "frame" : "score";
    }
};

struct Annotation {
    std::string id;
    std::string data_path;
    std::string notes;

    // V2 format: multiple targets with arbitrary names
    std::map<std::string, double> targets;  // "boom_frame" -> 180, "boom_quality" -> 0.85

    // Legacy V1 fields (for backward compatibility)
    int boom_frame = -1;
    int peak_frame = -1;

    // Get target value for frame-type targets (returns -1 if not found)
    int getTargetFrame(std::string const& name) const {
        auto it = targets.find(name);
        if (it != targets.end()) return static_cast<int>(it->second);
        // Fallback to v1 fields
        if (name == "boom_frame" || name == "boom") return boom_frame;
        if (name == "peak_frame" || name == "peak") return peak_frame;
        return -1;
    }

    // Get target value for score-type targets
    double getTargetScore(std::string const& name) const {
        auto it = targets.find(name);
        return it != targets.end() ? it->second : -1.0;
    }

    bool hasTarget(std::string const& name) const {
        if (targets.count(name)) return true;
        if ((name == "boom_frame" || name == "boom") && boom_frame >= 0) return true;
        if ((name == "peak_frame" || name == "peak") && peak_frame >= 0) return true;
        return false;
    }

    // Count how many targets this annotation has values for
    int countTargets(std::vector<TargetDef> const& target_defs) const {
        int count = 0;
        for (auto const& td : target_defs) {
            if (hasTarget(td.name)) count++;
        }
        return count;
    }
};

// Complete annotations data with target definitions
struct AnnotationsData {
    int version = 1;
    std::vector<TargetDef> target_defs;
    std::vector<Annotation> annotations;

    // Find target definition by name
    TargetDef const* findTargetDef(std::string const& name) const {
        for (auto const& td : target_defs) {
            if (td.name == name) return &td;
        }
        return nullptr;
    }

    // Get first frame-type target (for default primary)
    std::string firstFrameTarget() const {
        for (auto const& td : target_defs) {
            if (td.isFrame()) return td.name;
        }
        return "";
    }

    // Count annotations that have a specific target
    int countAnnotationsWithTarget(std::string const& target_name) const {
        int count = 0;
        for (auto const& ann : annotations) {
            if (ann.hasTarget(target_name)) count++;
        }
        return count;
    }
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

AnnotationsData loadAnnotations(std::string const& path) {
    AnnotationsData data;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open annotations file: " << path << "\n";
        return data;
    }

    try {
        nlohmann::json root = nlohmann::json::parse(file);

        // Check version (v2 supports target_defs and targets map)
        data.version = root.value("version", 1);

        // Parse target_defs (v2)
        if (data.version >= 2 && root.contains("target_defs")) {
            auto const& defs = root["target_defs"];
            if (defs.is_object()) {
                for (auto it = defs.begin(); it != defs.end(); ++it) {
                    TargetDef td;
                    td.name = it.key();
                    std::string type_str = it.value().get<std::string>();
                    td.type = optimize::parsePredictionType(type_str);
                    data.target_defs.push_back(td);
                }
            }
        }

        // If no target_defs, create defaults for backward compatibility
        if (data.target_defs.empty()) {
            data.target_defs.push_back({"boom_frame", optimize::PredictionType::Frame});
            data.target_defs.push_back({"peak_frame", optimize::PredictionType::Frame});
        }

        auto const& arr = root["annotations"];
        if (!arr.is_array()) {
            std::cerr << "Error: annotations must be an array\n";
            return data;
        }

        for (auto const& obj : arr) {
            Annotation ann;
            ann.id = obj.value("id", "");
            ann.data_path = obj.value("data_path", "");
            ann.notes = obj.value("notes", "");

            // V1 format: boom_frame, peak_frame as direct fields
            ann.boom_frame = obj.value("boom_frame", -1);
            ann.peak_frame = obj.value("peak_frame", -1);

            // V2 format: targets map
            if (data.version >= 2 && obj.contains("targets")) {
                auto const& targets_obj = obj["targets"];
                if (targets_obj.is_object()) {
                    for (auto it = targets_obj.begin(); it != targets_obj.end(); ++it) {
                        ann.targets[it.key()] = it.value().get<double>();
                    }
                    // Populate v1 fields from targets for backward compat
                    if (ann.boom_frame < 0 && ann.targets.count("boom_frame")) {
                        ann.boom_frame = static_cast<int>(ann.targets["boom_frame"]);
                    }
                    if (ann.peak_frame < 0 && ann.targets.count("peak_frame")) {
                        ann.peak_frame = static_cast<int>(ann.targets["peak_frame"]);
                    }
                }
            }

            // Also add v1 fields to targets map if not present
            if (ann.boom_frame >= 0 && !ann.targets.count("boom_frame")) {
                ann.targets["boom_frame"] = ann.boom_frame;
            }
            if (ann.peak_frame >= 0 && !ann.targets.count("peak_frame")) {
                ann.targets["peak_frame"] = ann.peak_frame;
            }

            if (!ann.id.empty() || !ann.data_path.empty()) {
                data.annotations.push_back(ann);
            }
        }
    } catch (nlohmann::json::exception const& e) {
        std::cerr << "Error parsing annotations JSON: " << e.what() << "\n";
        // Fall back to regex-based parsing for backward compat
        file.clear();
        file.seekg(0);
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        // Default target_defs for v1
        data.target_defs.push_back({"boom_frame", optimize::PredictionType::Frame});
        data.target_defs.push_back({"peak_frame", optimize::PredictionType::Frame});

        std::regex obj_pattern("\\{[^{}]*\"id\"[^{}]*\\}");
        auto obj_begin = std::sregex_iterator(content.begin(), content.end(), obj_pattern);
        auto obj_end = std::sregex_iterator();

        for (auto it = obj_begin; it != obj_end; ++it) {
            std::string obj_str = it->str();
            Annotation ann;
            ann.id = extractString(obj_str, "id");
            ann.data_path = extractString(obj_str, "data_path");
            ann.boom_frame = extractInt(obj_str, "boom_frame", -1);
            ann.peak_frame = extractInt(obj_str, "peak_frame", -1);
            ann.notes = extractString(obj_str, "notes");

            // Add to targets map
            if (ann.boom_frame >= 0) ann.targets["boom_frame"] = ann.boom_frame;
            if (ann.peak_frame >= 0) ann.targets["peak_frame"] = ann.peak_frame;

            if (!ann.id.empty() || !ann.data_path.empty()) {
                data.annotations.push_back(ann);
            }
        }
    }

    return data;
}

struct LoadedSimulation {
    std::string id;
    simulation_data::Reader reader;
    double frame_duration = 0.0;

    // Ground truth for all targets (from annotations)
    std::map<std::string, double> target_truths;  // target_name -> truth value

    bool load(Annotation const& ann) {
        if (!reader.open(ann.data_path)) return false;
        id = ann.id;
        target_truths = ann.targets;
        auto const& h = reader.header();
        frame_duration = h.duration_seconds / h.frame_count;
        return true;
    }

    // Get truth for a frame-type target
    int getFrameTruth(std::string const& target_name) const {
        auto it = target_truths.find(target_name);
        return it != target_truths.end() ? static_cast<int>(it->second) : -1;
    }

    // Get truth for a score-type target
    double getScoreTruth(std::string const& target_name) const {
        auto it = target_truths.find(target_name);
        return it != target_truths.end() ? it->second : -1.0;
    }

    bool hasTruth(std::string const& target_name) const {
        return target_truths.count(target_name) > 0;
    }
};

// ============================================================================
// EVALUATION RESULT
// ============================================================================

struct ParameterSet {
    ::MetricConfig metric_config;
    optimize::FrameDetectionParams boom;
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
        case optimize::FrameDetectionMethod::MaxValue:
            oss << "max";
            break;
        case optimize::FrameDetectionMethod::FirstPeakPercent:
            oss << "first@" << static_cast<int>(boom.peak_percent_threshold * 100) << "%"
                << " prom=" << std::fixed << std::setprecision(2) << boom.min_peak_prominence;
            break;
        case optimize::FrameDetectionMethod::DerivativePeak:
            oss << "deriv w=" << boom.smoothing_window;
            break;
        case optimize::FrameDetectionMethod::ThresholdCrossing:
            oss << "cross@" << static_cast<int>(boom.crossing_threshold * 100) << "% x"
                << boom.crossing_confirmation;
            break;
        case optimize::FrameDetectionMethod::SecondDerivativePeak:
            oss << "accel w=" << boom.smoothing_window;
            break;
        case optimize::FrameDetectionMethod::ConstantFrame:
            oss << "const=" << boom.constant_frame;
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
// STREAMING EVALUATION (memory-efficient with global thread pool)
// ============================================================================

// Computed metrics for one parameter configuration across all simulations
struct ComputedMetricsForConfig {
    std::vector<metrics::MetricsCollector> collectors;
    std::vector<double> frame_durations;
    std::vector<std::string> sim_ids;  // For error reporting
    std::atomic<size_t> sims_completed{0};

    // Default constructor
    ComputedMetricsForConfig() = default;

    // Move constructor (needed for vector storage)
    ComputedMetricsForConfig(ComputedMetricsForConfig&& other) noexcept
        : collectors(std::move(other.collectors))
        , frame_durations(std::move(other.frame_durations))
        , sim_ids(std::move(other.sim_ids))
        , sims_completed(other.sims_completed.load()) {}

    // Move assignment (needed for vector storage)
    ComputedMetricsForConfig& operator=(ComputedMetricsForConfig&& other) noexcept {
        if (this != &other) {
            collectors = std::move(other.collectors);
            frame_durations = std::move(other.frame_durations);
            sim_ids = std::move(other.sim_ids);
            sims_completed.store(other.sims_completed.load());
        }
        return *this;
    }

    // Delete copy operations (atomic not copyable)
    ComputedMetricsForConfig(ComputedMetricsForConfig const&) = delete;
    ComputedMetricsForConfig& operator=(ComputedMetricsForConfig const&) = delete;

    void init(size_t num_sims) {
        collectors.resize(num_sims);
        frame_durations.resize(num_sims);
        sim_ids.resize(num_sims);
        sims_completed.store(0);
    }

    void reset() {
        collectors.clear();
        collectors.shrink_to_fit();
        frame_durations.clear();
        sim_ids.clear();
        sims_completed.store(0);
    }
};

// Compute metrics for a single (config, sim) pair
void computeMetricsForSim(
    ParameterizedMetric const& pm,
    LoadedSimulation const& sim,
    metrics::MetricsCollector& collector,
    double& frame_duration_out,
    std::string& sim_id_out) {

    auto const& header = sim.reader.header();

    frame_duration_out = sim.frame_duration;
    sim_id_out = sim.id;

    std::unordered_map<std::string, ::MetricConfig> config_map;
    config_map[pm.metric_name] = pm.config;

    collector.setAllMetricConfigs(config_map);
    collector.registerStandardMetrics();

    for (int frame = 0; frame < static_cast<int>(header.frame_count); ++frame) {
        auto const* packed = sim.reader.getFramePacked(frame);
        if (packed == nullptr) break;
        collector.beginFrame(frame);
        collector.updateFromPackedStates(packed, header.pendulum_count);
        collector.endFrame();
    }
}

// Evaluate a frame detection method configuration for a specific target
EvaluationResult evaluateFrameTarget(
    ParameterizedMetric const& pm,
    ComputedMetricsForConfig const& computed,
    optimize::FrameDetectionParams const& detection_params,
    std::vector<LoadedSimulation> const& simulations,
    std::string const& target_name) {

    std::vector<int> errors;
    errors.reserve(computed.collectors.size());

    for (size_t i = 0; i < computed.collectors.size(); ++i) {
        int truth = simulations[i].getFrameTruth(target_name);
        if (truth < 0) continue;  // No ground truth for this sim

        auto detection = metrics::findBoomFrame(
            computed.collectors[i], computed.frame_durations[i], detection_params);

        if (detection.frame >= 0) {
            int error = std::abs(detection.frame - truth);
            errors.push_back(error);
        }
    }

    EvaluationResult result;
    result.params.metric_config = pm.config;
    result.params.boom = detection_params;

    // Compute effective sectors if applicable
    result.params.effective_sectors = std::visit(
        [](auto const& p) -> int {
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

std::string frameDetectionMethodToString(optimize::FrameDetectionMethod method) {
    switch (method) {
    case optimize::FrameDetectionMethod::MaxValue: return "max_value";
    case optimize::FrameDetectionMethod::FirstPeakPercent: return "first_peak_percent";
    case optimize::FrameDetectionMethod::DerivativePeak: return "derivative_peak";
    case optimize::FrameDetectionMethod::ThresholdCrossing: return "threshold_crossing";
    case optimize::FrameDetectionMethod::SecondDerivativePeak: return "second_derivative_peak";
    case optimize::FrameDetectionMethod::ConstantFrame: return "constant_frame";
    }
    return "max_value";
}

void writeFrameTargetParams(std::ofstream& file, std::string const& target_name,
                             optimize::FrameDetectionParams const& params) {
    file << "[targets." << target_name << "]\n";
    file << "type = \"frame\"\n";
    file << "metric = \"" << params.metric_name << "\"\n";
    file << "method = \"" << frameDetectionMethodToString(params.method) << "\"\n";

    // offset_seconds is used by all methods
    file << "offset_seconds = " << std::fixed << std::setprecision(2) << params.offset_seconds << "\n";

    // Only write method-specific params
    switch (params.method) {
    case optimize::FrameDetectionMethod::MaxValue:
        // No additional params
        break;
    case optimize::FrameDetectionMethod::FirstPeakPercent:
        file << "peak_percent_threshold = " << std::fixed << std::setprecision(2) << params.peak_percent_threshold << "\n";
        file << "min_peak_prominence = " << std::fixed << std::setprecision(2) << params.min_peak_prominence << "\n";
        break;
    case optimize::FrameDetectionMethod::DerivativePeak:
    case optimize::FrameDetectionMethod::SecondDerivativePeak:
        file << "smoothing_window = " << params.smoothing_window << "\n";
        break;
    case optimize::FrameDetectionMethod::ThresholdCrossing:
        file << "crossing_threshold = " << std::fixed << std::setprecision(2) << params.crossing_threshold << "\n";
        file << "crossing_confirmation = " << params.crossing_confirmation << "\n";
        break;
    case optimize::FrameDetectionMethod::ConstantFrame:
        file << "constant_frame = " << params.constant_frame << "\n";
        break;
    }
}


void writeScoreTargetParams(std::ofstream& file, std::string const& target_name,
                             optimize::ScoreParams const& params) {
    file << "[targets." << target_name << "]\n";
    file << "type = \"score\"\n";
    file << "metric = \"" << params.metric_name << "\"\n";
    file << "method = \"" << optimize::toString(params.method) << "\"\n";

    // Write method-specific params
    if (params.method == optimize::ScoreMethod::Composite && !params.weights.empty()) {
        file << "# weights: ";
        for (size_t i = 0; i < params.weights.size(); ++i) {
            if (i > 0) file << ", ";
            file << params.weights[i].first << "=" << params.weights[i].second;
        }
        file << "\n";
    } else if (params.method == optimize::ScoreMethod::PreBoomContrast ||
               params.method == optimize::ScoreMethod::BoomSteepness) {
        file << "window_seconds = " << std::fixed << std::setprecision(2) << params.window_seconds << "\n";
    } else if (params.method == optimize::ScoreMethod::DecayRate) {
        file << "decay_window_fraction = " << std::fixed << std::setprecision(2) << params.decay_window_fraction << "\n";
    } else if (params.method == optimize::ScoreMethod::Smoothness) {
        file << "smoothness_scale = " << std::fixed << std::setprecision(1) << params.smoothness_scale << "\n";
    } else if (params.method == optimize::ScoreMethod::BuildupGradient) {
        file << "gradient_scale = " << std::fixed << std::setprecision(1) << params.gradient_scale << "\n";
    }
}

// Evaluation result for score targets
struct ScoreEvaluationResult {
    optimize::ScoreParams params;
    double mae = std::numeric_limits<double>::max();  // Mean absolute error (0-1 scale)
    double median = 0.0;
    double max_error = 0.0;
    int samples_evaluated = 0;
    std::vector<double> per_sim_errors;

    std::string describeShort() const {
        std::string result = optimize::toString(params.method);
        // Add method-specific parameters
        if (params.method == optimize::ScoreMethod::PreBoomContrast ||
            params.method == optimize::ScoreMethod::BoomSteepness) {
            result += " w=" + std::to_string(params.window_seconds).substr(0, 4) + "s";
        } else if (params.method == optimize::ScoreMethod::DecayRate) {
            result += " f=" + std::to_string(params.decay_window_fraction).substr(0, 4);
        } else if (params.method == optimize::ScoreMethod::Smoothness) {
            result += " s=" + std::to_string(static_cast<int>(params.smoothness_scale));
        } else if (params.method == optimize::ScoreMethod::BuildupGradient) {
            result += " g=" + std::to_string(static_cast<int>(params.gradient_scale));
        }
        return result;
    }
};

// Optimized target result - stores the best config for each target
struct OptimizedTarget {
    std::string name;
    optimize::PredictionType type = optimize::PredictionType::Frame;

    // For frame targets
    EvaluationResult frame_result;

    // For score targets
    ScoreEvaluationResult score_result;

    bool isFrame() const { return type == optimize::PredictionType::Frame; }
    bool isScore() const { return type == optimize::PredictionType::Score; }

    double getMAE() const {
        return isFrame() ? frame_result.boom_mae : score_result.mae;
    }
};

void saveOptimizationResults(
    std::string const& path,
    std::string const& primary_target,
    EvaluationResult const& primary_result,
    std::vector<EvaluationResult> const& all_frame_results,
    std::map<std::string, OptimizedTarget> const& optimized_targets) {

    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not write to " << path << "\n";
        return;
    }

    file << "# Best parameters found by pendulum-optimize\n";
    file << "# Primary target: " << primary_target << "\n";
    file << "# Best metric: " << primary_result.params.boom.metric_name
         << " with MAE=" << std::fixed << std::setprecision(2) << primary_result.boom_mae << " frames\n";
    file << "# Samples evaluated: " << primary_result.samples_evaluated << "\n\n";

    // Find best result for each metric type from all_frame_results
    std::map<std::string, EvaluationResult const*> best_per_metric;
    for (auto const& r : all_frame_results) {
        std::string const& metric = r.params.boom.metric_name;
        if (best_per_metric.find(metric) == best_per_metric.end() ||
            r.boom_mae < best_per_metric[metric]->boom_mae) {
            best_per_metric[metric] = &r;
        }
    }

    std::vector<std::pair<std::string, EvaluationResult const*>> sorted_metrics(
        best_per_metric.begin(), best_per_metric.end());
    std::sort(sorted_metrics.begin(), sorted_metrics.end(),
              [](auto const& a, auto const& b) { return a.second->boom_mae < b.second->boom_mae; });

    // Write metric computation params
    for (auto const& [metric_name, best] : sorted_metrics) {
        file << "# " << metric_name << ": MAE=" << std::fixed << std::setprecision(2)
             << best->boom_mae << " frames\n";
        file << "[metrics." << metric_name << "]\n";
        writeMetricParams(file, best->params.metric_config.params);
        file << "\n";
    }

    // Write all optimized targets (frame and score)
    for (auto const& [target_name, opt] : optimized_targets) {
        if (opt.isFrame()) {
            file << "# " << target_name << ": MAE=" << std::fixed << std::setprecision(2)
                 << opt.frame_result.boom_mae << " frames"
                 << " (" << opt.frame_result.samples_evaluated << " samples)\n";
            writeFrameTargetParams(file, target_name, opt.frame_result.params.boom);
        } else {
            file << "# " << target_name << ": MAE=" << std::fixed << std::setprecision(4)
                 << opt.score_result.mae << " (score units, 0-1 scale)"
                 << " (" << opt.score_result.samples_evaluated << " samples)\n";
            writeScoreTargetParams(file, target_name, opt.score_result.params);
        }
        file << "\n";
    }

    std::cout << "Results saved to: " << path << "\n";
}

// ============================================================================
// PHASE 1 METRICS CACHING
// ============================================================================
// Save/load computed metrics to avoid expensive recomputation.
// The cached data includes:
// - All ParameterizedMetric configurations
// - For each config, metric values for all simulations
// - Validation data (sim IDs, frame counts, checksums)

// Cache file format version - increment when format changes
constexpr int METRICS_CACHE_VERSION = 1;

// Cached metric data for one simulation
struct CachedSimulationMetrics {
    std::string sim_id;
    double frame_duration;
    int frame_count;
    std::vector<double> metric_values;  // The actual time series
};

// Cached data for one parameter configuration
struct CachedMetricConfig {
    std::string metric_name;
    nlohmann::json config_json;  // Serialized MetricConfig
    std::vector<CachedSimulationMetrics> simulations;
};

// Full cache structure
struct MetricsCache {
    int version = METRICS_CACHE_VERSION;
    int grid_steps = 0;
    std::string annotations_path;
    std::vector<std::string> sim_ids;  // Expected simulation IDs in order
    std::vector<CachedMetricConfig> configs;
};

// Serialize MetricConfig to JSON
nlohmann::json metricConfigToJson(MetricConfig const& config) {
    nlohmann::json j;
    j["name"] = config.name;
    std::visit([&j](auto const& params) {
        using T = std::decay_t<decltype(params)>;
        if constexpr (std::is_same_v<T, SectorMetricParams>) {
            j["type"] = "sector";
            j["min_sectors"] = params.min_sectors;
            j["max_sectors"] = params.max_sectors;
            j["target_per_sector"] = params.target_per_sector;
        } else if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
            j["type"] = "cv_sector";
            j["min_sectors"] = params.min_sectors;
            j["max_sectors"] = params.max_sectors;
            j["target_per_sector"] = params.target_per_sector;
            j["cv_normalization"] = params.cv_normalization;
        } else if constexpr (std::is_same_v<T, GridMetricParams>) {
            j["type"] = "grid";
            j["min_grid"] = params.min_grid;
            j["max_grid"] = params.max_grid;
            j["target_per_cell"] = params.target_per_cell;
        } else if constexpr (std::is_same_v<T, FoldMetricParams>) {
            j["type"] = "fold";
            j["max_radius"] = params.max_radius;
            j["cv_normalization"] = params.cv_normalization;
        } else if constexpr (std::is_same_v<T, TrajectoryMetricParams>) {
            j["type"] = "trajectory";
            j["max_radius"] = params.max_radius;
            j["min_spread_threshold"] = params.min_spread_threshold;
        } else if constexpr (std::is_same_v<T, CurvatureMetricParams>) {
            j["type"] = "curvature";
            j["max_radius"] = params.max_radius;
            j["min_spread_threshold"] = params.min_spread_threshold;
            j["log_ratio_normalization"] = params.log_ratio_normalization;
        } else if constexpr (std::is_same_v<T, TrueFoldsMetricParams>) {
            j["type"] = "true_folds";
            j["max_radius"] = params.max_radius;
            j["min_spread_threshold"] = params.min_spread_threshold;
            j["gini_chaos_baseline"] = params.gini_chaos_baseline;
            j["gini_baseline_divisor"] = params.gini_baseline_divisor;
        } else if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
            j["type"] = "local_coherence";
            j["max_radius"] = params.max_radius;
            j["min_spread_threshold"] = params.min_spread_threshold;
            j["log_inverse_baseline"] = params.log_inverse_baseline;
            j["log_inverse_divisor"] = params.log_inverse_divisor;
        }
    }, config.params);
    return j;
}

// Deserialize MetricConfig from JSON
MetricConfig metricConfigFromJson(nlohmann::json const& j) {
    MetricConfig config;
    config.name = j.value("name", "");
    std::string type = j.value("type", "");

    if (type == "sector") {
        SectorMetricParams p;
        p.min_sectors = j.value("min_sectors", 8);
        p.max_sectors = j.value("max_sectors", 72);
        p.target_per_sector = j.value("target_per_sector", 40);
        config.params = p;
    } else if (type == "cv_sector") {
        CVSectorMetricParams p;
        p.min_sectors = j.value("min_sectors", 8);
        p.max_sectors = j.value("max_sectors", 72);
        p.target_per_sector = j.value("target_per_sector", 40);
        p.cv_normalization = j.value("cv_normalization", 1.5);
        config.params = p;
    } else if (type == "grid") {
        GridMetricParams p;
        p.min_grid = j.value("min_grid", 4);
        p.max_grid = j.value("max_grid", 32);
        p.target_per_cell = j.value("target_per_cell", 40);
        config.params = p;
    } else if (type == "fold") {
        FoldMetricParams p;
        p.max_radius = j.value("max_radius", 2.0);
        p.cv_normalization = j.value("cv_normalization", 1.5);
        config.params = p;
    } else if (type == "trajectory") {
        TrajectoryMetricParams p;
        p.max_radius = j.value("max_radius", 2.0);
        p.min_spread_threshold = j.value("min_spread_threshold", 0.05);
        config.params = p;
    } else if (type == "curvature") {
        CurvatureMetricParams p;
        p.max_radius = j.value("max_radius", 2.0);
        p.min_spread_threshold = j.value("min_spread_threshold", 0.05);
        p.log_ratio_normalization = j.value("log_ratio_normalization", 2.0);
        config.params = p;
    } else if (type == "true_folds") {
        TrueFoldsMetricParams p;
        p.max_radius = j.value("max_radius", 2.0);
        p.min_spread_threshold = j.value("min_spread_threshold", 0.05);
        p.gini_chaos_baseline = j.value("gini_chaos_baseline", 0.35);
        p.gini_baseline_divisor = j.value("gini_baseline_divisor", 0.65);
        config.params = p;
    } else if (type == "local_coherence") {
        LocalCoherenceMetricParams p;
        p.max_radius = j.value("max_radius", 2.0);
        p.min_spread_threshold = j.value("min_spread_threshold", 0.05);
        p.log_inverse_baseline = j.value("log_inverse_baseline", 1.0);
        p.log_inverse_divisor = j.value("log_inverse_divisor", 2.5);
        config.params = p;
    } else {
        // Default to SectorMetricParams for unknown types
        config.params = SectorMetricParams{};
    }

    return config;
}

// Save computed metrics to JSON file
bool saveMetricsCache(
    std::string const& path,
    std::vector<ParameterizedMetric> const& param_metrics,
    std::vector<ComputedMetricsForConfig> const& computed_configs,
    std::vector<LoadedSimulation> const& simulations,
    std::string const& annotations_path,
    int grid_steps) {

    std::cout << "Saving metrics cache to: " << path << "\n";

    nlohmann::json cache;
    cache["version"] = METRICS_CACHE_VERSION;
    cache["grid_steps"] = grid_steps;
    cache["annotations_path"] = annotations_path;

    // Save simulation IDs for validation
    nlohmann::json sim_ids = nlohmann::json::array();
    for (auto const& sim : simulations) {
        sim_ids.push_back(sim.id);
    }
    cache["sim_ids"] = sim_ids;

    // Save each config's computed metrics
    nlohmann::json configs = nlohmann::json::array();
    for (size_t cfg_idx = 0; cfg_idx < param_metrics.size(); ++cfg_idx) {
        auto const& pm = param_metrics[cfg_idx];
        auto const& computed = computed_configs[cfg_idx];

        nlohmann::json cfg_json;
        cfg_json["metric_name"] = pm.metric_name;
        cfg_json["config"] = metricConfigToJson(pm.config);

        // Save per-simulation data
        nlohmann::json sims = nlohmann::json::array();
        for (size_t sim_idx = 0; sim_idx < computed.collectors.size(); ++sim_idx) {
            auto const& collector = computed.collectors[sim_idx];
            auto const* series = collector.getMetric(pm.metric_name);

            nlohmann::json sim_json;
            sim_json["sim_id"] = computed.sim_ids[sim_idx];
            sim_json["frame_duration"] = computed.frame_durations[sim_idx];

            if (series != nullptr) {
                sim_json["frame_count"] = static_cast<int>(series->values().size());
                sim_json["values"] = series->values();
            } else {
                sim_json["frame_count"] = 0;
                sim_json["values"] = nlohmann::json::array();
            }

            sims.push_back(sim_json);
        }
        cfg_json["simulations"] = sims;

        configs.push_back(cfg_json);
    }
    cache["configs"] = configs;

    // Write to file
    std::ofstream file(path);
    if (!file) {
        std::cerr << "Error: Cannot write to " << path << "\n";
        return false;
    }

    // Use compact format to save space, but still readable
    file << cache.dump(2);
    file.close();

    std::cout << "  Saved " << param_metrics.size() << " metric configs x "
              << simulations.size() << " simulations\n";
    return true;
}

// Load metrics cache from JSON file
// Returns true if successful and populates output parameters
bool loadMetricsCache(
    std::string const& path,
    std::vector<ParameterizedMetric>& param_metrics_out,
    std::vector<ComputedMetricsForConfig>& computed_configs_out,
    std::vector<LoadedSimulation> const& simulations,
    std::string const& annotations_path,
    int grid_steps) {

    std::cout << "Loading metrics cache from: " << path << "\n";

    std::ifstream file(path);
    if (!file) {
        std::cerr << "Error: Cannot read " << path << "\n";
        return false;
    }

    nlohmann::json cache;
    try {
        file >> cache;
    } catch (nlohmann::json::parse_error const& e) {
        std::cerr << "Error: Invalid JSON in " << path << ": " << e.what() << "\n";
        return false;
    }

    // Version check
    int file_version = cache.value("version", 0);
    if (file_version != METRICS_CACHE_VERSION) {
        std::cerr << "Error: Cache version mismatch (file=" << file_version
                  << ", expected=" << METRICS_CACHE_VERSION << ")\n";
        return false;
    }

    // Grid steps check
    int file_grid_steps = cache.value("grid_steps", 0);
    if (file_grid_steps != grid_steps) {
        std::cerr << "Error: Grid steps mismatch (file=" << file_grid_steps
                  << ", expected=" << grid_steps << ")\n";
        return false;
    }

    // Annotations path check (informational, not strict - allow using same metrics with different annotations)
    std::string file_annotations_path = cache.value("annotations_path", "");
    if (file_annotations_path != annotations_path) {
        std::cerr << "Warning: Annotations path differs (file=" << file_annotations_path
                  << ", current=" << annotations_path << ")\n";
        // Don't fail, just warn - user may want to reuse metrics computation
    }

    // Validate simulation IDs match
    auto const& file_sim_ids = cache["sim_ids"];
    if (file_sim_ids.size() != simulations.size()) {
        std::cerr << "Error: Simulation count mismatch (file=" << file_sim_ids.size()
                  << ", expected=" << simulations.size() << ")\n";
        return false;
    }

    for (size_t i = 0; i < simulations.size(); ++i) {
        if (file_sim_ids[i].get<std::string>() != simulations[i].id) {
            std::cerr << "Error: Simulation ID mismatch at index " << i
                      << " (file=" << file_sim_ids[i].get<std::string>()
                      << ", expected=" << simulations[i].id << ")\n";
            return false;
        }
    }

    // Load configs
    auto const& configs = cache["configs"];
    param_metrics_out.clear();
    computed_configs_out.clear();
    param_metrics_out.reserve(configs.size());
    computed_configs_out.reserve(configs.size());

    for (auto const& cfg_json : configs) {
        // Reconstruct ParameterizedMetric
        ParameterizedMetric pm;
        pm.metric_name = cfg_json["metric_name"].get<std::string>();
        pm.config = metricConfigFromJson(cfg_json["config"]);
        param_metrics_out.push_back(pm);

        // Reconstruct ComputedMetricsForConfig
        ComputedMetricsForConfig computed;
        computed.init(simulations.size());

        auto const& sims_json = cfg_json["simulations"];
        for (size_t sim_idx = 0; sim_idx < sims_json.size(); ++sim_idx) {
            auto const& sim_json = sims_json[sim_idx];

            computed.sim_ids[sim_idx] = sim_json["sim_id"].get<std::string>();
            computed.frame_durations[sim_idx] = sim_json["frame_duration"].get<double>();

            // Reconstruct MetricsCollector with the metric series
            std::unordered_map<std::string, MetricConfig> config_map;
            config_map[pm.metric_name] = pm.config;
            computed.collectors[sim_idx].setAllMetricConfigs(config_map);
            computed.collectors[sim_idx].registerStandardMetrics();

            // Load metric values frame by frame
            auto const& values = sim_json["values"];
            for (size_t frame = 0; frame < values.size(); ++frame) {
                computed.collectors[sim_idx].beginFrame(static_cast<int>(frame));
                computed.collectors[sim_idx].setMetric(pm.metric_name, values[frame].get<double>());
                computed.collectors[sim_idx].endFrame();
            }
        }
        computed.sims_completed.store(simulations.size());

        computed_configs_out.push_back(std::move(computed));
    }

    std::cout << "  Loaded " << param_metrics_out.size() << " metric configs x "
              << simulations.size() << " simulations\n";
    return true;
}

// ============================================================================
// MAIN
// ============================================================================

void printUsage(char const* prog) {
    std::cerr
        << "Usage: " << prog << " annotations.json [options]\n\n"
        << "Multi-phase optimization for metric parameters and detection methods.\n\n"
        << "Options:\n"
        << "  --grid-steps <N>     Grid resolution per dimension (default: 8)\n"
        << "                       Use 3-4 for quick tests, 12-16 for thorough search\n"
        << "  --primary <target>   Primary target for metric optimization (default: first frame target)\n"
        << "  --output <file>      Output file for best parameters (default: best_params.toml)\n"
        << "  --save-metrics <file>  Save Phase 1 computed metrics to JSON file\n"
        << "  --load-metrics <file>  Load Phase 1 metrics from file (skips computation)\n"
        << "  --help               Show this help message\n\n"
        << "Optimization Phases:\n"
        << "  Phase 1: Compute metrics for all parameter configurations\n"
        << "  Phase 2: Optimize primary target to find best metric parameters\n"
        << "  Phase 3: Optimize secondary targets using primary's metric parameters\n\n"
        << "Metric Caching:\n"
        << "  Phase 1 is expensive (computes metrics for all configs x simulations).\n"
        << "  Use --save-metrics to cache results, then --load-metrics to reuse them.\n"
        << "  This allows fast iteration on detection methods without recomputing metrics.\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // Parse command line options
    std::string annotations_path;
    std::string output_file = "best_params.toml";
    std::string primary_target_arg;  // Empty = auto-detect
    std::string save_metrics_path;   // Path to save Phase 1 metrics
    std::string load_metrics_path;   // Path to load Phase 1 metrics
    int grid_steps = 8;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--grid-steps" && i + 1 < argc) {
            grid_steps = std::stoi(argv[++i]);
            if (grid_steps < 1) grid_steps = 1;
            if (grid_steps > 64) grid_steps = 64;
        } else if ((arg == "--primary" || arg == "-p") && i + 1 < argc) {
            primary_target_arg = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--save-metrics" && i + 1 < argc) {
            save_metrics_path = argv[++i];
        } else if (arg == "--load-metrics" && i + 1 < argc) {
            load_metrics_path = argv[++i];
        } else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        } else if (annotations_path.empty()) {
            annotations_path = arg;
        }
    }

    if (annotations_path.empty()) {
        std::cerr << "Error: annotations.json path required\n";
        printUsage(argv[0]);
        return 1;
    }

    // ========================================
    // Load annotations with target definitions
    // ========================================
    auto ann_data = loadAnnotations(annotations_path);
    if (ann_data.annotations.empty()) {
        std::cerr << "No valid annotations found.\n";
        return 1;
    }

    std::cout << "Loaded " << ann_data.annotations.size() << " annotations (v"
              << ann_data.version << ")\n";

    // ========================================
    // Print target statistics
    // ========================================
    std::cout << "\n=== Target Definitions ===\n";
    std::cout << std::left << std::setw(20) << "Target" << std::setw(10) << "Type"
              << "Annotations\n";
    std::cout << std::string(45, '-') << "\n";

    for (auto const& td : ann_data.target_defs) {
        int count = ann_data.countAnnotationsWithTarget(td.name);
        std::cout << std::left << std::setw(20) << td.name
                  << std::setw(10) << td.typeString()
                  << count << "\n";
    }
    std::cout << "\n";

    // Determine primary target
    std::string primary_target = primary_target_arg;
    if (primary_target.empty()) {
        primary_target = ann_data.firstFrameTarget();
    }
    if (primary_target.empty()) {
        std::cerr << "Error: No frame-type target found for primary optimization.\n";
        return 1;
    }

    auto const* primary_td = ann_data.findTargetDef(primary_target);
    if (!primary_td) {
        std::cerr << "Error: Primary target '" << primary_target << "' not found in target_defs.\n";
        return 1;
    }
    if (!primary_td->isFrame()) {
        std::cerr << "Error: Primary target must be a frame-type target.\n";
        return 1;
    }

    std::cout << "Primary target: " << primary_target << " (determines metric parameters)\n\n";

    // ========================================
    // Collect frame targets (skip score targets)
    // ========================================
    std::vector<TargetDef> frame_targets;
    for (auto const& td : ann_data.target_defs) {
        if (td.isFrame()) {
            frame_targets.push_back(td);
        }
    }

    if (frame_targets.empty()) {
        std::cerr << "No frame-type targets found.\n";
        return 1;
    }

    // ========================================
    // Load simulations (any sim with ANY frame target)
    // ========================================
    std::vector<LoadedSimulation> simulations;
    size_t total_frames = 0;
    size_t max_pendulums = 0;

    std::cout << "Loading simulations...\n";
    for (auto const& ann : ann_data.annotations) {
        if (!std::filesystem::exists(ann.data_path)) {
            std::cerr << "  Skipping " << ann.id << ": file not found: " << ann.data_path << "\n";
            continue;
        }

        // Check if this sim has ANY frame target
        bool has_frame_target = false;
        std::vector<std::string> sim_targets;
        for (auto const& td : frame_targets) {
            if (ann.hasTarget(td.name)) {
                has_frame_target = true;
                sim_targets.push_back(td.name);
            }
        }

        if (!has_frame_target) {
            std::cerr << "  Skipping " << ann.id << ": no frame targets\n";
            continue;
        }

        LoadedSimulation sim;
        if (sim.load(ann)) {
            auto const& h = sim.reader.header();
            total_frames += h.frame_count;
            max_pendulums = std::max(max_pendulums, static_cast<size_t>(h.pendulum_count));

            // Print which targets this sim has
            std::ostringstream targets_str;
            for (size_t i = 0; i < sim_targets.size(); ++i) {
                if (i > 0) targets_str << ", ";
                targets_str << sim_targets[i] << "@" << sim.getFrameTruth(sim_targets[i]);
            }
            std::cout << "  " << ann.id << ": " << h.frame_count << " frames, "
                      << h.pendulum_count << " pendulums [" << targets_str.str() << "]\n";
            simulations.push_back(std::move(sim));
        } else {
            std::cerr << "  FAILED: " << ann.data_path << "\n";
        }
    }

    if (simulations.empty()) {
        std::cerr << "No simulations loaded successfully.\n";
        return 1;
    }

    // Check that primary target has at least some sims
    int primary_sim_count = 0;
    for (auto const& sim : simulations) {
        if (sim.hasTruth(primary_target)) primary_sim_count++;
    }
    if (primary_sim_count == 0) {
        std::cerr << "Error: No simulations have ground truth for primary target '" << primary_target << "'.\n";
        return 1;
    }
    std::cout << "\nLoaded " << simulations.size() << " simulations ("
              << primary_sim_count << " with primary target '" << primary_target << "')\n";

    int N = static_cast<int>(max_pendulums);

    // ========================================
    // Build metric schemas and configurations
    // ========================================
    auto schemas = buildMetricSchemas();
    auto param_metrics = generateParameterizedMetrics(schemas, grid_steps, N);

    std::cout << "\n=== Grid Search Configuration ===\n";
    std::cout << "Grid steps: " << grid_steps << " per dimension\n";
    std::cout << "Simulations: " << simulations.size() << " (" << total_frames << " total frames)\n";
    std::cout << "Max pendulums: " << N << "\n\n";

    std::map<std::string, int> metric_counts;
    for (auto const& pm : param_metrics) {
        metric_counts[pm.metric_name]++;
    }
    std::cout << "Metric configurations (" << param_metrics.size() << " total):\n";
    for (auto const& [name, count] : metric_counts) {
        std::cout << "  " << name << ": " << count << "\n";
    }

    auto method_grid = BoomMethodGrid::create(grid_steps);
    size_t total_evals = param_metrics.size() * method_grid.totalMethods();

    std::cout << "\nDetection methods: " << method_grid.totalMethods() << "\n";
    std::cout << "Total evaluations: " << total_evals << "\n\n";

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    std::cout << "Threads: " << num_threads << "\n\n";

    // ========================================
    // PHASE 1 & 2: Compute metrics and optimize primary target
    // ========================================
    std::cout << "=== Phase 1 & 2: Optimizing Primary Target (" << primary_target << ") ===\n";

    auto start_time = std::chrono::steady_clock::now();
    std::vector<EvaluationResult> results;
    size_t num_sims = simulations.size();
    std::vector<ComputedMetricsForConfig> config_states(param_metrics.size());
    bool metrics_loaded_from_cache = false;

    // Try loading from cache if requested
    if (!load_metrics_path.empty()) {
        std::cout << "\n--- Attempting to load metrics from cache ---\n";
        if (loadMetricsCache(load_metrics_path, param_metrics, config_states, simulations, annotations_path, grid_steps)) {
            metrics_loaded_from_cache = true;
            std::cout << "Successfully loaded metrics from cache.\n";
        } else {
            std::cerr << "Failed to load from cache, will compute fresh.\n";
            // Reinitialize config_states since load may have partially modified them
            config_states.clear();
            config_states.resize(param_metrics.size());
        }
    }

    // Compute metrics if not loaded from cache
    if (!metrics_loaded_from_cache) {
        std::cout << "\n--- Computing metrics ---\n";

        for (size_t i = 0; i < param_metrics.size(); ++i) {
            config_states[i].init(num_sims);
        }

        std::atomic<size_t> metrics_completed{0};
        std::atomic<bool> done{false};

        std::thread progress_thread([&]() {
            while (!done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                size_t c = metrics_completed.load();
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - start_time).count();
                double rate = c > 0 ? c / elapsed : 0;
                double eta = rate > 0 ? (param_metrics.size() - c) / rate : 0;

                std::cout << "\rComputing: " << c << "/" << param_metrics.size()
                          << " metrics (" << std::fixed << std::setprecision(1)
                          << (100.0 * c / param_metrics.size()) << "%)"
                          << " | " << std::setprecision(1) << elapsed << "s"
                          << " | ETA: " << std::setprecision(0) << eta << "s     " << std::flush;
            }
        });

        size_t total_work_items = param_metrics.size() * num_sims;
        std::atomic<size_t> work_idx{0};

        std::vector<std::thread> workers;
        for (unsigned int t = 0; t < num_threads; ++t) {
            workers.emplace_back([&]() {
                while (true) {
                    size_t idx = work_idx.fetch_add(1);
                    if (idx >= total_work_items) break;

                    size_t config_idx = idx / num_sims;
                    size_t sim_idx = idx % num_sims;

                    auto const& pm = param_metrics[config_idx];
                    auto const& sim = simulations[sim_idx];
                    auto& state = config_states[config_idx];

                    computeMetricsForSim(
                        pm, sim,
                        state.collectors[sim_idx],
                        state.frame_durations[sim_idx],
                        state.sim_ids[sim_idx]);

                    if (state.sims_completed.fetch_add(1) + 1 == num_sims) {
                        metrics_completed.fetch_add(1);
                    }
                }
            });
        }

        for (auto& w : workers) {
            w.join();
        }

        done.store(true);
        progress_thread.join();
        std::cout << "\n";

        // Save metrics cache if requested
        if (!save_metrics_path.empty()) {
            std::cout << "\n--- Saving metrics to cache ---\n";
            saveMetricsCache(save_metrics_path, param_metrics, config_states, simulations, annotations_path, grid_steps);
        }
    }

    // Evaluate all detection methods
    std::cout << "\n--- Evaluating detection methods ---\n";
    std::atomic<size_t> eval_completed{0};
    std::atomic<bool> eval_done{false};
    auto eval_start = std::chrono::steady_clock::now();

    std::thread eval_progress([&]() {
        while (!eval_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            size_t c = eval_completed.load();
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - eval_start).count();
            double rate = c > 0 ? c / elapsed : 0;
            double eta = rate > 0 ? (param_metrics.size() - c) / rate : 0;

            std::cout << "\rEvaluating: " << c << "/" << param_metrics.size()
                      << " configs (" << std::fixed << std::setprecision(1)
                      << (100.0 * c / param_metrics.size()) << "%)"
                      << " | " << std::setprecision(1) << elapsed << "s"
                      << " | ETA: " << std::setprecision(0) << eta << "s     " << std::flush;
        }
    });

    std::mutex results_mutex;

    auto evaluateConfig = [&](size_t config_idx) {
        auto const& pm = param_metrics[config_idx];
        auto const& computed = config_states[config_idx];
        std::vector<EvaluationResult> local_results;

        auto evaluateMethod = [&](optimize::FrameDetectionParams const& bp) {
            auto result = evaluateFrameTarget(pm, computed, bp, simulations, primary_target);
            local_results.push_back(result);
        };

        // MaxValue
        for (double offset : method_grid.offset_vals) {
            optimize::FrameDetectionParams bp;
            bp.metric_name = pm.metric_name;
            bp.method = optimize::FrameDetectionMethod::MaxValue;
            bp.offset_seconds = offset;
            evaluateMethod(bp);
        }

        // FirstPeakPercent
        for (double pct : method_grid.peak_pct_vals) {
            for (double offset : method_grid.offset_vals) {
                for (double prom : method_grid.prominence_vals) {
                    optimize::FrameDetectionParams bp;
                    bp.metric_name = pm.metric_name;
                    bp.method = optimize::FrameDetectionMethod::FirstPeakPercent;
                    bp.peak_percent_threshold = pct;
                    bp.offset_seconds = offset;
                    bp.min_peak_prominence = prom;
                    evaluateMethod(bp);
                }
            }
        }

        // DerivativePeak
        for (int smooth : method_grid.smooth_vals) {
            for (double offset : method_grid.offset_vals) {
                optimize::FrameDetectionParams bp;
                bp.metric_name = pm.metric_name;
                bp.method = optimize::FrameDetectionMethod::DerivativePeak;
                bp.smoothing_window = smooth;
                bp.offset_seconds = offset;
                evaluateMethod(bp);
            }
        }

        // ThresholdCrossing
        for (double thresh : method_grid.crossing_thresh_vals) {
            for (int confirm : method_grid.crossing_confirm_vals) {
                for (double offset : method_grid.offset_vals) {
                    optimize::FrameDetectionParams bp;
                    bp.metric_name = pm.metric_name;
                    bp.method = optimize::FrameDetectionMethod::ThresholdCrossing;
                    bp.crossing_threshold = thresh;
                    bp.crossing_confirmation = confirm;
                    bp.offset_seconds = offset;
                    evaluateMethod(bp);
                }
            }
        }

        // SecondDerivativePeak
        for (int smooth : method_grid.smooth_vals) {
            for (double offset : method_grid.offset_vals) {
                optimize::FrameDetectionParams bp;
                bp.metric_name = pm.metric_name;
                bp.method = optimize::FrameDetectionMethod::SecondDerivativePeak;
                bp.smoothing_window = smooth;
                bp.offset_seconds = offset;
                evaluateMethod(bp);
            }
        }

        {
            std::lock_guard<std::mutex> lock(results_mutex);
            results.insert(results.end(), local_results.begin(), local_results.end());
        }
        config_states[config_idx].reset();
        eval_completed.fetch_add(1);
    };

    // Parallel evaluation
    std::atomic<size_t> eval_idx{0};
    std::vector<std::thread> eval_workers;
    for (unsigned int t = 0; t < num_threads; ++t) {
        eval_workers.emplace_back([&]() {
            while (true) {
                size_t idx = eval_idx.fetch_add(1);
                if (idx >= param_metrics.size()) break;
                evaluateConfig(idx);
            }
        });
    }

    for (auto& w : eval_workers) {
        w.join();
    }

    eval_done.store(true);
    eval_progress.join();
    std::cout << "\n";

    auto end_time = std::chrono::steady_clock::now();
    double total_secs = std::chrono::duration<double>(end_time - start_time).count();

    std::cout << "\nCompleted in " << std::fixed << std::setprecision(2) << total_secs << "s"
              << " (" << std::setprecision(0) << (results.size() / total_secs) << " evals/sec)\n\n";

    // Sort by MAE
    std::sort(results.begin(), results.end(),
              [](auto const& a, auto const& b) { return a.combined_score < b.combined_score; });

    // ========================================
    // RESULTS FOR PRIMARY TARGET
    // ========================================
    std::cout << std::string(100, '=') << "\n";
    std::cout << "PRIMARY TARGET: " << primary_target << "\n";
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

    // Winner
    if (results.empty()) {
        std::cerr << "No results for primary target.\n";
        return 1;
    }

    auto const& primary_winner = results[0];
    std::cout << "PRIMARY TARGET WINNER\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << "  Metric: " << primary_winner.params.boom.metric_name << "\n";
    std::cout << "  MAE: " << std::fixed << std::setprecision(2) << primary_winner.boom_mae << " frames\n";
    std::cout << "  Median: " << primary_winner.boom_median << " frames\n";
    std::cout << "  StdDev: " << primary_winner.boom_stddev << " frames\n";
    std::cout << "  Max: " << static_cast<int>(primary_winner.boom_max) << " frames\n";
    std::cout << "  Samples: " << primary_winner.samples_evaluated << "\n";

    if (!primary_winner.per_sim_errors.empty() && primary_winner.per_sim_errors.size() == simulations.size()) {
        std::cout << "\n  Per-simulation errors:\n";
        for (size_t i = 0; i < simulations.size(); ++i) {
            int truth = simulations[i].getFrameTruth(primary_target);
            std::cout << "    " << std::left << std::setw(30) << simulations[i].id
                      << std::right << " error=" << std::setw(4) << primary_winner.per_sim_errors[i]
                      << " (truth=" << truth << ")\n";
        }
    }
    std::cout << std::string(100, '-') << "\n\n";

    // ========================================
    // Build optimized targets map
    // ========================================
    std::map<std::string, OptimizedTarget> optimized_targets;

    // Add primary target
    {
        OptimizedTarget opt;
        opt.name = primary_target;
        opt.type = optimize::PredictionType::Frame;
        opt.frame_result = primary_winner;
        optimized_targets[primary_target] = opt;
    }

    // ========================================
    // PHASE 3: Optimize secondary frame targets
    // ========================================
    std::string best_metric_name = primary_winner.params.boom.metric_name;
    ::MetricConfig best_metric_config = primary_winner.params.metric_config;

    // Find secondary frame targets (skip score targets)
    std::vector<TargetDef> secondary_frame_targets;
    for (auto const& td : frame_targets) {
        if (td.name != primary_target) {
            // Count sims with this target
            int sim_count = 0;
            for (auto const& sim : simulations) {
                if (sim.hasTruth(td.name)) sim_count++;
            }
            if (sim_count > 0) {
                secondary_frame_targets.push_back(td);
            }
        }
    }

    if (!secondary_frame_targets.empty()) {
        std::cout << "=== Phase 3: Optimizing Secondary Frame Targets ===\n";
        std::cout << "Using metric: " << best_metric_name << " (from primary)\n";
        std::cout << "Detection methods: " << method_grid.totalMethods() << "\n\n";

        // Create the best metric config as a ParameterizedMetric
        ParameterizedMetric best_pm;
        best_pm.metric_name = best_metric_name;
        best_pm.config = best_metric_config;

        // Compute metrics once for all simulations using the best config
        std::cout << "Computing metrics for all simulations...\n";
        ComputedMetricsForConfig computed;
        computed.init(simulations.size());

        for (size_t i = 0; i < simulations.size(); ++i) {
            computeMetricsForSim(best_pm, simulations[i],
                                 computed.collectors[i],
                                 computed.frame_durations[i],
                                 computed.sim_ids[i]);
        }
        std::cout << "Done.\n\n";

        // Optimize each secondary frame target
        for (auto const& td : secondary_frame_targets) {
            int sim_count = 0;
            for (auto const& sim : simulations) {
                if (sim.hasTruth(td.name)) sim_count++;
            }

            std::cout << "--- " << td.name << " (" << sim_count << " simulations) ---\n";

            // Evaluate all detection methods
            std::vector<EvaluationResult> target_results;

            auto evaluateMethod = [&](optimize::FrameDetectionParams const& bp) {
                auto result = evaluateFrameTarget(best_pm, computed, bp, simulations, td.name);
                if (result.samples_evaluated > 0) {
                    target_results.push_back(result);
                }
            };

            // MaxValue
            for (double offset : method_grid.offset_vals) {
                optimize::FrameDetectionParams bp;
                bp.metric_name = best_metric_name;
                bp.method = optimize::FrameDetectionMethod::MaxValue;
                bp.offset_seconds = offset;
                evaluateMethod(bp);
            }

            // FirstPeakPercent
            for (double pct : method_grid.peak_pct_vals) {
                for (double offset : method_grid.offset_vals) {
                    for (double prom : method_grid.prominence_vals) {
                        optimize::FrameDetectionParams bp;
                        bp.metric_name = best_metric_name;
                        bp.method = optimize::FrameDetectionMethod::FirstPeakPercent;
                        bp.peak_percent_threshold = pct;
                        bp.offset_seconds = offset;
                        bp.min_peak_prominence = prom;
                        evaluateMethod(bp);
                    }
                }
            }

            // DerivativePeak
            for (int smooth : method_grid.smooth_vals) {
                for (double offset : method_grid.offset_vals) {
                    optimize::FrameDetectionParams bp;
                    bp.metric_name = best_metric_name;
                    bp.method = optimize::FrameDetectionMethod::DerivativePeak;
                    bp.smoothing_window = smooth;
                    bp.offset_seconds = offset;
                    evaluateMethod(bp);
                }
            }

            // ThresholdCrossing
            for (double thresh : method_grid.crossing_thresh_vals) {
                for (int confirm : method_grid.crossing_confirm_vals) {
                    for (double offset : method_grid.offset_vals) {
                        optimize::FrameDetectionParams bp;
                        bp.metric_name = best_metric_name;
                        bp.method = optimize::FrameDetectionMethod::ThresholdCrossing;
                        bp.crossing_threshold = thresh;
                        bp.crossing_confirmation = confirm;
                        bp.offset_seconds = offset;
                        evaluateMethod(bp);
                    }
                }
            }

            // SecondDerivativePeak
            for (int smooth : method_grid.smooth_vals) {
                for (double offset : method_grid.offset_vals) {
                    optimize::FrameDetectionParams bp;
                    bp.metric_name = best_metric_name;
                    bp.method = optimize::FrameDetectionMethod::SecondDerivativePeak;
                    bp.smoothing_window = smooth;
                    bp.offset_seconds = offset;
                    evaluateMethod(bp);
                }
            }

            // Find best result for this target
            if (!target_results.empty()) {
                std::sort(target_results.begin(), target_results.end(),
                          [](auto const& a, auto const& b) { return a.boom_mae < b.boom_mae; });

                auto const& best = target_results[0];

                // Print top 5
                std::cout << "  Top 5 methods:\n";
                for (size_t i = 0; i < std::min(target_results.size(), size_t(5)); ++i) {
                    auto const& r = target_results[i];
                    std::cout << "    " << (i + 1) << ". MAE=" << std::fixed << std::setprecision(1)
                              << r.boom_mae << " | " << r.params.describeFull() << "\n";
                }

                std::cout << "  Winner: MAE=" << std::fixed << std::setprecision(2) << best.boom_mae
                          << " frames, " << best.samples_evaluated << " samples\n";

                // Print per-sim errors
                if (!best.per_sim_errors.empty()) {
                    std::cout << "  Per-simulation errors:\n";
                    size_t err_idx = 0;
                    for (size_t i = 0; i < simulations.size() && err_idx < best.per_sim_errors.size(); ++i) {
                        if (simulations[i].hasTruth(td.name)) {
                            int truth = simulations[i].getFrameTruth(td.name);
                            std::cout << "    " << simulations[i].id << ": error="
                                      << best.per_sim_errors[err_idx] << " (truth=" << truth << ")\n";
                            err_idx++;
                        }
                    }
                }

                // Store optimized target
                OptimizedTarget opt;
                opt.name = td.name;
                opt.type = optimize::PredictionType::Frame;
                opt.frame_result = best;
                optimized_targets[td.name] = opt;
            } else {
                std::cout << "  No valid results (no simulations with ground truth?)\n";
            }
            std::cout << "\n";
        }
    }

    // ========================================
    // PHASE 4: Optimize score targets
    // ========================================
    std::vector<TargetDef> score_targets;
    for (auto const& td : ann_data.target_defs) {
        if (td.isScore()) {
            // Count sims with this target
            int sim_count = 0;
            for (auto const& sim : simulations) {
                if (sim.hasTruth(td.name)) sim_count++;
            }
            if (sim_count > 0) {
                score_targets.push_back(td);
            }
        }
    }

    if (!score_targets.empty()) {
        std::cout << "=== Phase 4: Optimizing Score Targets ===\n";
        std::cout << "Using metric: " << best_metric_name << " (from primary)\n";

        // Get score methods from registry
        auto score_predictor_defs = optimize::getScorePredictors();
        std::cout << "Score methods (" << score_predictor_defs.size() << " from registry): ";
        for (size_t i = 0; i < score_predictor_defs.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << score_predictor_defs[i]->name;
        }
        std::cout << "\n\n";

        // Use the same computed metrics from Phase 3
        ParameterizedMetric best_pm;
        best_pm.metric_name = best_metric_name;
        best_pm.config = best_metric_config;

        ComputedMetricsForConfig computed;
        computed.init(simulations.size());

        std::cout << "Computing metrics for all simulations...\n";
        for (size_t i = 0; i < simulations.size(); ++i) {
            computeMetricsForSim(best_pm, simulations[i],
                                 computed.collectors[i],
                                 computed.frame_durations[i],
                                 computed.sim_ids[i]);
        }
        std::cout << "Done.\n\n";

        // Get boom frames for reference (from primary target or first frame target)
        std::vector<int> reference_frames(simulations.size(), -1);
        for (size_t i = 0; i < simulations.size(); ++i) {
            // Try to get boom frame from the primary target prediction
            optimize::FrameDetectionParams bp = primary_winner.params.boom;
            bp.metric_name = best_metric_name;
            optimize::FrameDetector detector(bp);
            auto detection = detector.detect(computed.collectors[i], computed.frame_durations[i]);
            if (detection.valid()) {
                reference_frames[i] = detection.frame;
            }
        }

        for (auto const& td : score_targets) {
            int sim_count = 0;
            for (auto const& sim : simulations) {
                if (sim.hasTruth(td.name)) sim_count++;
            }

            std::cout << "--- " << td.name << " (" << sim_count << " simulations) ---\n";

            std::vector<ScoreEvaluationResult> target_results;

            // Helper lambda to evaluate a score params configuration
            auto evaluateScoreParams = [&](optimize::ScoreParams const& sp) {
                optimize::ScorePredictor predictor(sp);

                std::vector<double> errors;
                for (size_t i = 0; i < simulations.size(); ++i) {
                    if (!simulations[i].hasTruth(td.name)) continue;

                    double truth = simulations[i].getScoreTruth(td.name);
                    if (truth < 0.0) continue;  // Invalid truth

                    auto prediction = predictor.predict(
                        computed.collectors[i],
                        reference_frames[i],
                        computed.frame_durations[i]);

                    double error = std::abs(prediction.score - truth);
                    errors.push_back(error);
                }

                if (!errors.empty()) {
                    ScoreEvaluationResult result;
                    result.params = sp;
                    result.samples_evaluated = static_cast<int>(errors.size());
                    result.per_sim_errors = errors;

                    // Compute MAE
                    double sum = 0.0;
                    for (double e : errors) sum += e;
                    result.mae = sum / errors.size();

                    // Compute median
                    std::vector<double> sorted_errors = errors;
                    std::sort(sorted_errors.begin(), sorted_errors.end());
                    result.median = sorted_errors[sorted_errors.size() / 2];

                    // Compute max
                    result.max_error = *std::max_element(errors.begin(), errors.end());

                    target_results.push_back(result);
                }
            };

            // Parameter grids for score methods
            std::vector<double> window_vals = {0.5, 1.0, 2.0, 3.0};        // seconds
            std::vector<double> decay_frac_vals = {0.1, 0.3, 0.5, 0.7};    // fraction of post-peak
            std::vector<double> smooth_scale_vals = {1000.0, 5000.0, 10000.0, 50000.0};
            std::vector<double> gradient_scale_vals = {10.0, 50.0, 100.0, 500.0};

            // Try all score methods from registry
            for (auto const* pred_def : score_predictor_defs) {
                optimize::ScoreMethod method = optimize::parseScoreMethod(pred_def->name);

                // Methods with window_seconds parameter
                if (method == optimize::ScoreMethod::PreBoomContrast ||
                    method == optimize::ScoreMethod::BoomSteepness) {
                    for (double window : window_vals) {
                        optimize::ScoreParams sp;
                        sp.metric_name = best_metric_name;
                        sp.method = method;
                        sp.window_seconds = window;
                        evaluateScoreParams(sp);
                    }
                }
                // Methods with decay_window_fraction parameter
                else if (method == optimize::ScoreMethod::DecayRate) {
                    for (double frac : decay_frac_vals) {
                        optimize::ScoreParams sp;
                        sp.metric_name = best_metric_name;
                        sp.method = method;
                        sp.decay_window_fraction = frac;
                        evaluateScoreParams(sp);
                    }
                }
                // Methods with smoothness_scale parameter
                else if (method == optimize::ScoreMethod::Smoothness) {
                    for (double scale : smooth_scale_vals) {
                        optimize::ScoreParams sp;
                        sp.metric_name = best_metric_name;
                        sp.method = method;
                        sp.smoothness_scale = scale;
                        evaluateScoreParams(sp);
                    }
                }
                // Methods with gradient_scale parameter
                else if (method == optimize::ScoreMethod::BuildupGradient) {
                    for (double scale : gradient_scale_vals) {
                        optimize::ScoreParams sp;
                        sp.metric_name = best_metric_name;
                        sp.method = method;
                        sp.gradient_scale = scale;
                        evaluateScoreParams(sp);
                    }
                }
                // Non-parametric methods
                else {
                    optimize::ScoreParams sp;
                    sp.metric_name = best_metric_name;
                    sp.method = method;
                    evaluateScoreParams(sp);
                }
            }  // for pred_def

            // Find best result for this target
            if (!target_results.empty()) {
                std::sort(target_results.begin(), target_results.end(),
                          [](auto const& a, auto const& b) { return a.mae < b.mae; });

                auto const& best = target_results[0];

                // Print all methods
                std::cout << "  Methods evaluated:\n";
                for (size_t i = 0; i < target_results.size(); ++i) {
                    auto const& r = target_results[i];
                    std::cout << "    " << (i + 1) << ". MAE=" << std::fixed << std::setprecision(4)
                              << r.mae << " | " << r.describeShort() << "\n";
                }

                std::cout << "  Winner: MAE=" << std::fixed << std::setprecision(4) << best.mae
                          << ", " << best.samples_evaluated << " samples\n";

                // Print per-sim errors
                if (!best.per_sim_errors.empty()) {
                    std::cout << "  Per-simulation errors:\n";
                    size_t err_idx = 0;
                    for (size_t i = 0; i < simulations.size() && err_idx < best.per_sim_errors.size(); ++i) {
                        if (simulations[i].hasTruth(td.name)) {
                            double truth = simulations[i].getScoreTruth(td.name);
                            std::cout << "    " << simulations[i].id << ": error="
                                      << std::fixed << std::setprecision(4) << best.per_sim_errors[err_idx]
                                      << " (truth=" << std::setprecision(2) << truth << ")\n";
                            err_idx++;
                        }
                    }
                }

                // Store optimized target
                OptimizedTarget opt;
                opt.name = td.name;
                opt.type = optimize::PredictionType::Score;
                opt.score_result = best;
                optimized_targets[td.name] = opt;
            } else {
                std::cout << "  No valid results (no simulations with ground truth?)\n";
            }
            std::cout << "\n";
        }
    }

    // ========================================
    // Save results
    // ========================================
    saveOptimizationResults(output_file, primary_target, primary_winner, results, optimized_targets);

    std::cout << std::string(100, '=') << "\n";
    return 0;
}
