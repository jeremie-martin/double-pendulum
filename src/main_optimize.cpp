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

    std::string describe() const {
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
            oss << "max off=" << boom.offset_seconds;
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
};

// Evaluation results for a parameter set
struct EvaluationResult {
    ParameterSet params;
    double boom_mae = 0.0;       // Mean absolute error for boom detection (frames)
    double peak_mae = 0.0;       // Mean absolute error for peak detection (frames)
    double combined_score = 0.0; // Combined score (lower is better)
    int samples_evaluated = 0;
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
    std::vector<int> min_sectors_vals = {2, 3, 4, 5, 6, 7, 8, 9}; // Reduced for speed
    std::vector<int> max_sectors_vals = {16, 32, 48, 64, 80, 96}; // Reduced for speed
    std::vector<int> target_vals = {20, 30, 40, 50, 60, 70, 80};  // Reduced for speed

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
                    if (bi >= boom_configs.size())
                        break;

                    auto const& boom_cfg = boom_configs[bi];
                    auto boom_params = boom_cfg.toParams();

                    double boom_error_sum = 0.0;
                    int boom_samples = 0;

                    // Evaluate against all simulations (very fast - just reads pre-computed data)
                    for (auto const& computed : computed_for_config) {
                        auto boom = evaluateBoomDetection(computed, boom_params);

                        if (computed.boom_frame_truth >= 0 && boom.frame >= 0) {
                            boom_error_sum += std::abs(boom.frame - computed.boom_frame_truth);
                            boom_samples++;
                        }
                    }

                    EvaluationResult result;
                    result.params.metrics = metric_cfg.params;
                    result.params.boom = boom_params;
                    result.boom_mae = boom_samples > 0 ? boom_error_sum / boom_samples : 1e9;
                    result.peak_mae = 1e9; // Not tracking peak for now
                    result.samples_evaluated = boom_samples;
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

    // Display top 10 results
    std::cout << "Top 10 parameter combinations:\n";
    std::cout << std::string(90, '-') << "\n";
    std::cout << std::setw(4) << "Rank" << std::setw(12) << "Boom MAE" << std::setw(12)
              << "Peak MAE" << std::setw(10) << "Score"
              << "  Parameters\n";
    std::cout << std::string(90, '-') << "\n";

    for (size_t i = 0; i < std::min(results.size(), size_t(10)); ++i) {
        auto const& r = results[i];
        std::cout << std::setw(4) << (i + 1) << std::setw(12) << std::fixed << std::setprecision(1)
                  << r.boom_mae;
        if (r.peak_mae < 1e6) {
            std::cout << std::setw(12) << std::fixed << std::setprecision(1) << r.peak_mae;
        } else {
            std::cout << std::setw(12) << "N/A";
        }
        std::cout << std::setw(10) << std::fixed << std::setprecision(1)
                  << r.boom_mae // Use boom_mae as score when no peak
                  << "  " << r.params.describe() << "\n";
    }

    std::cout << std::string(90, '-') << "\n\n";

    // Save best parameters
    if (!results.empty()) {
        saveBestParams("best_params.toml", results[0]);
    }

    return 0;
}
