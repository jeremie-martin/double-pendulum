#include "simulation.h"

#include "enum_strings.h"
#include "metrics/boom_detection.h"
#include "metrics/metrics_init.h"
#include "optimize/frame_detector.h"
#include "optimize/score_predictor.h"
#include "optimize/target_evaluator.h"
#include "simulation_data.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <png.h>
#include <sstream>
#include <thread>

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;

namespace {

// Convert config targets to PredictionTargets and evaluate using TargetEvaluator
std::vector<optimize::PredictionResult> evaluateTargets(
    std::vector<TargetConfig> const& targets,
    metrics::MetricsCollector const& collector,
    metrics::EventDetector const& events,
    metrics::CausticnessAnalyzer const& analyzer,
    double frame_duration) {

    if (targets.empty()) {
        return {};
    }

    optimize::TargetEvaluator evaluator;
    for (auto const& tc : targets) {
        auto target = optimize::targetConfigToPredictionTarget(
            tc.name, tc.type, tc.metric, tc.method,
            tc.offset_seconds, tc.peak_percent_threshold,
            tc.min_peak_prominence, tc.smoothing_window,
            tc.crossing_threshold, tc.crossing_confirmation,
            tc.weights);
        evaluator.addTarget(target);
    }

    return evaluator.evaluate(collector, events, analyzer, frame_duration);
}

// Get boom params from targets, or return empty params for defaults
optimize::FrameDetectionParams getBoomParamsFromTargets(
    std::vector<TargetConfig> const& targets) {

    for (auto const& tc : targets) {
        if (tc.name == "boom" && tc.type == "frame") {
            auto target = optimize::targetConfigToPredictionTarget(
                tc.name, tc.type, tc.metric, tc.method,
                tc.offset_seconds, tc.peak_percent_threshold,
                tc.min_peak_prominence, tc.smoothing_window,
                tc.crossing_threshold, tc.crossing_confirmation,
                tc.weights);
            return target.frameParams();
        }
    }
    return optimize::FrameDetectionParams{};
}

// Create default predictions when no targets configured (backward compat)
std::vector<optimize::PredictionResult> createDefaultPredictions(
    std::optional<int> boom_frame,
    double boom_seconds,
    double boom_value,
    std::optional<int> chaos_frame,
    double chaos_seconds,
    double chaos_variance,
    metrics::CausticnessAnalyzer const& analyzer) {

    std::vector<optimize::PredictionResult> predictions;

    if (boom_frame && *boom_frame >= 0) {
        optimize::PredictionResult boom_pred;
        boom_pred.target_name = "boom";
        boom_pred.type = optimize::PredictionType::Frame;
        boom_pred.predicted_frame = *boom_frame;
        boom_pred.predicted_seconds = boom_seconds;
        boom_pred.predicted_score = boom_value;
        predictions.push_back(boom_pred);
    }

    if (chaos_frame && *chaos_frame >= 0) {
        optimize::PredictionResult chaos_pred;
        chaos_pred.target_name = "chaos";
        chaos_pred.type = optimize::PredictionType::Frame;
        chaos_pred.predicted_frame = *chaos_frame;
        chaos_pred.predicted_seconds = chaos_seconds;
        chaos_pred.predicted_score = chaos_variance;
        predictions.push_back(chaos_pred);
    }

    if (analyzer.hasResults()) {
        optimize::PredictionResult quality_pred;
        quality_pred.target_name = "boom_quality";
        quality_pred.type = optimize::PredictionType::Score;
        quality_pred.predicted_score = analyzer.score();
        predictions.push_back(quality_pred);
    }

    return predictions;
}

void savePNGFile(char const* path, uint8_t const* data, int width, int height) {
    FILE* fp = fopen(path, "wb");
    if (!fp)
        return;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);

    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows(height);
    for (int y = 0; y < height; ++y) {
        rows[y] = const_cast<png_bytep>(data + y * width * 3);
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);

    png_destroy_write_struct(&png, &info);
    fclose(fp);
}
} // namespace

Simulation::Simulation(Config const& config) : config_(config), color_gen_(config.color) {
    // Apply per-metric parameters from config (must be done before registerStandardMetrics)
    metrics_collector_.setAllMetricConfigs(config_.metric_configs);

    // Initialize metrics system using common helper
    // frame_duration is computed from config (duration_seconds / total_frames)
    double frame_duration = config_.simulation.frameDuration();
    metrics::initializeMetricsSystem(
        metrics_collector_, event_detector_, causticness_analyzer_,
        config_.detection.chaos_threshold, config_.detection.chaos_confirmation,
        frame_duration, /*with_gpu=*/true);
}

Simulation::~Simulation() {
    renderer_.shutdown();
    gl_.shutdown();
}

std::string Simulation::createRunDirectory() {
    std::string path;

    if (config_.output.mode == OutputMode::Direct) {
        // Use output directory directly (for batch mode)
        path = config_.output.directory;
    } else {
        // Generate timestamp-based directory name
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&time);

        std::ostringstream dir_name;
        dir_name << config_.output.directory << "/run_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
        path = dir_name.str();
    }

    std::filesystem::create_directories(path);

    // Create frames subdirectory for PNG output
    if (config_.output.format == OutputFormat::PNG) {
        std::filesystem::create_directories(path + "/frames");
    }

    return path;
}

void Simulation::saveConfigCopy(std::string const& original_path) {
    // Copy the original config file if it exists
    if (std::filesystem::exists(original_path)) {
        std::filesystem::copy_file(original_path, run_directory_ + "/config.toml",
                                   std::filesystem::copy_options::overwrite_existing);
    }
}

void Simulation::saveMetadata(SimulationResults const& results) {
    std::ofstream out(run_directory_ + "/metadata.json");
    if (!out)
        return;

    // Get current time as ISO string
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    std::ostringstream time_str;
    time_str << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");

    // Calculate simulation speed (physics time / video time)
    double video_duration =
        static_cast<double>(config_.simulation.total_frames) / config_.output.video_fps;
    double simulation_speed = config_.simulation.duration_seconds / video_duration;

    // Calculate boom_seconds if available
    double boom_seconds = 0.0;
    if (results.boom_frame) {
        double frame_duration =
            config_.simulation.duration_seconds / config_.simulation.total_frames;
        boom_seconds = *results.boom_frame * frame_duration;
    }

    out << std::fixed << std::setprecision(6);
    out << "{\n";
    out << "  \"version\": \"1.0\",\n";
    out << "  \"created_at\": \"" << time_str.str() << "\",\n";
    out << "  \"config\": {\n";
    out << "    \"duration_seconds\": " << config_.simulation.duration_seconds << ",\n";
    out << "    \"total_frames\": " << config_.simulation.total_frames << ",\n";
    out << "    \"video_fps\": " << config_.output.video_fps << ",\n";
    out << "    \"video_duration\": " << video_duration << ",\n";
    out << "    \"simulation_speed\": " << simulation_speed << ",\n";
    out << "    \"pendulum_count\": " << config_.simulation.pendulum_count << ",\n";
    out << "    \"width\": " << config_.render.width << ",\n";
    out << "    \"height\": " << config_.render.height << ",\n";
    out << "    \"physics_quality\": \"" << toString(config_.simulation.physics_quality) << "\",\n";
    out << "    \"max_dt\": " << config_.simulation.max_dt << ",\n";
    out << "    \"substeps\": " << config_.simulation.substeps() << ",\n";
    out << "    \"dt\": " << config_.simulation.dt() << "\n";
    out << "  },\n";
    out << "  \"results\": {\n";
    out << "    \"frames_completed\": " << results.frames_completed << ",\n";
    if (results.boom_frame) {
        out << "    \"boom_frame\": " << *results.boom_frame << ",\n";
        out << "    \"boom_seconds\": " << boom_seconds << ",\n";
        out << "    \"boom_causticness\": " << results.boom_causticness << ",\n";
    } else {
        out << "    \"boom_frame\": null,\n";
        out << "    \"boom_seconds\": null,\n";
        out << "    \"boom_causticness\": null,\n";
    }
    if (results.chaos_frame) {
        out << "    \"chaos_frame\": " << *results.chaos_frame << ",\n";
        out << "    \"chaos_variance\": " << results.chaos_variance << ",\n";
    } else {
        out << "    \"chaos_frame\": null,\n";
        out << "    \"chaos_variance\": null,\n";
    }
    out << "    \"final_uniformity\": " << results.final_uniformity << "\n";
    out << "  },\n";
    out << "  \"timing\": {\n";
    out << "    \"total_seconds\": " << results.timing.total_seconds << ",\n";
    out << "    \"physics_seconds\": " << results.timing.physics_seconds << ",\n";
    out << "    \"render_seconds\": " << results.timing.render_seconds << ",\n";
    out << "    \"io_seconds\": " << results.timing.io_seconds << "\n";
    out << "  },\n";

    // Add detection parameters section
    out << "  \"detection\": {\n";
    out << "    \"chaos\": {\n";
    out << "      \"threshold\": " << config_.detection.chaos_threshold << ",\n";
    out << "      \"confirmation\": " << config_.detection.chaos_confirmation << ",\n";
    out << "      \"early_stop\": " << (config_.detection.early_stop_after_chaos ? "true" : "false") << "\n";
    out << "    },\n";
    out << "    \"targets\": [\n";
    bool first_target = true;
    for (auto const& tc : config_.targets) {
        if (!first_target)
            out << ",\n";
        out << "      {\n";
        out << "        \"name\": \"" << tc.name << "\",\n";
        out << "        \"type\": \"" << tc.type << "\",\n";
        out << "        \"metric\": \"" << tc.metric << "\",\n";
        out << "        \"method\": \"" << tc.method << "\"";
        if (tc.type == "frame") {
            out << ",\n        \"offset_seconds\": " << tc.offset_seconds;
            // Write method-specific params
            if (tc.method == "first_peak_percent") {
                out << ",\n        \"peak_percent_threshold\": " << tc.peak_percent_threshold;
                out << ",\n        \"min_peak_prominence\": " << tc.min_peak_prominence;
            } else if (tc.method == "derivative_peak" || tc.method == "second_derivative_peak") {
                out << ",\n        \"smoothing_window\": " << tc.smoothing_window;
            } else if (tc.method == "threshold_crossing") {
                out << ",\n        \"crossing_threshold\": " << tc.crossing_threshold;
                out << ",\n        \"crossing_confirmation\": " << tc.crossing_confirmation;
            }
        }
        out << "\n      }";
        first_target = false;
    }
    out << "\n    ]\n";
    out << "  }";

    // Add scores section if any scores computed
    if (!results.score.empty()) {
        out << ",\n";
        out << "  \"scores\": {\n";
        bool first = true;
        for (auto const& [name, value] : results.score.scores) {
            if (!first)
                out << ",\n";
            out << "    \"" << name << "\": " << value;
            first = false;
        }
        out << "\n";
        out << "  }";
    }

    // Add predictions section if any predictions computed
    if (!results.predictions.empty()) {
        out << ",\n";
        out << "  \"predictions\": {\n";
        bool first = true;
        for (auto const& p : results.predictions) {
            if (!first)
                out << ",\n";
            out << "    \"" << p.target_name << "\": {\n";
            out << "      \"type\": \"" << optimize::toString(p.type) << "\",\n";
            if (p.isFrame()) {
                out << "      \"frame\": " << p.predicted_frame << ",\n";
                out << "      \"seconds\": " << p.predicted_seconds << ",\n";
            }
            out << "      \"score\": " << p.predicted_score << "\n";
            out << "    }";
            first = false;
        }
        out << "\n";
        out << "  }";
    }

    out << "\n}\n";
}

void Simulation::saveMetricsCSV() {
    std::ofstream out(run_directory_ + "/metrics.csv");
    if (!out) {
        return;
    }

    // Get all metric series (using metric names for consistency)
    auto* variance = metrics_collector_.getMetric(metrics::MetricNames::Variance);
    auto* circular_spread = metrics_collector_.getMetric(metrics::MetricNames::CircularSpread);
    auto* spread_ratio = metrics_collector_.getMetric(metrics::MetricNames::SpreadRatio);
    auto* angular_range = metrics_collector_.getMetric(metrics::MetricNames::AngularRange);
    auto* angular_causticness =
        metrics_collector_.getMetric(metrics::MetricNames::AngularCausticness);
    // New caustic metrics
    auto* r1 = metrics_collector_.getMetric(metrics::MetricNames::R1);
    auto* r2 = metrics_collector_.getMetric(metrics::MetricNames::R2);
    auto* joint_concentration =
        metrics_collector_.getMetric(metrics::MetricNames::JointConcentration);
    auto* tip_causticness = metrics_collector_.getMetric(metrics::MetricNames::TipCausticness);
    auto* spatial_concentration =
        metrics_collector_.getMetric(metrics::MetricNames::SpatialConcentration);
    // Alternative caustic metrics (experimental)
    auto* cv_causticness = metrics_collector_.getMetric(metrics::MetricNames::CVCausticness);
    auto* organization = metrics_collector_.getMetric(metrics::MetricNames::OrganizationCausticness);
    auto* fold_causticness = metrics_collector_.getMetric(metrics::MetricNames::FoldCausticness);
    // New paradigm metrics (local coherence based)
    auto* smoothness = metrics_collector_.getMetric(metrics::MetricNames::TrajectorySmoothness);
    auto* curvature = metrics_collector_.getMetric(metrics::MetricNames::Curvature);
    auto* true_folds = metrics_collector_.getMetric(metrics::MetricNames::TrueFolds);
    auto* local_coherence = metrics_collector_.getMetric(metrics::MetricNames::LocalCoherence);
    // GPU and energy metrics
    auto* brightness = metrics_collector_.getMetric(metrics::MetricNames::Brightness);
    auto* coverage = metrics_collector_.getMetric(metrics::MetricNames::Coverage);
    auto* total_energy = metrics_collector_.getMetric(metrics::MetricNames::TotalEnergy);

    // Determine number of frames
    size_t frame_count = variance ? variance->size() : 0;
    if (frame_count == 0)
        return;

    // Helper to get value at frame
    auto getValue = [](auto* series, size_t i) -> double {
        return (series && i < series->size()) ? series->at(i) : 0.0;
    };

    // Write header - CANONICAL COLUMN ORDER for simulation metrics CSV
    // This order is intentional and matches what downstream tools expect.
    // Physics metrics first, then new caustic metrics, then GPU metrics, then energy.
    out << "frame,variance,circular_spread,spread_ratio,angular_range,angular_causticness,"
        << "r1_concentration,r2_concentration,joint_concentration,"
        << "tip_causticness,spatial_concentration,"
        << "cv_causticness,organization_causticness,fold_causticness,"
        << "trajectory_smoothness,curvature,true_folds,local_coherence,"
        << "brightness,coverage,total_energy\n";
    out << std::fixed << std::setprecision(6);

    // Write data
    for (size_t i = 0; i < frame_count; ++i) {
        out << i;
        out << "," << getValue(variance, i);
        out << "," << getValue(circular_spread, i);
        out << "," << getValue(spread_ratio, i);
        out << "," << getValue(angular_range, i);
        out << "," << getValue(angular_causticness, i);
        // New caustic metrics
        out << "," << getValue(r1, i);
        out << "," << getValue(r2, i);
        out << "," << getValue(joint_concentration, i);
        out << "," << getValue(tip_causticness, i);
        out << "," << getValue(spatial_concentration, i);
        // Alternative caustic metrics
        out << "," << getValue(cv_causticness, i);
        out << "," << getValue(organization, i);
        out << "," << getValue(fold_causticness, i);
        // New paradigm metrics
        out << "," << getValue(smoothness, i);
        out << "," << getValue(curvature, i);
        out << "," << getValue(true_folds, i);
        out << "," << getValue(local_coherence, i);
        // GPU and energy metrics
        out << "," << getValue(brightness, i);
        out << "," << getValue(coverage, i);
        out << "," << getValue(total_energy, i);
        out << "\n";
    }
}

SimulationResults Simulation::run(ProgressCallback progress, std::string const& config_path) {
    int const pendulum_count = config_.simulation.pendulum_count;
    int const total_frames = config_.simulation.total_frames;
    int const substeps = config_.simulation.substeps();
    double const dt = config_.simulation.dt();
    int const width = config_.render.width;
    int const height = config_.render.height;

    // Initialize headless OpenGL context
    if (!gl_.init()) {
        std::cerr << "Failed to initialize headless OpenGL\n";
        return SimulationResults{};
    }

    // Initialize GPU renderer
    if (!renderer_.init(width, height)) {
        std::cerr << "Failed to initialize GL renderer\n";
        return SimulationResults{};
    }

    // Create run directory with timestamp
    run_directory_ = createRunDirectory();
    std::cout << "Output directory: " << run_directory_ << "\n";

    // Print prediction target configuration
    if (!config_.targets.empty()) {
        std::cout << "Prediction targets:\n";
        for (auto const& tc : config_.targets) {
            std::cout << "  " << tc.name << ": metric=" << tc.metric
                      << ", method=" << tc.method;
            if (tc.type == "frame") {
                std::cout << ", offset=" << tc.offset_seconds << "s";
            }
            std::cout << "\n";
        }
    } else {
        std::cout << "Prediction targets: (defaults)\n";
    }

    // Timing accumulators
    Duration physics_time{0};
    Duration render_time{0};
    Duration io_time{0};
    auto total_start = Clock::now();

    // Max value tracking for diagnostics
    std::vector<float> max_values;
    max_values.reserve(total_frames);

    // Determine thread count
    int thread_count = config_.render.thread_count;
    if (thread_count <= 0) {
        thread_count = std::thread::hardware_concurrency();
    }

    // Initialize pendulums with varied initial angles
    std::vector<Pendulum> pendulums(pendulum_count);
    initializePendulums(pendulums);

    // Pre-compute colors for all pendulums
    std::vector<Color> colors(pendulum_count);
    for (int i = 0; i < pendulum_count; ++i) {
        colors[i] = color_gen_.getColorForIndex(i, pendulum_count);
    }

    // Setup video writer if needed
    std::unique_ptr<VideoWriter> video_writer;
    if (config_.output.format == OutputFormat::Video) {
        std::string video_path = run_directory_ + "/video.mp4";
        video_writer =
            std::make_unique<VideoWriter>(width, height, config_.output.video_fps, config_.output);
        if (!video_writer->open(video_path)) {
            std::cerr << "Failed to open video writer\n";
            return SimulationResults{};
        }
    }

    // Setup simulation data writer for metric iteration
    std::unique_ptr<simulation_data::Writer> data_writer;
    if (config_.output.save_simulation_data) {
        data_writer = std::make_unique<simulation_data::Writer>();
        std::string data_path = run_directory_ + "/simulation_data.bin";
        if (!data_writer->open(data_path, config_, total_frames)) {
            std::cerr << "Failed to open simulation data writer\n";
            return SimulationResults{};
        }
    }

    // Allocate buffers
    std::vector<PendulumState> states(pendulum_count);
    std::vector<uint8_t> rgb_buffer(width * height * 3);

    // Coordinate transform
    float centerX = static_cast<float>(width) / 2.0f;
    float centerY = static_cast<float>(height) / 2.0f;
    float scale = static_cast<float>(width) / 5.0f;

    SimulationResults results;

    // Frame duration for event timing
    double const frame_duration = config_.simulation.frameDuration();

    // Reset metrics for fresh run
    metrics_collector_.reset();
    event_detector_.reset();

    // Main simulation loop (streaming mode)
    bool early_stopped = false;
    for (int frame = 0; frame < total_frames; ++frame) {
        // Physics timing
        auto physics_start = Clock::now();
        stepPendulums(pendulums, states, substeps, dt, thread_count);
        physics_time += Clock::now() - physics_start;

        // Save raw simulation data for metric iteration
        if (data_writer) {
            data_writer->writeFrame(states);
        }

        // Begin frame for metrics collection
        metrics_collector_.beginFrame(frame);

        // Track all physics metrics (variance, spread, causticness metrics)
        // using full state data for position-based metrics
        metrics_collector_.updateFromStates(states);

        // Compute total energy (physics metric)
        double total_energy = 0.0;
        for (auto const& p : pendulums) {
            total_energy += p.totalEnergy();
        }
        metrics_collector_.setMetric(metrics::MetricNames::TotalEnergy, total_energy);

        // NOTE: endFrame() and event detection happen AFTER rendering to include GPU metrics

        // Render timing (GPU)
        auto render_start = Clock::now();
        renderer_.clear();

        for (int i = 0; i < pendulum_count; ++i) {
            auto const& state = states[i];
            auto const& color = colors[i];

            float x0 = centerX;
            float y0 = centerY;
            float x1 = centerX + static_cast<float>(state.x1 * scale);
            float y1 = centerY + static_cast<float>(state.y1 * scale);
            float x2 = centerX + static_cast<float>(state.x2 * scale);
            float y2 = centerY + static_cast<float>(state.y2 * scale);

            renderer_.drawLine(x0, y0, x1, y1, color.r, color.g, color.b);
            renderer_.drawLine(x1, y1, x2, y2, color.r, color.g, color.b);
        }

        // Read pixels with full post-processing pipeline
        renderer_.readPixels(rgb_buffer, static_cast<float>(config_.post_process.exposure),
                             static_cast<float>(config_.post_process.contrast),
                             static_cast<float>(config_.post_process.gamma),
                             config_.post_process.tone_map,
                             static_cast<float>(config_.post_process.reinhard_white_point),
                             config_.post_process.normalization, pendulum_count);
        max_values.push_back(renderer_.lastMax());

        // Update metrics with GPU stats (always collected - metrics are central)
        {
            metrics::GPUMetricsBundle gpu_metrics;
            gpu_metrics.max_value = renderer_.lastMax();
            gpu_metrics.brightness = renderer_.lastBrightness();
            gpu_metrics.coverage = renderer_.lastCoverage();
            metrics_collector_.setGPUMetrics(gpu_metrics);
        }

        // End frame metrics collection (after ALL metrics including GPU are set)
        metrics_collector_.endFrame();

        // Update event detection (needs complete frame data)
        bool had_chaos = event_detector_.isDetected(metrics::EventNames::Chaos);
        event_detector_.update(metrics_collector_, frame_duration);

        // Early stop if chaos was newly detected and configured
        if (!had_chaos && event_detector_.isDetected(metrics::EventNames::Chaos) &&
            config_.detection.early_stop_after_chaos) {
            results.chaos_frame = event_detector_.getEvent(metrics::EventNames::Chaos)->frame;
            results.chaos_variance = event_detector_.getEvent(metrics::EventNames::Chaos)->value;
            results.frames_completed = frame + 1;
            early_stopped = true;
            // Still need to write this frame
            render_time += Clock::now() - render_start;
            auto io_start = Clock::now();
            if (config_.output.format == OutputFormat::Video) {
                video_writer->writeFrame(rgb_buffer.data());
            } else {
                savePNG(rgb_buffer, width, height, frame);
            }
            io_time += Clock::now() - io_start;
            break;
        }

        render_time += Clock::now() - render_start;

        // I/O timing
        auto io_start = Clock::now();
        if (config_.output.format == OutputFormat::Video) {
            video_writer->writeFrame(rgb_buffer.data());
        } else {
            savePNG(rgb_buffer, width, height, frame);
        }
        io_time += Clock::now() - io_start;

        results.frames_completed = frame + 1;

        // Progress
        if (progress) {
            progress(frame + 1, total_frames);
        } else {
            std::cout << "\rFrame " << std::setw(4) << (frame + 1) << "/" << total_frames
                      << std::flush;
        }
    }

    if (video_writer) {
        auto io_start = Clock::now();
        video_writer->close();
        io_time += Clock::now() - io_start;
    }

    // Finalize simulation data file
    if (data_writer) {
        auto io_start = Clock::now();
        data_writer->close();
        io_time += Clock::now() - io_start;
    }

    auto total_time = Clock::now() - total_start;

    // Populate timing results
    results.timing.total_seconds = Duration(total_time).count();
    results.timing.physics_seconds = physics_time.count();
    results.timing.render_seconds = render_time.count();
    results.timing.io_seconds = io_time.count();

    // Extract detected events (legacy variance-based)
    if (auto boom_event = event_detector_.getEvent(metrics::EventNames::Boom)) {
        if (boom_event->detected()) {
            results.boom_frame = boom_event->frame;
            results.boom_causticness = boom_event->value;
        }
    }
    if (auto chaos_event = event_detector_.getEvent(metrics::EventNames::Chaos)) {
        if (chaos_event->detected()) {
            results.chaos_frame = chaos_event->frame;
            results.chaos_variance = chaos_event->value;
        }
    }

    // Detect boom using configured target or defaults
    auto boom_params = getBoomParamsFromTargets(config_.targets);
    bool has_boom_target = !boom_params.metric_name.empty();

    auto boom = has_boom_target
        ? metrics::findBoomFrame(metrics_collector_, frame_duration, boom_params)
        : metrics::findBoomFrame(metrics_collector_, frame_duration);

    if (boom.frame >= 0) {
        results.boom_frame = boom.frame;
        results.boom_causticness = boom.metric_value;

        // Force boom event for analyzers (e.g., CausticnessAnalyzer) to use
        double variance_at_boom = 0.0;
        if (auto* var_series = metrics_collector_.getMetric(metrics::MetricNames::Variance)) {
            if (boom.frame < static_cast<int>(var_series->size())) {
                variance_at_boom = var_series->at(boom.frame);
            }
        }
        metrics::forceBoomEvent(event_detector_, boom, variance_at_boom);
    }

    // Extract metrics history
    if (auto const* variance_series = metrics_collector_.getMetric(metrics::MetricNames::Variance)) {
        results.variance_history = variance_series->values();
    }
    results.spread_history = metrics_collector_.getSpreadHistory();
    results.final_uniformity = metrics_collector_.getUniformity();

    // Run analyzers to compute quality scores
    causticness_analyzer_.analyze(metrics_collector_, event_detector_);

    // Aggregate scores
    if (causticness_analyzer_.hasResults()) {
        results.score.set(metrics::ScoreNames::Causticness, causticness_analyzer_.score());
        results.score.set(metrics::ScoreNames::PeakClarity,
                          causticness_analyzer_.peakClarityScore());
        results.score.set(metrics::ScoreNames::PostBoomSustain,
                          causticness_analyzer_.postBoomAreaNormalized());
    }

    // Populate multi-target predictions using TargetEvaluator
    if (!config_.targets.empty()) {
        results.predictions = evaluateTargets(
            config_.targets, metrics_collector_, event_detector_,
            causticness_analyzer_, frame_duration);
    } else {
        // Use default predictions (backward compat)
        double boom_seconds = results.boom_frame ? *results.boom_frame * frame_duration : 0.0;
        double chaos_seconds = results.chaos_frame ? *results.chaos_frame * frame_duration : 0.0;
        results.predictions = createDefaultPredictions(
            results.boom_frame, boom_seconds, results.boom_causticness,
            results.chaos_frame, chaos_seconds, results.chaos_variance,
            causticness_analyzer_);
    }

    // Save metadata, config copy, and metrics
    saveMetadata(results);
    if (!config_path.empty()) {
        saveConfigCopy(config_path);
    }
    saveMetricsCSV();

    // Store output paths in results
    results.output_directory = run_directory_;
    if (config_.output.format == OutputFormat::Video) {
        results.video_path = run_directory_ + "/video.mp4";
    }

    // Determine output path for display
    std::string output_path =
        results.video_path.empty() ? run_directory_ + "/frames/" : results.video_path;

    // Calculate video duration
    double video_duration =
        static_cast<double>(results.frames_completed) / config_.output.video_fps;

    // Print results
    std::cout << "\n\n";
    if (early_stopped) {
        std::cout << "=== Simulation Stopped Early (chaos detected) ===\n";
    } else {
        std::cout << "=== Simulation Complete ===\n";
    }

    // Calculate simulation speed
    double simulation_speed = config_.simulation.duration_seconds / video_duration;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Frames:      " << results.frames_completed << "/" << total_frames;
    if (early_stopped)
        std::cout << " (early stop)";
    std::cout << "\n";
    std::cout << "Video:       " << video_duration << "s @ " << config_.output.video_fps << " FPS ("
              << simulation_speed << "x speed)\n";
    std::cout << "Pendulums:   " << pendulum_count << "\n";
    std::cout << "Physics:     " << substeps << " substeps/frame, dt=" << (dt * 1000) << "ms\n";
    std::cout << "Total time:  " << results.timing.total_seconds << "s\n";
    std::cout << "  Physics:   " << std::setw(5) << results.timing.physics_seconds << "s ("
              << std::setw(4)
              << (results.timing.physics_seconds / results.timing.total_seconds * 100) << "%)\n";
    std::cout << "  Render:    " << std::setw(5) << results.timing.render_seconds << "s ("
              << std::setw(4)
              << (results.timing.render_seconds / results.timing.total_seconds * 100) << "%)\n";
    std::cout << "  I/O:       " << std::setw(5) << results.timing.io_seconds << "s ("
              << std::setw(4) << (results.timing.io_seconds / results.timing.total_seconds * 100)
              << "%)\n";

    if (results.boom_frame) {
        double boom_seconds = *results.boom_frame * frame_duration;
        std::cout << "Boom:        " << std::setprecision(2) << boom_seconds << "s (frame "
                  << *results.boom_frame << ", causticness=" << std::setprecision(4)
                  << results.boom_causticness << ")\n";
    }
    if (results.chaos_frame) {
        double chaos_seconds = *results.chaos_frame * frame_duration;
        std::cout << "Chaos:       " << std::setprecision(2) << chaos_seconds << "s (frame "
                  << *results.chaos_frame << ", var=" << std::setprecision(4)
                  << results.chaos_variance << ")\n";
    }
    std::cout << "Uniformity:  " << std::setprecision(2) << results.final_uniformity
              << " (target: 0.9)\n";

    // Display analyzer scores
    if (causticness_analyzer_.hasResults()) {
        auto const& json = causticness_analyzer_.toJSON();
        auto const& m = json["metrics"];
        std::cout << "Causticness: " << std::setprecision(2) << causticness_analyzer_.score()
                  << " (peak=" << m.value("peak_causticness", 0.0)
                  << ", avg=" << m.value("average_causticness", 0.0)
                  << ", clarity=" << m.value("peak_clarity_score", 0.0) << ")\n";
    }

    std::cout << "\nOutput: " << output_path << "\n";

    return results;
}

metrics::ProbePhaseResults Simulation::runProbe(ProgressCallback progress) {
    // Physics-only simulation for parameter evaluation
    // No GL, no rendering, no I/O - just physics + variance/spread tracking

    metrics::ProbePhaseResults results;

    int const pendulum_count = config_.simulation.pendulum_count;
    int const total_frames = config_.simulation.total_frames;
    int const substeps = config_.simulation.substeps();
    double const dt = config_.simulation.dt();

    // Determine thread count
    int thread_count = config_.render.thread_count;
    if (thread_count <= 0) {
        thread_count = std::thread::hardware_concurrency();
    }

    // Initialize pendulums
    std::vector<Pendulum> pendulums(pendulum_count);
    initializePendulums(pendulums);

    // Allocate state buffer
    std::vector<PendulumState> states(pendulum_count);

    // Reset metrics system for fresh probe (physics-only, no GPU)
    double const frame_duration = config_.simulation.frameDuration();
    metrics::resetMetricsSystem(metrics_collector_, event_detector_, causticness_analyzer_);
    metrics::initializeMetricsSystem(
        metrics_collector_, event_detector_, causticness_analyzer_,
        config_.detection.chaos_threshold, config_.detection.chaos_confirmation,
        frame_duration, /*with_gpu=*/false);

    // Main physics loop
    for (int frame = 0; frame < total_frames; ++frame) {
        // Step physics
        stepPendulums(pendulums, states, substeps, dt, thread_count);

        // Begin frame for metrics collection
        metrics_collector_.beginFrame(frame);

        // Extract angles for variance and spread tracking
        std::vector<double> angle1s, angle2s;
        angle1s.reserve(pendulum_count);
        angle2s.reserve(pendulum_count);
        for (auto const& state : states) {
            angle1s.push_back(state.th1);
            angle2s.push_back(state.th2);
        }

        // Update metrics collector with angles
        metrics_collector_.updateFromAngles(angle1s, angle2s);

        // Update event detection
        event_detector_.update(metrics_collector_, frame_duration);

        // End frame metrics collection
        metrics_collector_.endFrame();

        results.frames_completed = frame + 1;

        // Progress callback
        if (progress) {
            progress(frame + 1, total_frames);
        }
    }

    // Get chaos event from detector
    if (auto chaos_event = event_detector_.getEvent(metrics::EventNames::Chaos)) {
        if (chaos_event->detected()) {
            results.chaos_frame = chaos_event->frame;
            results.chaos_seconds = chaos_event->seconds;
        }
    }

    // Get boom params from config.targets (consistent with run())
    auto boom_params = getBoomParamsFromTargets(config_.targets);
    bool has_boom_target = !boom_params.metric_name.empty();

    // Run post-simulation analysis with configured boom params
    auto boom = has_boom_target
        ? metrics::runPostSimulationAnalysis(
              metrics_collector_, event_detector_, causticness_analyzer_, frame_duration, boom_params)
        : metrics::runPostSimulationAnalysis(
              metrics_collector_, event_detector_, causticness_analyzer_, frame_duration);

    if (boom.frame >= 0) {
        results.boom_frame = boom.frame;
        results.boom_seconds = boom.seconds;
    }

    // Final metrics
    if (auto const* variance_series = metrics_collector_.getMetric(metrics::MetricNames::Variance)) {
        if (!variance_series->empty()) {
            results.final_variance = variance_series->current();
        }
    }
    results.final_uniformity = metrics_collector_.getUniformity();

    // Scores from analyzers
    if (causticness_analyzer_.hasResults()) {
        results.score.set(metrics::ScoreNames::Causticness, causticness_analyzer_.score());
        results.score.set(metrics::ScoreNames::PeakClarity,
                          causticness_analyzer_.peakClarityScore());
        results.score.set(metrics::ScoreNames::PostBoomSustain,
                          causticness_analyzer_.postBoomAreaNormalized());
    }

    // Populate predictions using TargetEvaluator (consistent with run())
    if (!config_.targets.empty()) {
        results.predictions = evaluateTargets(
            config_.targets, metrics_collector_, event_detector_,
            causticness_analyzer_, frame_duration);
    } else {
        // Use default predictions (backward compat)
        results.predictions = createDefaultPredictions(
            results.boom_frame >= 0 ? std::optional<int>(results.boom_frame) : std::nullopt,
            results.boom_seconds,
            0.0, // boom_value not tracked in probe
            results.chaos_frame >= 0 ? std::optional<int>(results.chaos_frame) : std::nullopt,
            results.chaos_seconds,
            0.0, // chaos_variance not tracked in probe
            causticness_analyzer_);
    }

    results.completed = true;
    results.passed_filter = true; // No filter applied in basic probe

    return results;
}

void Simulation::initializePendulums(std::vector<Pendulum>& pendulums) {
    int const n = pendulums.size();
    double const center_angle = config_.physics.initial_angle1;
    double const variation = config_.simulation.angle_variation;

    for (int i = 0; i < n; ++i) {
        // Linear spread of initial angles around center
        double t = (n > 1) ? static_cast<double>(i) / (n - 1) : 0.0;
        double th1 = center_angle - variation / 2 + t * variation;

        pendulums[i] = Pendulum(
            config_.physics.gravity, config_.physics.length1, config_.physics.length2,
            config_.physics.mass1, config_.physics.mass2, th1, config_.physics.initial_angle2,
            config_.physics.initial_velocity1, config_.physics.initial_velocity2);
    }
}

void Simulation::stepPendulums(std::vector<Pendulum>& pendulums, std::vector<PendulumState>& states,
                               int substeps, double dt, int thread_count) {
    int const n = pendulums.size();
    int const chunk_size = n / thread_count;

    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; ++t) {
        int start = t * chunk_size;
        int end = (t == thread_count - 1) ? n : start + chunk_size;

        threads.emplace_back([&, start, end]() {
            for (int i = start; i < end; ++i) {
                for (int s = 0; s < substeps; ++s) {
                    states[i] = pendulums[i].step(dt);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }
}

void Simulation::savePNG(std::vector<uint8_t> const& data, int width, int height, int frame) {
    std::ostringstream path;
    path << run_directory_ << "/frames/" << config_.output.filename_prefix << std::setfill('0')
         << std::setw(4) << frame << ".png";

    savePNGFile(path.str().c_str(), data.data(), width, height);
}
