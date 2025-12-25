// Metric stability analysis tool for double pendulum
//
// Analyzes how stable metrics are across different pendulum counts.
// This is critical for validating that probe filtering (low N) can predict
// full simulation results (high N).
//
// Usage:
//   ./pendulum-stability [options]
//
// Options:
//   --config <path>      Config file for simulation parameters
//   --counts <list>      Comma-separated pendulum counts (default: 500,1000,2000,5000,10000)
//   --frames <N>         Number of frames to simulate (default: from config)
//   --output <path>      Output CSV for detailed per-frame analysis
//   --seed <N>           Random seed for reproducibility (default: 42)
//   -h, --help           Show this help

#include "config.h"
#include "metrics/boom_detection.h"
#include "metrics/metrics_collector.h"
#include "metrics/metrics_init.h"
#include "metrics/event_detector.h"
#include "metrics/causticness_analyzer.h"
#include "optimize/frame_detector.h"
#include "optimize/prediction_target.h"
#include "optimize/target_evaluator.h"
#include "pendulum.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// STATISTICS HELPERS
// ============================================================================

struct Stats {
    double mean = 0.0;
    double stddev = 0.0;
    double min = 0.0;
    double max = 0.0;
    double cv = 0.0;  // coefficient of variation (stddev/mean)

    static Stats compute(std::vector<double> const& values) {
        Stats s;
        if (values.empty()) return s;

        s.min = *std::min_element(values.begin(), values.end());
        s.max = *std::max_element(values.begin(), values.end());

        double sum = std::accumulate(values.begin(), values.end(), 0.0);
        s.mean = sum / values.size();

        double sq_sum = 0.0;
        for (double v : values) {
            sq_sum += (v - s.mean) * (v - s.mean);
        }
        s.stddev = std::sqrt(sq_sum / values.size());

        // CV is undefined when mean is 0; use 0 or infinity based on stddev
        if (std::abs(s.mean) > 1e-10) {
            s.cv = s.stddev / std::abs(s.mean);
        } else if (s.stddev > 1e-10) {
            s.cv = std::numeric_limits<double>::infinity();
        } else {
            s.cv = 0.0;  // Both are ~0, perfectly stable
        }

        return s;
    }
};

// ============================================================================
// SIMULATION RESULTS
// ============================================================================

struct SimulationRun {
    int pendulum_count = 0;
    double duration_seconds = 0.0;
    int frame_count = 0;
    double frame_duration = 0.0;

    // Per-frame metric values
    std::map<std::string, std::vector<double>> metrics;

    // Detected targets
    int boom_frame = -1;
    double boom_seconds = -1.0;
    double boom_metric_value = 0.0;

    int chaos_frame = -1;
    double chaos_seconds = -1.0;

    // Quality scores
    double boom_quality = 0.0;
    double peak_clarity = 0.0;
    double uniformity = 0.0;
};

// ============================================================================
// CORE SIMULATION
// ============================================================================

SimulationRun runSimulation(
    Config const& config,
    int pendulum_count) {

    SimulationRun result;
    result.pendulum_count = pendulum_count;
    result.duration_seconds = config.simulation.duration_seconds;
    result.frame_count = config.simulation.total_frames;
    result.frame_duration = config.simulation.frameDuration();

    // Initialize pendulums with consistent spread
    std::vector<Pendulum> pendulums;
    pendulums.reserve(pendulum_count);

    // Initial angles are in radians in config.physics
    double const th1_center = config.physics.initial_angle1;
    double const th2_center = config.physics.initial_angle2;
    double const spread = config.simulation.angle_variation;  // Already in radians

    // Deterministic spread: evenly distribute across the spread range
    for (int i = 0; i < pendulum_count; ++i) {
        double t = pendulum_count > 1
            ? static_cast<double>(i) / (pendulum_count - 1)
            : 0.5;
        double th1 = th1_center + (t - 0.5) * spread;
        double th2 = th2_center + (t - 0.5) * spread;

        pendulums.emplace_back(
            config.physics.gravity,
            config.physics.length1,
            config.physics.length2,
            config.physics.mass1,
            config.physics.mass2,
            th1, th2,
            config.physics.initial_velocity1,
            config.physics.initial_velocity2);
    }

    // Initialize metrics
    metrics::MetricsCollector collector;
    metrics::EventDetector detector;
    metrics::CausticnessAnalyzer causticness_analyzer;

    collector.setAllMetricConfigs(config.metric_configs);
    metrics::initializeMetricsSystem(
        collector, detector, causticness_analyzer,
        result.frame_duration, /*with_gpu=*/false);

    // Physics parameters
    int const substeps = config.simulation.substeps();
    double const step_dt = result.frame_duration / substeps;

    // Simulation loop
    for (int frame = 0; frame < result.frame_count; ++frame) {
        // Physics step - returns state after step
        std::vector<PendulumState> states(pendulum_count);
        for (int sub = 0; sub < substeps; ++sub) {
            for (int i = 0; i < pendulum_count; ++i) {
                states[i] = pendulums[i].step(step_dt);
            }
        }

        // Update metrics with full state (includes positions for spatial metrics)
        collector.beginFrame(frame);
        collector.updateFromStates(states);
        collector.endFrame();

        detector.update(collector, result.frame_duration);
    }

    // Extract per-frame values for all metrics
    for (auto const& name : collector.getMetricNames()) {
        auto const* series = collector.getMetric(name);
        if (series) {
            result.metrics[name] = series->values();
        }
    }

    // Run post-simulation analysis
    optimize::FrameDetectionParams boom_params;
    for (auto const& tc : config.targets) {
        if (tc.name == "boom" && tc.type == "frame") {
            auto target = optimize::targetConfigToPredictionTarget(
                tc.name, tc.type, tc.metric, tc.method,
                tc.offset_seconds, tc.peak_percent_threshold,
                tc.min_peak_prominence, tc.smoothing_window,
                tc.crossing_threshold, tc.crossing_confirmation,
                tc.weights);
            boom_params = target.frameParams();
            break;
        }
    }

    auto boom = metrics::runPostSimulationAnalysis(
        collector, detector, causticness_analyzer,
        result.frame_duration, boom_params);

    result.boom_frame = boom.frame;
    result.boom_seconds = boom.seconds;
    result.boom_metric_value = boom.metric_value;

    if (auto chaos = detector.getEvent(metrics::EventNames::Chaos)) {
        result.chaos_frame = chaos->frame;
        result.chaos_seconds = chaos->frame * result.frame_duration;
    }

    result.uniformity = collector.getUniformity();
    if (causticness_analyzer.hasResults()) {
        result.boom_quality = causticness_analyzer.score();
        result.peak_clarity = causticness_analyzer.getMetrics().peak_clarity_score;
    }

    return result;
}

// ============================================================================
// STABILITY ANALYSIS
// ============================================================================

struct MetricStability {
    std::string name;
    double mean_cv = 0.0;      // Mean CV across all frames
    double max_cv = 0.0;       // Max CV (worst case)
    double median_cv = 0.0;    // Median CV
    int unstable_frames = 0;   // Frames with CV > 10%
    std::string stability_grade;

    void computeGrade() {
        if (mean_cv < 0.01) {
            stability_grade = "A+ (excellent)";
        } else if (mean_cv < 0.05) {
            stability_grade = "A  (good)";
        } else if (mean_cv < 0.10) {
            stability_grade = "B  (acceptable)";
        } else if (mean_cv < 0.20) {
            stability_grade = "C  (marginal)";
        } else if (mean_cv < 0.50) {
            stability_grade = "D  (poor)";
        } else {
            stability_grade = "F  (unstable)";
        }
    }
};

struct TargetStability {
    std::string name;
    std::vector<int> detected_frames;
    std::vector<double> detected_seconds;
    Stats frame_stats;
    bool all_detected = true;
};

struct StabilityReport {
    std::vector<MetricStability> metrics;
    std::vector<TargetStability> targets;
    std::vector<int> pendulum_counts;
    int total_frames;
};

StabilityReport analyzeStability(std::vector<SimulationRun> const& runs) {
    StabilityReport report;

    if (runs.empty()) return report;

    for (auto const& run : runs) {
        report.pendulum_counts.push_back(run.pendulum_count);
    }
    report.total_frames = runs[0].frame_count;

    // Collect all metric names from first run
    std::vector<std::string> metric_names;
    for (auto const& [name, _] : runs[0].metrics) {
        metric_names.push_back(name);
    }
    std::sort(metric_names.begin(), metric_names.end());

    // Analyze each metric
    for (auto const& metric_name : metric_names) {
        MetricStability ms;
        ms.name = metric_name;

        std::vector<double> frame_cvs;

        // For each frame, compute CV across different N values
        for (int frame = 0; frame < report.total_frames; ++frame) {
            std::vector<double> values_at_frame;
            for (auto const& run : runs) {
                auto it = run.metrics.find(metric_name);
                if (it != run.metrics.end() && frame < static_cast<int>(it->second.size())) {
                    values_at_frame.push_back(it->second[frame]);
                }
            }

            if (values_at_frame.size() >= 2) {
                Stats s = Stats::compute(values_at_frame);
                if (std::isfinite(s.cv)) {
                    frame_cvs.push_back(s.cv);
                    if (s.cv > 0.10) {
                        ms.unstable_frames++;
                    }
                }
            }
        }

        if (!frame_cvs.empty()) {
            Stats cv_stats = Stats::compute(frame_cvs);
            ms.mean_cv = cv_stats.mean;
            ms.max_cv = cv_stats.max;

            // Compute median CV
            std::vector<double> sorted_cvs = frame_cvs;
            std::sort(sorted_cvs.begin(), sorted_cvs.end());
            ms.median_cv = sorted_cvs[sorted_cvs.size() / 2];
        }

        ms.computeGrade();
        report.metrics.push_back(ms);
    }

    // Analyze boom frame stability
    TargetStability boom_stability;
    boom_stability.name = "boom";
    for (auto const& run : runs) {
        if (run.boom_frame >= 0) {
            boom_stability.detected_frames.push_back(run.boom_frame);
            boom_stability.detected_seconds.push_back(run.boom_seconds);
        } else {
            boom_stability.all_detected = false;
        }
    }
    if (!boom_stability.detected_frames.empty()) {
        std::vector<double> frames_d(boom_stability.detected_frames.begin(),
                                      boom_stability.detected_frames.end());
        boom_stability.frame_stats = Stats::compute(frames_d);
    }
    report.targets.push_back(boom_stability);

    // Analyze chaos frame stability
    TargetStability chaos_stability;
    chaos_stability.name = "chaos";
    for (auto const& run : runs) {
        if (run.chaos_frame >= 0) {
            chaos_stability.detected_frames.push_back(run.chaos_frame);
            chaos_stability.detected_seconds.push_back(run.chaos_seconds);
        } else {
            chaos_stability.all_detected = false;
        }
    }
    if (!chaos_stability.detected_frames.empty()) {
        std::vector<double> frames_d(chaos_stability.detected_frames.begin(),
                                      chaos_stability.detected_frames.end());
        chaos_stability.frame_stats = Stats::compute(frames_d);
    }
    report.targets.push_back(chaos_stability);

    return report;
}

// ============================================================================
// OUTPUT
// ============================================================================

void printReport(StabilityReport const& report, std::vector<SimulationRun> const& runs) {
    std::cout << "\n";
    std::cout << std::string(80, '=') << "\n";
    std::cout << "METRIC STABILITY ANALYSIS\n";
    std::cout << std::string(80, '=') << "\n\n";

    // Configuration summary
    std::cout << "Configuration:\n";
    std::cout << "  Pendulum counts: ";
    for (size_t i = 0; i < report.pendulum_counts.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << report.pendulum_counts[i];
    }
    std::cout << "\n";
    std::cout << "  Frames analyzed: " << report.total_frames << "\n";
    if (!runs.empty()) {
        std::cout << "  Duration: " << std::fixed << std::setprecision(1)
                  << runs[0].duration_seconds << "s\n";
    }
    std::cout << "\n";

    // Sort metrics by stability (best first)
    std::vector<MetricStability const*> sorted_metrics;
    for (auto const& m : report.metrics) {
        sorted_metrics.push_back(&m);
    }
    std::sort(sorted_metrics.begin(), sorted_metrics.end(),
              [](auto* a, auto* b) { return a->mean_cv < b->mean_cv; });

    // Metric stability table
    std::cout << "METRIC STABILITY (sorted by mean CV, lower is better)\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left << std::setw(30) << "Metric"
              << std::right << std::setw(10) << "Mean CV"
              << std::setw(10) << "Max CV"
              << std::setw(12) << "Unstable %"
              << "  Grade\n";
    std::cout << std::string(80, '-') << "\n";

    for (auto const* m : sorted_metrics) {
        double unstable_pct = report.total_frames > 0
            ? 100.0 * m->unstable_frames / report.total_frames
            : 0.0;

        std::cout << std::left << std::setw(30) << m->name
                  << std::right << std::fixed
                  << std::setw(9) << std::setprecision(2) << (m->mean_cv * 100) << "%"
                  << std::setw(9) << std::setprecision(2) << (m->max_cv * 100) << "%"
                  << std::setw(11) << std::setprecision(1) << unstable_pct << "%"
                  << "  " << m->stability_grade << "\n";
    }
    std::cout << std::string(80, '-') << "\n\n";

    // Target detection stability
    std::cout << "TARGET DETECTION STABILITY\n";
    std::cout << std::string(80, '-') << "\n";

    for (auto const& t : report.targets) {
        std::cout << "  " << t.name << ":\n";

        if (t.detected_frames.empty()) {
            std::cout << "    Not detected in any run\n";
            continue;
        }

        if (!t.all_detected) {
            std::cout << "    WARNING: Not detected in all runs ("
                      << t.detected_frames.size() << "/" << report.pendulum_counts.size() << ")\n";
        }

        std::cout << "    Frames: ";
        for (size_t i = 0; i < t.detected_frames.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << t.detected_frames[i];
        }
        std::cout << "\n";

        std::cout << "    Mean: " << std::fixed << std::setprecision(1) << t.frame_stats.mean
                  << " frames, StdDev: " << std::setprecision(1) << t.frame_stats.stddev
                  << " frames, Range: [" << static_cast<int>(t.frame_stats.min)
                  << ", " << static_cast<int>(t.frame_stats.max) << "]\n";

        if (t.frame_stats.stddev < 3.0) {
            std::cout << "    Grade: A+ (excellent, <3 frame variation)\n";
        } else if (t.frame_stats.stddev < 10.0) {
            std::cout << "    Grade: A  (good, <10 frame variation)\n";
        } else if (t.frame_stats.stddev < 30.0) {
            std::cout << "    Grade: B  (acceptable)\n";
        } else {
            std::cout << "    Grade: C  (variable)\n";
        }
    }
    std::cout << std::string(80, '-') << "\n\n";

    // Quality metrics comparison
    std::cout << "QUALITY METRICS BY PENDULUM COUNT\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left << std::setw(12) << "N"
              << std::right << std::setw(12) << "Uniformity"
              << std::setw(12) << "Quality"
              << std::setw(12) << "Clarity"
              << std::setw(12) << "Boom (s)\n";
    std::cout << std::string(80, '-') << "\n";

    for (auto const& run : runs) {
        std::cout << std::left << std::setw(12) << run.pendulum_count
                  << std::right << std::fixed << std::setprecision(4)
                  << std::setw(12) << run.uniformity
                  << std::setw(12) << run.boom_quality
                  << std::setw(12) << run.peak_clarity
                  << std::setw(12) << std::setprecision(2) << run.boom_seconds << "\n";
    }
    std::cout << std::string(80, '-') << "\n";

    // Compute stability of quality metrics
    std::vector<double> uniformities, qualities, clarities;
    for (auto const& run : runs) {
        uniformities.push_back(run.uniformity);
        qualities.push_back(run.boom_quality);
        clarities.push_back(run.peak_clarity);
    }

    auto u_stats = Stats::compute(uniformities);
    auto q_stats = Stats::compute(qualities);
    auto c_stats = Stats::compute(clarities);

    std::cout << std::left << std::setw(12) << "CV:"
              << std::right << std::setprecision(2)
              << std::setw(11) << (u_stats.cv * 100) << "%"
              << std::setw(11) << (q_stats.cv * 100) << "%"
              << std::setw(11) << (c_stats.cv * 100) << "%\n";
    std::cout << std::string(80, '-') << "\n\n";

    // Summary
    int excellent = 0, good = 0, acceptable = 0, poor = 0;
    for (auto const* m : sorted_metrics) {
        if (m->mean_cv < 0.01) excellent++;
        else if (m->mean_cv < 0.05) good++;
        else if (m->mean_cv < 0.10) acceptable++;
        else poor++;
    }

    std::cout << "SUMMARY\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << "  Metrics analyzed: " << report.metrics.size() << "\n";
    std::cout << "  Excellent (<1% CV): " << excellent << "\n";
    std::cout << "  Good (<5% CV): " << good << "\n";
    std::cout << "  Acceptable (<10% CV): " << acceptable << "\n";
    std::cout << "  Poor (>=10% CV): " << poor << "\n";
    std::cout << std::string(80, '=') << "\n";
}

void saveDetailedCSV(std::string const& path,
                     std::vector<SimulationRun> const& runs,
                     StabilityReport const& report) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not write to " << path << "\n";
        return;
    }

    // Header
    file << "frame";
    for (auto const& m : report.metrics) {
        for (int n : report.pendulum_counts) {
            file << "," << m.name << "_N" << n;
        }
        file << "," << m.name << "_mean," << m.name << "_cv";
    }
    file << "\n";

    // Data rows
    for (int frame = 0; frame < report.total_frames; ++frame) {
        file << frame;

        for (auto const& m : report.metrics) {
            std::vector<double> values;

            // Values at each N
            for (auto const& run : runs) {
                auto it = run.metrics.find(m.name);
                if (it != run.metrics.end() && frame < static_cast<int>(it->second.size())) {
                    double v = it->second[frame];
                    file << "," << std::fixed << std::setprecision(6) << v;
                    values.push_back(v);
                } else {
                    file << ",";
                }
            }

            // Mean and CV
            if (values.size() >= 2) {
                auto s = Stats::compute(values);
                file << "," << s.mean << "," << s.cv;
            } else {
                file << ",,";
            }
        }
        file << "\n";
    }

    std::cout << "Detailed CSV saved to: " << path << "\n";
}

// ============================================================================
// MAIN
// ============================================================================

void printUsage(char const* prog) {
    std::cout << "Metric Stability Analysis Tool\n\n"
              << "Analyzes how stable metrics are across different pendulum counts.\n"
              << "This validates that probe filtering (low N) can predict full simulation results.\n\n"
              << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --config <path>      Config file (default: config/default.toml or defaults)\n"
              << "  --counts <list>      Comma-separated pendulum counts\n"
              << "                       (default: 500,1000,2000,5000,10000)\n"
              << "  --frames <N>         Override number of frames\n"
              << "  --output <path>      Output CSV for detailed per-frame analysis\n"
              << "  --seed <N>           Random seed (default: 42)\n"
              << "  -h, --help           Show this help\n\n"
              << "Examples:\n"
              << "  # Quick test with 5 counts\n"
              << "  " << prog << " --counts 500,1000,1500,2000,2500\n\n"
              << "  # Full analysis with custom config\n"
              << "  " << prog << " --config my_config.toml --counts 1000,5000,10000,50000\n\n"
              << "  # Save detailed per-frame data\n"
              << "  " << prog << " --output stability_data.csv\n";
}

std::vector<int> parseCounts(std::string const& s) {
    std::vector<int> counts;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, ',')) {
        try {
            int n = std::stoi(token);
            if (n > 0) counts.push_back(n);
        } catch (...) {}
    }
    return counts;
}

int main(int argc, char* argv[]) {
    // Default options
    std::string config_path;
    std::vector<int> pendulum_counts = {500, 1000, 2000, 5000, 10000};
    std::string output_path;
    int frame_override = -1;
    unsigned int seed = 42;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--counts" && i + 1 < argc) {
            pendulum_counts = parseCounts(argv[++i]);
        } else if (arg == "--frames" && i + 1 < argc) {
            frame_override = std::stoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<unsigned int>(std::stoi(argv[++i]));
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (pendulum_counts.empty()) {
        std::cerr << "Error: No valid pendulum counts specified\n";
        return 1;
    }

    // Sort counts for consistent output
    std::sort(pendulum_counts.begin(), pendulum_counts.end());

    // Load config
    Config config;
    if (!config_path.empty() && fs::exists(config_path)) {
        std::cout << "Loading config: " << config_path << "\n";
        config = Config::load(config_path);
    } else if (fs::exists("config/default.toml")) {
        std::cout << "Loading config: config/default.toml\n";
        config = Config::load("config/default.toml");
    } else if (fs::exists("config/best_params.toml")) {
        std::cout << "Loading config: config/best_params.toml\n";
        config = Config::load("config/best_params.toml");
    } else {
        std::cout << "Using default configuration\n";
        config = Config::defaults();
    }

    if (frame_override > 0) {
        config.simulation.total_frames = frame_override;
    }

    // Print configuration
    std::cout << "\nStability Analysis Configuration:\n";
    std::cout << "  Initial angles: " << std::fixed << std::setprecision(1)
              << (config.physics.initial_angle1 * 180.0 / M_PI) << "°, "
              << (config.physics.initial_angle2 * 180.0 / M_PI) << "°\n";
    std::cout << "  Angle spread: " << (config.simulation.angle_variation * 180.0 / M_PI) << "°\n";
    std::cout << "  Duration: " << config.simulation.duration_seconds << "s, "
              << config.simulation.total_frames << " frames\n";
    std::cout << "  Pendulum counts: ";
    for (size_t i = 0; i < pendulum_counts.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << pendulum_counts[i];
    }
    std::cout << "\n";

    // Run simulations
    std::vector<SimulationRun> runs;
    auto start_time = std::chrono::steady_clock::now();

    (void)seed;  // Reserved for future use

    std::cout << "\nRunning simulations...\n";
    for (int n : pendulum_counts) {
        std::cout << "  N=" << std::setw(6) << n << " ... " << std::flush;
        auto sim_start = std::chrono::steady_clock::now();

        auto run = runSimulation(config, n);
        runs.push_back(std::move(run));

        auto sim_end = std::chrono::steady_clock::now();
        double sim_time = std::chrono::duration<double>(sim_end - sim_start).count();
        std::cout << std::fixed << std::setprecision(2) << sim_time << "s"
                  << " (boom@" << runs.back().boom_frame << ")\n";
    }

    auto end_time = std::chrono::steady_clock::now();
    double total_time = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << "Total simulation time: " << std::fixed << std::setprecision(1)
              << total_time << "s\n";

    // Analyze stability
    auto report = analyzeStability(runs);

    // Print report
    printReport(report, runs);

    // Save detailed CSV if requested
    if (!output_path.empty()) {
        saveDetailedCSV(output_path, runs, report);
    }

    return 0;
}
