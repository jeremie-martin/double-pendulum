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
#include <map>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
    MetricParams metrics;
    BoomDetectionParams boom;

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
            oss << "max off=" << std::fixed << std::setprecision(1) << boom.offset_seconds;
            break;
        case BoomDetectionMethod::FirstPeakPercent:
            oss << "first@" << (int)(boom.peak_percent_threshold * 100) << "%";
            break;
        case BoomDetectionMethod::DerivativePeak:
            oss << "deriv w=" << boom.smoothing_window;
            break;
        }
        return oss.str();
    }

    // Full description including metric params
    std::string describeFull() const {
        std::ostringstream oss;
        oss << describeShort();
        oss << " [sec=" << metrics.min_sectors << "-" << metrics.max_sectors
            << " tgt=" << metrics.target_per_sector << "]";
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

// Phase 1: Compute all metrics for a simulation (expensive, done once per MetricParams)
void computeMetrics(LoadedSimulation const& sim, MetricParams const& metric_params,
                    ComputedMetrics& out) {
    auto const& header = sim.reader.header();
    int frame_count = header.frame_count;

    out.sim_id = sim.id;
    out.frame_duration = sim.frame_duration;
    out.boom_frame_truth = sim.boom_frame_truth;

    // Create metrics collector with our parameters
    out.collector.setMetricParams(metric_params);
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

// Metric parameter configuration (Phase 1 - expensive computation)
struct MetricConfig {
    MetricParams params;

    bool operator==(MetricConfig const& other) const {
        return params.min_sectors == other.params.min_sectors &&
               params.max_sectors == other.params.max_sectors &&
               params.target_per_sector == other.params.target_per_sector;
    }
};

// Boom detection configuration (Phase 2 - cheap evaluation)
struct BoomConfig {
    std::string metric_name;
    BoomDetectionMethod method;
    double offset_seconds = 0.0;
    double peak_percent_threshold = 0.6;
    int smoothing_window = 5;

    BoomDetectionParams toParams() const {
        BoomDetectionParams p;
        p.metric_name = metric_name;
        p.method = method;
        p.offset_seconds = offset_seconds;
        p.peak_percent_threshold = peak_percent_threshold;
        p.smoothing_window = smoothing_window;
        return p;
    }

    std::string describe() const {
        std::ostringstream oss;
        // Shorten metric name for display
        std::string metric_short = metric_name;
        if (metric_short.find("_causticness") != std::string::npos) {
            metric_short = metric_short.substr(0, metric_short.find("_causticness"));
        } else if (metric_short.find("_concentration") != std::string::npos) {
            metric_short = metric_short.substr(0, metric_short.find("_concentration")) + "_conc";
        } else if (metric_short.find("_coherence") != std::string::npos) {
            metric_short = metric_short.substr(0, metric_short.find("_coherence")) + "_coh";
        }
        oss << metric_short << " ";
        switch (method) {
        case BoomDetectionMethod::MaxCausticness:
            oss << "max off=" << offset_seconds;
            break;
        case BoomDetectionMethod::FirstPeakPercent:
            oss << "first@" << (int)(peak_percent_threshold * 100) << "%";
            break;
        case BoomDetectionMethod::DerivativePeak:
            oss << "deriv w=" << smoothing_window;
            break;
        }
        return oss.str();
    }
};

// Generate metric parameter configurations (Phase 1)
std::vector<MetricConfig> generateMetricConfigs() {
    std::vector<MetricConfig> configs;

    // Sector variations
    std::vector<int> min_sectors_vals = {2, 4, 6, 8}; // Reduced for speed
    std::vector<int> max_sectors_vals = {32, 64, 96}; // Reduced for speed
    std::vector<int> target_vals = {25, 50, 75};      // Reduced for speed

    for (int min_sec : min_sectors_vals) {
        for (int max_sec : max_sectors_vals) {
            for (int target : target_vals) {
                MetricConfig cfg;
                cfg.params.min_sectors = min_sec;
                cfg.params.max_sectors = max_sec;
                cfg.params.target_per_sector = target;
                configs.push_back(cfg);
            }
        }
    }

    return configs;
}

// Generate boom detection configurations (Phase 2)
std::vector<BoomConfig> generateBoomConfigs() {
    std::vector<BoomConfig> configs;

    // Metrics to try for boom detection
    std::vector<std::string> metric_names = {"angular_causticness",   "tip_causticness",
                                             "spatial_concentration", "cv_causticness",
                                             "fold_causticness",      "local_coherence"};

    // Boom detection variations
    std::vector<double> offset_vals = {-0.2, -0.1, 0.0, 0.1, 0.2, 0.3, 0.4};
    std::vector<double> peak_pct_vals = {0.4, 0.5, 0.6, 0.7, 0.8, 0.9};
    std::vector<int> smooth_vals = {3, 5, 7};

    for (auto const& metric : metric_names) {
        // MaxCausticness with different offsets
        for (double offset : offset_vals) {
            BoomConfig cfg;
            cfg.metric_name = metric;
            cfg.method = BoomDetectionMethod::MaxCausticness;
            cfg.offset_seconds = offset;
            configs.push_back(cfg);
        }

        // FirstPeakPercent with different thresholds
        for (double pct : peak_pct_vals) {
            BoomConfig cfg;
            cfg.metric_name = metric;
            cfg.method = BoomDetectionMethod::FirstPeakPercent;
            cfg.peak_percent_threshold = pct;
            configs.push_back(cfg);
        }

        // DerivativePeak with different smoothing
        for (int smooth : smooth_vals) {
            BoomConfig cfg;
            cfg.metric_name = metric;
            cfg.method = BoomDetectionMethod::DerivativePeak;
            cfg.smoothing_window = smooth;
            configs.push_back(cfg);
        }
    }

    return configs;
}

// Save best parameters to TOML file
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

    file << "[metrics]\n";
    file << "min_sectors = " << best.params.metrics.min_sectors << "\n";
    file << "max_sectors = " << best.params.metrics.max_sectors << "\n";
    file << "target_per_sector = " << best.params.metrics.target_per_sector << "\n";
    file << "cv_normalization = " << best.params.metrics.cv_normalization << "\n";
    file << "min_spread_threshold = " << best.params.metrics.min_spread_threshold << "\n";
    file << "gini_chaos_baseline = " << best.params.metrics.gini_chaos_baseline << "\n";
    file << "gini_baseline_divisor = " << best.params.metrics.gini_baseline_divisor << "\n\n";

    file << "[boom_detection]\n";
    file << "metric_name = \"" << best.params.boom.metric_name << "\"\n";
    switch (best.params.boom.method) {
    case BoomDetectionMethod::MaxCausticness:
        file << "method = \"max_causticness\"\n";
        file << "offset_seconds = " << best.params.boom.offset_seconds << "\n";
        break;
    case BoomDetectionMethod::FirstPeakPercent:
        file << "method = \"first_peak_percent\"\n";
        file << "peak_percent_threshold = " << best.params.boom.peak_percent_threshold << "\n";
        break;
    case BoomDetectionMethod::DerivativePeak:
        file << "method = \"derivative_peak\"\n";
        file << "smoothing_window = " << best.params.boom.smoothing_window << "\n";
        break;
    }
    file << "min_peak_prominence = " << best.params.boom.min_peak_prominence << "\n";

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

    // Generate configurations
    auto metric_configs = generateMetricConfigs();
    auto boom_configs = generateBoomConfigs();
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0)
        num_threads = 4;

    size_t total_combinations = metric_configs.size() * boom_configs.size();
    std::cout << "=== Two-Phase Optimization ===\n";
    std::cout << "Phase 1: " << metric_configs.size() << " metric configs × " << simulations.size()
              << " simulations (expensive)\n";
    std::cout << "Phase 2: " << boom_configs.size() << " boom detection methods (fast)\n";
    std::cout << "Total: " << total_combinations << " parameter combinations\n";
    std::cout << "Threads: " << num_threads << "\n\n";

    auto start_time = std::chrono::steady_clock::now();

    // ============================================
    // PHASE 1: Compute metrics (expensive, done once per MetricConfig)
    // ============================================
    size_t total_phase1_work = metric_configs.size() * simulations.size();
    std::cout << "Phase 1: Computing metrics (" << total_phase1_work << " work items)...\n";

    // Storage: computed_metrics[metric_config_idx][sim_idx]
    std::vector<std::vector<ComputedMetrics>> all_computed_metrics(metric_configs.size());
    for (size_t mc = 0; mc < metric_configs.size(); ++mc) {
        all_computed_metrics[mc].resize(simulations.size());
    }

    // Parallelize across ALL (metric_config, simulation) pairs
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

    // Worker threads process (metric_config, simulation) pairs
    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&]() {
            while (true) {
                size_t idx = work_idx.fetch_add(1);
                if (idx >= total_phase1_work) {
                    break;
                }

                // Decode work item: idx = mc * num_sims + sim
                size_t mc = idx / simulations.size();
                size_t sim = idx % simulations.size();

                computeMetrics(simulations[sim], metric_configs[mc].params,
                               all_computed_metrics[mc][sim]);

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
    // PHASE 2: Search boom detection methods (cheap)
    // ============================================
    std::cout << "Phase 2: Evaluating " << boom_configs.size() << " boom detection methods...\n";

    std::vector<EvaluationResult> results;
    results.reserve(total_combinations);
    std::mutex results_mutex;

    // For each metric configuration
    for (size_t mc = 0; mc < metric_configs.size(); ++mc) {
        auto const& metric_cfg = metric_configs[mc];
        auto const& computed_for_config = all_computed_metrics[mc];

        // Thread-safe progress
        std::atomic<size_t> boom_idx{0};
        std::vector<std::thread> workers;

        for (unsigned int t = 0; t < num_threads; ++t) {
            workers.emplace_back([&]() {
                while (true) {
                    size_t bi = boom_idx.fetch_add(1);
                    if (bi >= boom_configs.size()) {
                        break;
                    }

                    auto const& boom_cfg = boom_configs[bi];
                    auto boom_params = boom_cfg.toParams();

                    std::vector<int> errors;
                    errors.reserve(computed_for_config.size());

                    // Evaluate against all simulations
                    for (auto const& computed : computed_for_config) {
                        auto boom = evaluateBoomDetection(computed, boom_params);

                        if (computed.boom_frame_truth >= 0 && boom.frame >= 0) {
                            int error = std::abs(boom.frame - computed.boom_frame_truth);
                            errors.push_back(error);
                        }
                    }

                    EvaluationResult result;
                    result.params.metrics = metric_cfg.params;
                    result.params.boom = boom_params;
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
                }
            });
        }

        for (auto& w : workers)
            w.join();

        std::cout << "  MetricConfig " << (mc + 1) << "/" << metric_configs.size() << ": "
                  << boom_configs.size() << " boom methods evaluated\n";
    }

    auto phase2_done = std::chrono::steady_clock::now();
    double phase2_total = std::chrono::duration<double>(phase2_done - phase1_done).count();
    double total_secs = std::chrono::duration<double>(phase2_done - start_time).count();

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

    std::vector<std::string> metric_types = {"angular_causticness",   "tip_causticness",
                                             "spatial_concentration", "cv_causticness",
                                             "fold_causticness",      "local_coherence"};

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
    // Best MetricParams (aggregated across all boom methods)
    // ----------------------------------------
    std::cout << "BEST METRIC PARAMETERS (aggregated across boom methods)\n";
    std::cout << std::string(100, '-') << "\n";

    // Group results by MetricConfig and find average MAE
    std::map<std::tuple<int, int, int>, std::vector<double>> metric_config_maes;
    for (auto const& r : results) {
        auto key = std::make_tuple(r.params.metrics.min_sectors, r.params.metrics.max_sectors,
                                   r.params.metrics.target_per_sector);
        metric_config_maes[key].push_back(r.boom_mae);
    }

    // Compute average MAE for each MetricConfig
    std::vector<std::pair<std::tuple<int, int, int>, double>> metric_config_avg;
    for (auto const& [key, maes] : metric_config_maes) {
        double sum = 0;
        for (double m : maes) {
            sum += m;
        }
        metric_config_avg.push_back({key, sum / maes.size()});
    }

    // Sort by average MAE
    std::sort(metric_config_avg.begin(), metric_config_avg.end(),
              [](auto const& a, auto const& b) { return a.second < b.second; });

    // Show top 5
    std::cout << "  " << std::setw(20) << "MetricParams" << std::setw(12) << "Avg MAE"
              << std::setw(12) << "Best MAE" << "\n";
    for (size_t i = 0; i < std::min(metric_config_avg.size(), size_t(5)); ++i) {
        auto const& [key, avg_mae] = metric_config_avg[i];
        auto [min_sec, max_sec, target] = key;

        // Find best MAE for this config
        double best_mae = 1e9;
        for (auto const& r : results) {
            if (r.params.metrics.min_sectors == min_sec &&
                r.params.metrics.max_sectors == max_sec &&
                r.params.metrics.target_per_sector == target) {
                best_mae = std::min(best_mae, r.boom_mae);
            }
        }

        std::ostringstream cfg_str;
        cfg_str << "sec=" << min_sec << "-" << max_sec << " tgt=" << target;
        std::cout << "  " << std::setw(20) << cfg_str.str() << std::setw(12) << std::fixed
                  << std::setprecision(1) << avg_mae << std::setw(12) << std::setprecision(1)
                  << best_mae << "\n";
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
        }
        std::cout << "  MetricParams:     min_sectors=" << winner.params.metrics.min_sectors
                  << ", max_sectors=" << winner.params.metrics.max_sectors
                  << ", target=" << winner.params.metrics.target_per_sector << "\n";
        std::cout << "\n";
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
