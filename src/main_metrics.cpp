#include "simulation_data.h"
#include "config.h"
#include "gl_renderer.h"
#include "headless_gl.h"
#include "color_scheme.h"
#include "metrics/metrics_collector.h"
#include "metrics/event_detector.h"
#include "metrics/boom_analyzer.h"
#include "metrics/causticness_analyzer.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

void printUsage(char const* program) {
    std::cout << "Double Pendulum Metric Iteration Tool\n\n"
              << "Recompute metrics from saved simulation data without re-running physics.\n\n"
              << "Usage:\n"
              << "  " << program << " <simulation_data.bin> [options]\n\n"
              << "Options:\n"
              << "  --config <path>       Config for render/detection settings\n"
              << "                        (default: config.toml in same directory)\n"
              << "  --physics-only        Only compute physics metrics (default, no GPU)\n"
              << "  --render              Re-render frames and compute GPU metrics\n"
              << "  --output <path>       Output metrics CSV to path\n"
              << "  --validate            Compare with saved metrics.csv\n"
              << "  -h, --help            Show this help\n\n"
              << "Examples:\n"
              << "  # Recompute physics metrics\n"
              << "  " << program << " output/run_xxx/simulation_data.bin\n\n"
              << "  # Re-render with modified post-processing\n"
              << "  " << program << " output/run_xxx/simulation_data.bin --render\n\n"
              << "  # Validate reproducibility\n"
              << "  " << program << " output/run_xxx/simulation_data.bin --validate\n";
}

struct Options {
    fs::path data_path;
    fs::path config_path;
    fs::path output_path;
    bool physics_only = true;
    bool render = false;
    bool validate = false;
};

bool parseArgs(int argc, char* argv[], Options& opts) {
    if (argc < 2) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            exit(0);
        } else if (arg == "--config" && i + 1 < argc) {
            opts.config_path = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            opts.output_path = argv[++i];
        } else if (arg == "--physics-only") {
            opts.physics_only = true;
            opts.render = false;
        } else if (arg == "--render") {
            opts.render = true;
            opts.physics_only = false;
        } else if (arg == "--validate") {
            opts.validate = true;
        } else if (arg[0] != '-' && opts.data_path.empty()) {
            opts.data_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    if (opts.data_path.empty()) {
        std::cerr << "Error: No simulation data file specified\n";
        return false;
    }

    return true;
}

int computePhysicsMetrics(Options const& opts,
                          simulation_data::Reader const& reader,
                          Config const& config) {
    std::cout << "\nComputing physics metrics...\n";

    auto start_time = std::chrono::high_resolution_clock::now();

    metrics::MetricsCollector collector;
    collector.registerStandardMetrics();

    metrics::EventDetector detector;
    detector.addBoomCriteria(config.detection.boom_threshold,
                             config.detection.boom_confirmation,
                             metrics::MetricNames::Variance);
    detector.addChaosCriteria(config.detection.chaos_threshold,
                              config.detection.chaos_confirmation,
                              metrics::MetricNames::Variance);

    double const frame_duration =
        config.simulation.duration_seconds / reader.frameCount();

    std::vector<double> angle1s, angle2s;

    // Process each frame
    uint32_t const total_frames = reader.frameCount();
    for (uint32_t frame = 0; frame < total_frames; ++frame) {
        reader.getAnglesForFrame(frame, angle1s, angle2s);

        collector.beginFrame(static_cast<int>(frame));
        collector.updateFromAngles(angle1s, angle2s);
        collector.endFrame();

        detector.update(collector, frame_duration);

        // Progress indicator
        if (frame % 100 == 0 || frame == total_frames - 1) {
            std::cout << "\r  Frame " << frame + 1 << "/" << total_frames
                      << " (" << (100 * (frame + 1) / total_frames) << "%)"
                      << std::flush;
        }
    }
    std::cout << "\n";

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           end_time - start_time)
                           .count();

    std::cout << "  Processing time: " << duration_ms << " ms\n";

    // Run analyzers
    metrics::BoomAnalyzer boom_analyzer;
    metrics::CausticnessAnalyzer causticness_analyzer;
    causticness_analyzer.setFrameDuration(frame_duration);

    boom_analyzer.analyze(collector, detector);
    causticness_analyzer.analyze(collector, detector);

    // Print results
    std::cout << "\nResults:\n";

    // Boom detection: use max angular_causticness frame with 0.3s offset
    if (auto const* caustic_series = collector.getMetric(metrics::MetricNames::AngularCausticness)) {
        auto const& values = caustic_series->values();
        if (!values.empty()) {
            auto max_it = std::max_element(values.begin(), values.end());
            int max_frame = static_cast<int>(std::distance(values.begin(), max_it));
            // Apply 0.3s offset for consistency with other executables
            int offset_frames = static_cast<int>(0.3 / frame_duration);
            int boom_frame = std::max(0, max_frame - offset_frames);
            double boom_seconds = boom_frame * frame_duration;
            std::cout << "  Boom: frame " << boom_frame << " ("
                      << std::fixed << std::setprecision(2) << boom_seconds << "s)"
                      << ", causticness=" << std::setprecision(4) << *max_it << "\n";
        }
    }

    if (auto chaos = detector.getEvent(metrics::EventNames::Chaos)) {
        double chaos_seconds = chaos->frame * frame_duration;
        std::cout << "  Chaos: frame " << chaos->frame << " ("
                  << std::fixed << std::setprecision(2) << chaos_seconds << "s)"
                  << ", variance=" << std::setprecision(4) << chaos->value << "\n";
    } else {
        std::cout << "  Chaos: not detected\n";
    }

    std::cout << "  Final uniformity: " << std::fixed << std::setprecision(4)
              << collector.getUniformity() << "\n";

    if (boom_analyzer.hasResults()) {
        std::cout << "  Boom score: " << std::fixed << std::setprecision(3)
                  << boom_analyzer.score() << "\n";
    }

    if (causticness_analyzer.hasResults()) {
        auto const& metrics = causticness_analyzer.getMetrics();
        std::cout << "  Peak clarity: " << std::fixed << std::setprecision(3)
                  << metrics.peak_clarity_score;
        if (metrics.competing_peaks_count > 0) {
            std::cout << " (" << metrics.competing_peaks_count << " competing peak"
                      << (metrics.competing_peaks_count > 1 ? "s" : "")
                      << ", max ratio=" << std::setprecision(2) << metrics.max_competitor_ratio
                      << ")";
        }
        std::cout << "\n";

        std::cout << "  Post-boom sustain: " << std::fixed << std::setprecision(3)
                  << metrics.post_boom_area_normalized
                  << " (area=" << std::setprecision(1) << metrics.post_boom_area
                  << " over " << std::setprecision(1) << metrics.post_boom_duration << "s)\n";

        // Show detected peaks
        auto const& peaks = causticness_analyzer.getDetectedPeaks();
        if (!peaks.empty()) {
            std::cout << "  Detected peaks: " << peaks.size() << " [";
            for (size_t i = 0; i < peaks.size() && i < 5; ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << std::setprecision(2) << peaks[i].seconds << "s";
            }
            if (peaks.size() > 5) std::cout << ", ...";
            std::cout << "]\n";
        }
    }

    // Export metrics
    fs::path output_path = opts.output_path;
    if (output_path.empty()) {
        output_path = opts.data_path.parent_path() / "metrics_recomputed.csv";
    }

    collector.exportCSV(output_path.string());
    std::cout << "\nMetrics saved to: " << output_path << "\n";

    return 0;
}

int computeGPUMetrics(Options const& opts,
                      simulation_data::Reader const& reader,
                      Config const& config) {
    std::cout << "\nInitializing GPU rendering...\n";

    // Initialize headless GL
    HeadlessGL gl;
    if (!gl.init()) {
        std::cerr << "Failed to initialize headless OpenGL\n";
        return 1;
    }

    GLRenderer renderer;
    if (!renderer.init(config.render.width, config.render.height)) {
        std::cerr << "Failed to initialize GL renderer\n";
        return 1;
    }

    ColorSchemeGenerator color_gen(config.color);

    metrics::MetricsCollector collector;
    collector.registerStandardMetrics();
    collector.registerGPUMetrics();

    metrics::EventDetector detector;
    detector.addBoomCriteria(config.detection.boom_threshold,
                             config.detection.boom_confirmation,
                             metrics::MetricNames::Variance);
    detector.addChaosCriteria(config.detection.chaos_threshold,
                              config.detection.chaos_confirmation,
                              metrics::MetricNames::Variance);

    double const frame_duration =
        config.simulation.duration_seconds / reader.frameCount();

    std::cout << "Re-rendering " << reader.frameCount() << " frames at "
              << config.render.width << "x" << config.render.height << "...\n";

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<double> angle1s, angle2s;

    // Process each frame
    uint32_t const total_frames = reader.frameCount();
    for (uint32_t frame = 0; frame < total_frames; ++frame) {
        // Get frame data
        auto states = reader.getFrame(frame);
        reader.getAnglesForFrame(frame, angle1s, angle2s);

        // Clear and render
        renderer.clear();

        // Render each pendulum
        int const pendulum_count = static_cast<int>(states.size());
        float const center_x = config.render.width / 2.0f;
        float const center_y = config.render.height / 2.0f;
        float const scale =
            std::min(config.render.width, config.render.height) / 4.5f;

        for (int i = 0; i < pendulum_count; ++i) {
            auto const& state = states[i];
            auto color = color_gen.getColorForIndex(i, pendulum_count);

            float x0 = center_x;
            float y0 = center_y;
            float x1 = center_x + static_cast<float>(state.x1) * scale;
            float y1 = center_y + static_cast<float>(state.y1) * scale;
            float x2 = center_x + static_cast<float>(state.x2) * scale;
            float y2 = center_y + static_cast<float>(state.y2) * scale;

            renderer.drawLine(x0, y0, x1, y1, color.r, color.g, color.b);
            renderer.drawLine(x1, y1, x2, y2, color.r, color.g, color.b);
        }

        // Apply post-processing and compute metrics
        renderer.updateDisplayTexture(
            config.post_process.exposure, config.post_process.contrast,
            config.post_process.gamma, config.post_process.tone_map,
            config.post_process.reinhard_white_point,
            config.post_process.normalization);
        renderer.computeMetrics();

        // Collect metrics
        collector.beginFrame(static_cast<int>(frame));
        collector.updateFromAngles(angle1s, angle2s);

        metrics::GPUMetricsBundle gpu_metrics;
        gpu_metrics.max_value = renderer.lastMax();
        gpu_metrics.brightness = renderer.lastBrightness();
        gpu_metrics.coverage = renderer.lastCoverage();
        collector.setGPUMetrics(gpu_metrics);

        collector.endFrame();
        detector.update(collector, frame_duration);

        // Progress indicator
        if (frame % 50 == 0 || frame == total_frames - 1) {
            std::cout << "\r  Frame " << frame + 1 << "/" << total_frames
                      << " (" << (100 * (frame + 1) / total_frames) << "%)"
                      << std::flush;
        }
    }
    std::cout << "\n";

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
                          end_time - start_time)
                          .count();

    std::cout << "  Rendering time: " << duration_s << " s\n";
    std::cout << "  FPS: " << std::fixed << std::setprecision(1)
              << (static_cast<double>(total_frames) / duration_s) << "\n";

    // Run analyzers
    metrics::BoomAnalyzer boom_analyzer;
    metrics::CausticnessAnalyzer causticness_analyzer;
    causticness_analyzer.setFrameDuration(frame_duration);
    boom_analyzer.analyze(collector, detector);
    causticness_analyzer.analyze(collector, detector);

    // Print results
    std::cout << "\nResults:\n";

    // Boom detection: use max angular_causticness frame with 0.3s offset
    if (auto const* caustic_series = collector.getMetric(metrics::MetricNames::AngularCausticness)) {
        auto const& values = caustic_series->values();
        if (!values.empty()) {
            auto max_it = std::max_element(values.begin(), values.end());
            int max_frame = static_cast<int>(std::distance(values.begin(), max_it));
            // Apply 0.3s offset for consistency with other executables
            int offset_frames = static_cast<int>(0.3 / frame_duration);
            int boom_frame = std::max(0, max_frame - offset_frames);
            double boom_seconds = boom_frame * frame_duration;
            std::cout << "  Boom: frame " << boom_frame << " ("
                      << std::fixed << std::setprecision(2) << boom_seconds << "s)"
                      << ", causticness=" << std::setprecision(4) << *max_it << "\n";
        }
    }

    if (boom_analyzer.hasResults()) {
        std::cout << "  Boom score: " << std::fixed << std::setprecision(3)
                  << boom_analyzer.score() << "\n";
    }
    if (causticness_analyzer.hasResults()) {
        auto const& metrics = causticness_analyzer.getMetrics();
        std::cout << "  Causticness score: " << std::fixed << std::setprecision(3)
                  << causticness_analyzer.score() << "\n";
        std::cout << "  Peak clarity: " << std::fixed << std::setprecision(3)
                  << metrics.peak_clarity_score << "\n";
        std::cout << "  Post-boom sustain: " << std::fixed << std::setprecision(3)
                  << metrics.post_boom_area_normalized << "\n";
    }

    // Export metrics
    fs::path output_path = opts.output_path;
    if (output_path.empty()) {
        output_path = opts.data_path.parent_path() / "metrics_recomputed.csv";
    }

    collector.exportCSV(output_path.string());
    std::cout << "\nMetrics saved to: " << output_path << "\n";

    return 0;
}

int main(int argc, char* argv[]) {
    Options opts;

    if (!parseArgs(argc, argv, opts)) {
        printUsage(argv[0]);
        return 1;
    }

    // Check data file exists
    if (!fs::exists(opts.data_path)) {
        std::cerr << "Error: File not found: " << opts.data_path << "\n";
        return 1;
    }

    // Load simulation data
    simulation_data::Reader reader;
    if (!reader.open(opts.data_path)) {
        std::cerr << "Error: Failed to load simulation data\n";
        return 1;
    }

    // Load config
    Config config;
    if (opts.config_path.empty()) {
        // Try to find config.toml in same directory as data file
        opts.config_path = opts.data_path.parent_path() / "config.toml";
    }

    if (fs::exists(opts.config_path)) {
        std::cout << "Loading config: " << opts.config_path << "\n";
        config = Config::load(opts.config_path.string());
    } else {
        std::cout << "Warning: Config not found, using defaults\n";
        config = Config::defaults();
    }

    // Validate physics parameters match
    if (!simulation_data::validatePhysicsMatch(reader.header(), config)) {
        std::cerr << "Warning: Physics parameters in config don't match saved data\n";
    }

    // Run metric computation
    if (opts.render) {
        return computeGPUMetrics(opts, reader, config);
    } else {
        return computePhysicsMetrics(opts, reader, config);
    }
}
