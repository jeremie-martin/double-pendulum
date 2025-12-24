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
    int boom_frame = -1;           // Ground truth boom frame (-1 = unknown)
    int peak_frame = -1;           // Ground truth peak causticness frame (-1 = unknown)
    std::string notes;
};

// Parameter set being tested
struct ParameterSet {
    MetricParams metrics;
    BoomDetectionParams boom;

    std::string describe() const {
        std::ostringstream oss;
        oss << "sectors=" << metrics.min_sectors << "-" << metrics.max_sectors
            << " target=" << metrics.target_per_sector;
        switch (boom.method) {
        case BoomDetectionMethod::MaxCausticness:
            oss << " method=max offset=" << boom.offset_seconds;
            break;
        case BoomDetectionMethod::FirstPeakPercent:
            oss << " method=first_peak pct=" << boom.peak_percent_threshold;
            break;
        case BoomDetectionMethod::DerivativePeak:
            oss << " method=deriv smooth=" << boom.smoothing_window;
            break;
        }
        return oss.str();
    }
};

// Evaluation results for a parameter set
struct EvaluationResult {
    ParameterSet params;
    double boom_mae = 0.0;    // Mean absolute error for boom detection (frames)
    double peak_mae = 0.0;    // Mean absolute error for peak detection (frames)
    double combined_score = 0.0;  // Combined score (lower is better)
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

// Compute metrics for a pre-loaded simulation using given parameters
metrics::BoomDetection evaluateSimulation(
    LoadedSimulation const& sim,
    MetricParams const& metric_params,
    BoomDetectionParams const& boom_params,
    double& out_peak_frame) {

    auto const& header = sim.reader.header();
    int frame_count = header.frame_count;

    // Create metrics collector with our parameters
    metrics::MetricsCollector collector;
    collector.setMetricParams(metric_params);
    collector.registerStandardMetrics();

    // Process all frames using zero-copy packed state access
    for (int frame = 0; frame < frame_count; ++frame) {
        auto const* packed = sim.reader.getFramePacked(frame);
        if (!packed) {
            break;
        }
        collector.beginFrame(frame);
        collector.updateFromPackedStates(packed, header.pendulum_count);
        collector.endFrame();
    }

    // Find peak causticness frame (max value, no offset)
    auto const* caustic_series = collector.getMetric(boom_params.metric_name);
    if (!caustic_series || caustic_series->empty()) {
        caustic_series = collector.getMetric(metrics::MetricNames::AngularCausticness);
    }

    if (caustic_series && !caustic_series->empty()) {
        auto const& values = caustic_series->values();
        auto max_it = std::max_element(values.begin(), values.end());
        out_peak_frame = static_cast<double>(std::distance(values.begin(), max_it));
    } else {
        out_peak_frame = -1;
    }

    // Detect boom using our parameters
    return metrics::findBoomFrame(collector, sim.frame_duration, boom_params);
}

// Generate parameter sets for grid search
std::vector<ParameterSet> generateParameterGrid() {
    std::vector<ParameterSet> grid;

    // Sector variations
    std::vector<int> min_sectors_vals = {6, 8, 12};
    std::vector<int> max_sectors_vals = {48, 72, 96};
    std::vector<int> target_vals = {20, 40, 60};

    // Boom detection variations
    std::vector<double> offset_vals = {0.0, 0.15, 0.3, 0.5};
    std::vector<double> peak_pct_vals = {0.5, 0.6, 0.7, 0.8};
    std::vector<int> smooth_vals = {3, 5, 7};

    // Generate combinations
    for (int min_sec : min_sectors_vals) {
        for (int max_sec : max_sectors_vals) {
            for (int target : target_vals) {
                MetricParams mp;
                mp.min_sectors = min_sec;
                mp.max_sectors = max_sec;
                mp.target_per_sector = target;

                // MaxCausticness with different offsets
                for (double offset : offset_vals) {
                    ParameterSet ps;
                    ps.metrics = mp;
                    ps.boom.method = BoomDetectionMethod::MaxCausticness;
                    ps.boom.offset_seconds = offset;
                    grid.push_back(ps);
                }

                // FirstPeakPercent with different thresholds
                for (double pct : peak_pct_vals) {
                    ParameterSet ps;
                    ps.metrics = mp;
                    ps.boom.method = BoomDetectionMethod::FirstPeakPercent;
                    ps.boom.peak_percent_threshold = pct;
                    grid.push_back(ps);
                }

                // DerivativePeak with different smoothing
                for (int smooth : smooth_vals) {
                    ParameterSet ps;
                    ps.metrics = mp;
                    ps.boom.method = BoomDetectionMethod::DerivativePeak;
                    ps.boom.smoothing_window = smooth;
                    grid.push_back(ps);
                }
            }
        }
    }

    return grid;
}

// Save best parameters to TOML file
void saveBestParams(std::string const& path, EvaluationResult const& best) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not write to " << path << "\n";
        return;
    }

    file << "# Best parameters found by pendulum-optimize\n";
    file << "# Boom MAE: " << std::fixed << std::setprecision(2)
         << best.boom_mae << " frames\n";
    file << "# Peak MAE: " << std::fixed << std::setprecision(2)
         << best.peak_mae << " frames\n";
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
            total_pendulums = h.pendulum_count;  // Same for all
            std::cout << "  " << ann.id << ": " << h.frame_count << " frames, "
                      << h.pendulum_count << " pendulums, boom@" << ann.boom_frame
                      << " (" << std::fixed << std::setprecision(0) << load_ms << "ms)\n";
            simulations.push_back(std::move(sim));
        } else {
            std::cerr << "  FAILED: " << ann.data_path << "\n";
        }
    }

    auto load_end = std::chrono::steady_clock::now();
    double load_time = std::chrono::duration<double>(load_end - load_start).count();
    std::cout << "Loaded " << simulations.size() << " simulations in "
              << std::fixed << std::setprecision(2) << load_time << "s"
              << " (" << total_frames << " total frames, " << total_pendulums << " pendulums)\n\n";

    if (simulations.empty()) {
        std::cerr << "No simulations loaded successfully.\n";
        return 1;
    }

    // Generate parameter grid
    auto grid = generateParameterGrid();
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::cout << "Testing " << grid.size() << " parameter combinations\n";
    std::cout << "Using " << num_threads << " threads\n";
    std::cout << "Simulations: " << simulations.size() << " files, ~"
              << simulations[0].reader.header().frame_count << " frames each\n\n";

    // Pre-allocate results vector
    std::vector<EvaluationResult> results(grid.size());

    // Thread-safe progress tracking
    std::atomic<size_t> completed{0};
    std::atomic<bool> done{false};
    std::mutex print_mutex;
    auto start_time = std::chrono::steady_clock::now();

    // Progress printer thread
    std::thread progress_thread([&]() {
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            size_t c = completed.load();
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            double secs = std::chrono::duration<double>(elapsed).count();
            double rate = c > 0 ? secs / c : 0;
            double eta = (grid.size() - c) * rate;

            std::lock_guard<std::mutex> lock(print_mutex);
            std::cout << "\rProgress: " << c << "/" << grid.size()
                      << " (" << std::fixed << std::setprecision(1)
                      << (100.0 * c / grid.size()) << "%)"
                      << " | " << std::setprecision(1) << secs << "s elapsed"
                      << " | ETA: " << std::setprecision(0) << eta << "s"
                      << "     " << std::flush;
        }
    });

    // Worker function for evaluating a range of parameters
    auto evaluate_range = [&](size_t start_idx, size_t end_idx) {
        for (size_t pi = start_idx; pi < end_idx; ++pi) {
            auto const& params = grid[pi];
            EvaluationResult& result = results[pi];
            result.params = params;

            double boom_error_sum = 0.0;
            double peak_error_sum = 0.0;
            int boom_samples = 0;
            int peak_samples = 0;

            for (auto const& sim : simulations) {
                double peak_frame = -1;
                auto boom = evaluateSimulation(sim, params.metrics, params.boom, peak_frame);

                if (sim.boom_frame_truth >= 0 && boom.frame >= 0) {
                    boom_error_sum += std::abs(boom.frame - sim.boom_frame_truth);
                    boom_samples++;
                }

                if (sim.peak_frame_truth >= 0 && peak_frame >= 0) {
                    peak_error_sum += std::abs(peak_frame - sim.peak_frame_truth);
                    peak_samples++;
                }
            }

            result.boom_mae = boom_samples > 0 ? boom_error_sum / boom_samples : 1e9;
            result.peak_mae = peak_samples > 0 ? peak_error_sum / peak_samples : 1e9;
            result.samples_evaluated = boom_samples + peak_samples;
            result.combined_score = 0.7 * result.boom_mae + 0.3 * result.peak_mae;

            completed.fetch_add(1);
        }
    };

    // Launch worker threads
    std::vector<std::thread> workers;
    size_t chunk_size = (grid.size() + num_threads - 1) / num_threads;
    for (unsigned int t = 0; t < num_threads; ++t) {
        size_t start_idx = t * chunk_size;
        size_t end_idx = std::min(start_idx + chunk_size, grid.size());
        if (start_idx < grid.size()) {
            workers.emplace_back(evaluate_range, start_idx, end_idx);
        }
    }

    // Wait for all workers to complete
    for (auto& w : workers) {
        w.join();
    }

    done.store(true);
    progress_thread.join();

    auto total_time = std::chrono::steady_clock::now() - start_time;
    double total_secs = std::chrono::duration<double>(total_time).count();
    std::cout << "\rCompleted " << grid.size() << " evaluations in "
              << std::fixed << std::setprecision(2) << total_secs << "s"
              << " (" << std::setprecision(1) << (grid.size() / total_secs) << " evals/sec)\n\n";

    // Sort by combined score
    std::sort(results.begin(), results.end(), [](auto const& a, auto const& b) {
        return a.combined_score < b.combined_score;
    });

    // Display top 10 results
    std::cout << "Top 10 parameter combinations:\n";
    std::cout << std::string(90, '-') << "\n";
    std::cout << std::setw(4) << "Rank"
              << std::setw(12) << "Boom MAE"
              << std::setw(12) << "Peak MAE"
              << std::setw(10) << "Score"
              << "  Parameters\n";
    std::cout << std::string(90, '-') << "\n";

    for (size_t i = 0; i < std::min(results.size(), size_t(10)); ++i) {
        auto const& r = results[i];
        std::cout << std::setw(4) << (i + 1)
                  << std::setw(12) << std::fixed << std::setprecision(1) << r.boom_mae;
        if (r.peak_mae < 1e6) {
            std::cout << std::setw(12) << std::fixed << std::setprecision(1) << r.peak_mae;
        } else {
            std::cout << std::setw(12) << "N/A";
        }
        std::cout << std::setw(10) << std::fixed << std::setprecision(1) << r.boom_mae  // Use boom_mae as score when no peak
                  << "  " << r.params.describe() << "\n";
    }

    std::cout << std::string(90, '-') << "\n\n";

    // Save best parameters
    if (!results.empty()) {
        saveBestParams("best_params.toml", results[0]);
    }

    return 0;
}
