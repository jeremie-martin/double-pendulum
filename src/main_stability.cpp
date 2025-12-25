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

// Time segment for analyzing stability at different phases
struct TimeSegment {
    std::string name;
    int start_frame;
    int end_frame;
    double cv = 0.0;
    std::string grade;

    void computeGrade() {
        if (cv < 0.01) grade = "A+";
        else if (cv < 0.05) grade = "A";
        else if (cv < 0.10) grade = "B";
        else if (cv < 0.20) grade = "C";
        else if (cv < 0.50) grade = "D";
        else grade = "F";
    }
};

// Absolute value analysis for a metric at a specific N
struct AbsoluteValueStats {
    int N = 0;
    double mean_value = 0.0;      // Mean value across all frames
    double value_at_boom = 0.0;   // Value at boom frame
    double max_value = 0.0;       // Peak value
    int max_frame = 0;            // Frame of peak value
    double deviation_from_ref = 0.0;      // Absolute deviation from highest-N
    double rel_deviation_from_ref = 0.0;  // Relative deviation (%)
    double boom_deviation = 0.0;          // Boom-time absolute deviation
    double boom_rel_deviation = 0.0;      // Boom-time relative deviation (%)
};

struct MetricStability {
    std::string name;
    double mean_cv = 0.0;      // Mean CV across all frames
    double max_cv = 0.0;       // Max CV (worst case)
    double median_cv = 0.0;    // Median CV
    int unstable_frames = 0;   // Frames with CV > 10%
    std::string stability_grade;

    // Time-segmented CV analysis
    std::vector<TimeSegment> segments;
    double cv_at_boom = 0.0;   // CV specifically at boom frame
    std::string boom_grade;

    // Per-frame CV data (for detailed analysis)
    std::vector<double> frame_cvs;

    // === NEW: Absolute value analysis ===
    std::vector<AbsoluteValueStats> abs_stats;  // One per N value
    double scale_correlation = 0.0;  // Correlation between N and mean value (-1 to 1)
    double scale_sensitivity = 0.0;  // How much values change per doubling of N (%)
    int convergence_N = 0;           // Smallest N where deviation < 5% from max-N

    // Reference values (from highest N)
    double ref_mean = 0.0;
    double ref_boom_value = 0.0;
    double ref_max_value = 0.0;

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

        // Grade at boom
        if (cv_at_boom < 0.01) boom_grade = "A+";
        else if (cv_at_boom < 0.05) boom_grade = "A";
        else if (cv_at_boom < 0.10) boom_grade = "B";
        else if (cv_at_boom < 0.20) boom_grade = "C";
        else if (cv_at_boom < 0.50) boom_grade = "D";
        else boom_grade = "F";
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
    double duration_seconds;
    int boom_frame = -1;  // Consensus boom frame (mode across runs)
};

StabilityReport analyzeStability(std::vector<SimulationRun> const& runs) {
    StabilityReport report;

    if (runs.empty()) return report;

    for (auto const& run : runs) {
        report.pendulum_counts.push_back(run.pendulum_count);
    }
    report.total_frames = runs[0].frame_count;
    report.duration_seconds = runs[0].duration_seconds;

    // Find consensus boom frame (most common among runs)
    std::map<int, int> boom_counts;
    for (auto const& run : runs) {
        if (run.boom_frame >= 0) {
            boom_counts[run.boom_frame]++;
        }
    }
    int max_count = 0;
    for (auto const& [frame, count] : boom_counts) {
        if (count > max_count) {
            max_count = count;
            report.boom_frame = frame;
        }
    }

    // Define time segments (as frame ranges)
    // Segments: Early (0-30%), Middle (30-60%), Late (60-90%), Final (90-100%)
    std::vector<std::pair<std::string, std::pair<int, int>>> segment_defs = {
        {"early",  {0, report.total_frames * 30 / 100}},
        {"middle", {report.total_frames * 30 / 100, report.total_frames * 60 / 100}},
        {"late",   {report.total_frames * 60 / 100, report.total_frames * 90 / 100}},
        {"final",  {report.total_frames * 90 / 100, report.total_frames}}
    };

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
        ms.frame_cvs.resize(report.total_frames, 0.0);

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
                    ms.frame_cvs[frame] = s.cv;
                    if (s.cv > 0.10) {
                        ms.unstable_frames++;
                    }
                }
            }
        }

        // Compute overall statistics
        std::vector<double> valid_cvs;
        for (double cv : ms.frame_cvs) {
            if (cv > 0 || ms.frame_cvs[0] == 0) {  // Include zeros only if first is zero
                valid_cvs.push_back(cv);
            }
        }

        if (!valid_cvs.empty()) {
            Stats cv_stats = Stats::compute(valid_cvs);
            ms.mean_cv = cv_stats.mean;
            ms.max_cv = cv_stats.max;

            std::vector<double> sorted_cvs = valid_cvs;
            std::sort(sorted_cvs.begin(), sorted_cvs.end());
            ms.median_cv = sorted_cvs[sorted_cvs.size() / 2];
        }

        // Compute time-segmented CV
        for (auto const& [seg_name, range] : segment_defs) {
            TimeSegment seg;
            seg.name = seg_name;
            seg.start_frame = range.first;
            seg.end_frame = range.second;

            std::vector<double> seg_cvs;
            for (int f = range.first; f < range.second && f < report.total_frames; ++f) {
                if (f < static_cast<int>(ms.frame_cvs.size())) {
                    seg_cvs.push_back(ms.frame_cvs[f]);
                }
            }

            if (!seg_cvs.empty()) {
                seg.cv = Stats::compute(seg_cvs).mean;
            }
            seg.computeGrade();
            ms.segments.push_back(seg);
        }

        // CV at boom frame (and nearby frames for robustness)
        if (report.boom_frame >= 0) {
            int boom_start = std::max(0, report.boom_frame - 5);
            int boom_end = std::min(report.total_frames, report.boom_frame + 5);
            std::vector<double> boom_cvs;
            for (int f = boom_start; f < boom_end; ++f) {
                if (f < static_cast<int>(ms.frame_cvs.size())) {
                    boom_cvs.push_back(ms.frame_cvs[f]);
                }
            }
            if (!boom_cvs.empty()) {
                ms.cv_at_boom = Stats::compute(boom_cvs).mean;
            }
        }

        // === NEW: Absolute value analysis ===
        // Compute per-N statistics
        for (size_t run_idx = 0; run_idx < runs.size(); ++run_idx) {
            auto const& run = runs[run_idx];
            auto it = run.metrics.find(metric_name);
            if (it == run.metrics.end()) continue;

            AbsoluteValueStats abs;
            abs.N = run.pendulum_count;

            // Compute mean and find max
            double sum = 0.0;
            abs.max_value = -std::numeric_limits<double>::infinity();
            for (size_t f = 0; f < it->second.size(); ++f) {
                double v = it->second[f];
                sum += v;
                if (v > abs.max_value) {
                    abs.max_value = v;
                    abs.max_frame = static_cast<int>(f);
                }
            }
            abs.mean_value = sum / it->second.size();

            // Value at boom
            if (report.boom_frame >= 0 && report.boom_frame < static_cast<int>(it->second.size())) {
                abs.value_at_boom = it->second[report.boom_frame];
            }

            ms.abs_stats.push_back(abs);
        }

        // Compute reference values (from highest N, which is last in sorted runs)
        if (!ms.abs_stats.empty()) {
            auto const& ref = ms.abs_stats.back();
            ms.ref_mean = ref.mean_value;
            ms.ref_boom_value = ref.value_at_boom;
            ms.ref_max_value = ref.max_value;

            // Compute deviations from reference for each N
            for (auto& abs : ms.abs_stats) {
                abs.deviation_from_ref = abs.mean_value - ms.ref_mean;
                if (std::abs(ms.ref_mean) > 1e-10) {
                    abs.rel_deviation_from_ref = (abs.mean_value - ms.ref_mean) / std::abs(ms.ref_mean) * 100.0;
                }
                abs.boom_deviation = abs.value_at_boom - ms.ref_boom_value;
                if (std::abs(ms.ref_boom_value) > 1e-10) {
                    abs.boom_rel_deviation = (abs.value_at_boom - ms.ref_boom_value) / std::abs(ms.ref_boom_value) * 100.0;
                }
            }

            // Compute scale correlation (Pearson correlation between log(N) and mean value)
            // Positive = increases with N, Negative = decreases with N
            if (ms.abs_stats.size() >= 3) {
                double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0, sum_y2 = 0;
                int n = static_cast<int>(ms.abs_stats.size());
                for (auto const& abs : ms.abs_stats) {
                    double x = std::log(static_cast<double>(abs.N));
                    double y = abs.mean_value;
                    sum_x += x;
                    sum_y += y;
                    sum_xy += x * y;
                    sum_x2 += x * x;
                    sum_y2 += y * y;
                }
                double denom = std::sqrt((n * sum_x2 - sum_x * sum_x) * (n * sum_y2 - sum_y * sum_y));
                if (denom > 1e-10) {
                    ms.scale_correlation = (n * sum_xy - sum_x * sum_y) / denom;
                }

                // Scale sensitivity: % change per doubling of N
                // Using linear regression slope on log(N) vs value
                double slope = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x);
                double mean_y = sum_y / n;
                if (std::abs(mean_y) > 1e-10) {
                    ms.scale_sensitivity = (slope * std::log(2.0)) / std::abs(mean_y) * 100.0;
                }
            }

            // Find convergence N (smallest N where rel deviation < 5%)
            for (auto const& abs : ms.abs_stats) {
                if (std::abs(abs.rel_deviation_from_ref) < 5.0) {
                    ms.convergence_N = abs.N;
                    break;
                }
            }
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
    std::cout << std::string(100, '=') << "\n";
    std::cout << "METRIC STABILITY ANALYSIS\n";
    std::cout << std::string(100, '=') << "\n\n";

    // Configuration summary
    std::cout << "Configuration:\n";
    std::cout << "  Pendulum counts: ";
    for (size_t i = 0; i < report.pendulum_counts.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << report.pendulum_counts[i];
    }
    std::cout << "\n";
    std::cout << "  Frames analyzed: " << report.total_frames << "\n";
    std::cout << "  Duration: " << std::fixed << std::setprecision(1)
              << report.duration_seconds << "s\n";
    if (report.boom_frame >= 0) {
        double boom_sec = report.boom_frame * report.duration_seconds / report.total_frames;
        std::cout << "  Boom frame: " << report.boom_frame
                  << " (" << std::setprecision(2) << boom_sec << "s)\n";
    }
    std::cout << "\n";

    // Sort metrics by stability (best first)
    std::vector<MetricStability const*> sorted_metrics;
    for (auto const& m : report.metrics) {
        sorted_metrics.push_back(&m);
    }
    std::sort(sorted_metrics.begin(), sorted_metrics.end(),
              [](auto* a, auto* b) { return a->mean_cv < b->mean_cv; });

    // Metric stability table with time segments
    std::cout << "METRIC STABILITY BY TIME SEGMENT (CV%, lower is better)\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << std::left << std::setw(28) << "Metric"
              << std::right
              << std::setw(10) << "Early"
              << std::setw(10) << "Middle"
              << std::setw(10) << "Late"
              << std::setw(10) << "Final"
              << std::setw(10) << "@Boom"
              << std::setw(10) << "Overall"
              << "  Grade\n";
    std::cout << std::left << std::setw(28) << ""
              << std::right
              << std::setw(10) << "(0-30%)"
              << std::setw(10) << "(30-60%)"
              << std::setw(10) << "(60-90%)"
              << std::setw(10) << "(90-100%)"
              << std::setw(10) << "(±5frm)"
              << std::setw(10) << ""
              << "\n";
    std::cout << std::string(100, '-') << "\n";

    for (auto const* m : sorted_metrics) {
        std::cout << std::left << std::setw(28) << m->name << std::right << std::fixed;

        // Time segment CVs
        for (auto const& seg : m->segments) {
            std::cout << std::setw(8) << std::setprecision(1) << (seg.cv * 100) << "%"
                      << seg.grade[0];  // First char of grade (A/B/C/D/F)
        }

        // CV at boom
        if (report.boom_frame >= 0) {
            std::cout << std::setw(8) << std::setprecision(1) << (m->cv_at_boom * 100) << "%"
                      << m->boom_grade[0];
        } else {
            std::cout << std::setw(10) << "N/A";
        }

        // Overall
        std::cout << std::setw(8) << std::setprecision(1) << (m->mean_cv * 100) << "%"
                  << "  " << m->stability_grade << "\n";
    }
    std::cout << std::string(100, '-') << "\n\n";

    // === NEW: Absolute Value Analysis Table ===
    std::cout << "ABSOLUTE VALUE ANALYSIS (deviation from N=" << report.pendulum_counts.back() << " reference)\n";
    std::cout << std::string(120, '-') << "\n";
    std::cout << std::left << std::setw(24) << "Metric"
              << std::right
              << std::setw(12) << "Ref@Boom"
              << std::setw(10) << "ScaleCorr"
              << std::setw(10) << "Sens/2x";

    // Show deviation for each N (except reference)
    for (size_t i = 0; i + 1 < report.pendulum_counts.size(); ++i) {
        std::ostringstream oss;
        oss << "N" << report.pendulum_counts[i];
        std::cout << std::setw(10) << oss.str();
    }
    std::cout << std::setw(12) << "Converge@\n";
    std::cout << std::string(120, '-') << "\n";

    for (auto const* m : sorted_metrics) {
        if (m->abs_stats.empty()) continue;

        std::cout << std::left << std::setw(24) << m->name << std::right << std::fixed;

        // Reference boom value
        std::cout << std::setw(12) << std::setprecision(4) << m->ref_boom_value;

        // Scale correlation and sensitivity
        std::cout << std::setw(10) << std::setprecision(2) << m->scale_correlation;
        std::cout << std::setw(9) << std::setprecision(1) << m->scale_sensitivity << "%";

        // Deviation for each N (relative %)
        for (size_t i = 0; i + 1 < m->abs_stats.size(); ++i) {
            double dev = m->abs_stats[i].boom_rel_deviation;
            std::cout << std::setw(9) << std::setprecision(1) << dev << "%";
        }

        // Convergence N
        if (m->convergence_N > 0) {
            std::cout << std::setw(12) << m->convergence_N;
        } else {
            std::cout << std::setw(12) << ">max";
        }
        std::cout << "\n";
    }
    std::cout << std::string(120, '-') << "\n";
    std::cout << "  ScaleCorr: Pearson correlation between log(N) and value (-1 to +1)\n";
    std::cout << "  Sens/2x: % change in value per doubling of N\n";
    std::cout << "  N columns: relative deviation (%) from reference at boom time\n";
    std::cout << "  Converge@: Smallest N where deviation < 5%\n\n";

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
    std::vector<std::string> needs_work;
    std::vector<std::string> boom_stable;
    std::vector<std::string> boom_unstable;

    for (auto const* m : sorted_metrics) {
        if (m->mean_cv < 0.01) excellent++;
        else if (m->mean_cv < 0.05) good++;
        else if (m->mean_cv < 0.10) acceptable++;
        else {
            poor++;
            needs_work.push_back(m->name);
        }

        // Check boom stability specifically
        if (report.boom_frame >= 0) {
            if (m->cv_at_boom < 0.05) {
                boom_stable.push_back(m->name);
            } else if (m->cv_at_boom >= 0.10) {
                boom_unstable.push_back(m->name);
            }
        }
    }

    std::cout << "SUMMARY\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << "  Metrics analyzed: " << report.metrics.size() << "\n";
    std::cout << "  Excellent (<1% CV): " << excellent << "\n";
    std::cout << "  Good (<5% CV): " << good << "\n";
    std::cout << "  Acceptable (<10% CV): " << acceptable << "\n";
    std::cout << "  Poor (>=10% CV): " << poor << "\n";

    // Actionable feedback for iterative improvement
    if (!needs_work.empty()) {
        std::cout << "\n";
        std::cout << "FOCUS AREAS (metrics with >=10% CV):\n";
        for (auto const& name : needs_work) {
            // Find the metric and show segment-specific advice
            for (auto const* m : sorted_metrics) {
                if (m->name == name) {
                    std::cout << "  • " << name << ": overall "
                              << std::fixed << std::setprecision(1) << (m->mean_cv * 100) << "%";

                    // Find worst segment
                    double worst_cv = 0;
                    std::string worst_seg;
                    for (auto const& seg : m->segments) {
                        if (seg.cv > worst_cv) {
                            worst_cv = seg.cv;
                            worst_seg = seg.name;
                        }
                    }
                    if (!worst_seg.empty()) {
                        std::cout << ", worst in " << worst_seg
                                  << " (" << std::setprecision(0) << (worst_cv * 100) << "%)";
                    }

                    // Show boom stability
                    if (report.boom_frame >= 0) {
                        std::cout << ", @boom: " << m->boom_grade;
                    }
                    std::cout << "\n";
                    break;
                }
            }
        }
    }

    // Quick boom-specific summary for optimization use
    if (report.boom_frame >= 0) {
        std::cout << "\n";
        std::cout << "BOOM-TIME STABILITY (most important for optimization):\n";
        std::cout << "  Stable @boom (<5% CV): " << boom_stable.size() << " metrics\n";
        if (!boom_unstable.empty()) {
            std::cout << "  Unstable @boom (>=10% CV): ";
            for (size_t i = 0; i < boom_unstable.size() && i < 5; ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << boom_unstable[i];
            }
            if (boom_unstable.size() > 5) {
                std::cout << " (+" << (boom_unstable.size() - 5) << " more)";
            }
            std::cout << "\n";
        } else {
            std::cout << "  All metrics stable at boom time!\n";
        }
    }

    std::cout << std::string(80, '=') << "\n";

    // One-liner for quick iteration (can be grepped/parsed)
    int total = static_cast<int>(report.metrics.size());
    int stable = excellent + good;
    std::cout << "\n[STABILITY] " << stable << "/" << total << " stable"
              << " | " << poor << " need work"
              << " | boom detection: " << (report.targets[0].frame_stats.stddev < 3 ? "stable" : "variable")
              << "\n";
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
        // Add deviation columns (relative to max N)
        for (size_t i = 0; i + 1 < report.pendulum_counts.size(); ++i) {
            file << "," << m.name << "_dev_N" << report.pendulum_counts[i];
        }
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

            // Deviation from max N (relative %)
            if (values.size() >= 2) {
                double ref_value = values.back();  // Max N is last
                for (size_t i = 0; i + 1 < values.size(); ++i) {
                    if (std::abs(ref_value) > 1e-10) {
                        double dev = (values[i] - ref_value) / std::abs(ref_value) * 100.0;
                        file << "," << std::setprecision(2) << dev;
                    } else {
                        file << ",0";
                    }
                }
            } else {
                for (size_t i = 0; i + 1 < report.pendulum_counts.size(); ++i) {
                    file << ",";
                }
            }
        }
        file << "\n";
    }

    std::cout << "Detailed CSV saved to: " << path << "\n";

    // Also save a summary CSV with the absolute value analysis
    std::string summary_path = path;
    auto pos = summary_path.rfind(".csv");
    if (pos != std::string::npos) {
        summary_path = summary_path.substr(0, pos) + "_summary.csv";
    } else {
        summary_path += "_summary";
    }

    std::ofstream summary_file(summary_path);
    if (summary_file.is_open()) {
        summary_file << "metric,mean_cv,cv_at_boom,ref_boom_value,ref_mean,ref_max,"
                     << "scale_correlation,scale_sensitivity,convergence_N";
        for (size_t i = 0; i < report.pendulum_counts.size(); ++i) {
            summary_file << ",boom_N" << report.pendulum_counts[i];
        }
        for (size_t i = 0; i + 1 < report.pendulum_counts.size(); ++i) {
            summary_file << ",boom_dev_N" << report.pendulum_counts[i];
        }
        summary_file << "\n";

        for (auto const& m : report.metrics) {
            summary_file << m.name
                         << "," << std::fixed << std::setprecision(6) << m.mean_cv
                         << "," << m.cv_at_boom
                         << "," << m.ref_boom_value
                         << "," << m.ref_mean
                         << "," << m.ref_max_value
                         << "," << std::setprecision(4) << m.scale_correlation
                         << "," << std::setprecision(2) << m.scale_sensitivity
                         << "," << m.convergence_N;

            // Boom values at each N
            for (auto const& abs : m.abs_stats) {
                summary_file << "," << std::setprecision(6) << abs.value_at_boom;
            }
            // Boom deviations
            for (size_t i = 0; i + 1 < m.abs_stats.size(); ++i) {
                summary_file << "," << std::setprecision(2) << m.abs_stats[i].boom_rel_deviation;
            }
            summary_file << "\n";
        }
        std::cout << "Summary CSV saved to: " << summary_path << "\n";
    }
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
