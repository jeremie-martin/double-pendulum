#include "simulation.h"

#include "enum_strings.h"

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

Simulation::Simulation(Config const& config) : config_(config), color_gen_(config.color) {}

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
        out << "    \"boom_variance\": " << results.boom_variance << ",\n";
    } else {
        out << "    \"boom_frame\": null,\n";
        out << "    \"boom_seconds\": null,\n";
        out << "    \"boom_variance\": null,\n";
    }
    if (results.white_frame) {
        out << "    \"white_frame\": " << *results.white_frame << ",\n";
        out << "    \"white_variance\": " << results.white_variance << ",\n";
    } else {
        out << "    \"white_frame\": null,\n";
        out << "    \"white_variance\": null,\n";
    }
    out << "    \"final_spread_ratio\": " << results.final_spread_ratio << "\n";
    out << "  },\n";
    out << "  \"timing\": {\n";
    out << "    \"total_seconds\": " << results.timing.total_seconds << ",\n";
    out << "    \"physics_seconds\": " << results.timing.physics_seconds << ",\n";
    out << "    \"render_seconds\": " << results.timing.render_seconds << ",\n";
    out << "    \"io_seconds\": " << results.timing.io_seconds << "\n";
    out << "  }\n";
    out << "}\n";
}

void Simulation::saveVarianceCSV(std::vector<double> const& variance,
                                  std::vector<float> const& max_values,
                                  std::vector<SpreadMetrics> const& spread) {
    std::ofstream out(run_directory_ + "/variance.csv");
    if (!out)
        return;

    out << "frame,variance,max_value,spread_ratio\n";
    out << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < variance.size(); ++i) {
        out << i << "," << variance[i];
        if (i < max_values.size()) {
            out << "," << max_values[i];
        } else {
            out << ",";
        }
        if (i < spread.size()) {
            out << "," << spread[i].spread_ratio;
        }
        out << "\n";
    }
}

void Simulation::saveAnalysisCSV(std::vector<FrameAnalysis> const& analysis) {
    std::ofstream out(run_directory_ + "/analysis.csv");
    if (!out)
        return;

    out << "frame,variance,max_value,brightness,total_energy\n";
    out << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < analysis.size(); ++i) {
        auto const& a = analysis[i];
        out << i << "," << a.variance << "," << a.max_value << ","
            << a.brightness << "," << a.total_energy << "\n";
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

    // Allocate buffers
    std::vector<PendulumState> states(pendulum_count);
    std::vector<uint8_t> rgb_buffer(width * height * 3);

    // Coordinate transform
    float centerX = static_cast<float>(width) / 2.0f;
    float centerY = static_cast<float>(height) / 2.0f;
    float scale = static_cast<float>(width) / 5.0f;

    SimulationResults results;

    // Detection thresholds from config
    auto const& detect = config_.detection;

    // Main simulation loop (streaming mode)
    bool early_stopped = false;
    for (int frame = 0; frame < total_frames; ++frame) {
        // Physics timing
        auto physics_start = Clock::now();
        stepPendulums(pendulums, states, substeps, dt, thread_count);
        physics_time += Clock::now() - physics_start;

        // Track variance and spread (always enabled - cheap operation)
        std::vector<double> angle1s, angle2s;
        angle1s.reserve(pendulum_count);
        angle2s.reserve(pendulum_count);
        for (auto const& state : states) {
            angle1s.push_back(state.th1); // First pendulum angle (for spread)
            angle2s.push_back(state.th2); // Second pendulum angle (for variance)
        }
        variance_tracker_.updateWithSpread(angle2s, angle1s);

        // Extended analysis (when enabled - includes energy computation)
        if (config_.analysis.enabled) {
            analysis_tracker_.update(pendulums, 0.0f, 0.0f);  // GPU stats updated after render
        }

        // Update detection results using shared utility
        bool had_white = results.white_frame.has_value();
        VarianceUtils::ThresholdResults detection{results.boom_frame, results.boom_variance,
                                                   results.white_frame, results.white_variance};
        VarianceUtils::updateDetection(detection, variance_tracker_,
                                       detect.boom_threshold, detect.boom_confirmation,
                                       detect.white_threshold, detect.white_confirmation);
        results.boom_frame = detection.boom_frame;
        results.boom_variance = detection.boom_variance;
        results.white_frame = detection.white_frame;
        results.white_variance = detection.white_variance;

        // Early stop if white was newly detected and configured
        if (!had_white && results.white_frame.has_value() && detect.early_stop_after_white) {
            results.frames_completed = frame + 1;
            early_stopped = true;
            break;
        }

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
                             config_.post_process.normalization,
                             pendulum_count);
        max_values.push_back(renderer_.lastMax());

        // Update analysis with GPU stats (brightness)
        if (config_.analysis.enabled) {
            analysis_tracker_.updateGPUStats(renderer_.lastMax(), renderer_.lastBrightness());
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

    auto total_time = Clock::now() - total_start;

    // Populate timing results
    results.timing.total_seconds = Duration(total_time).count();
    results.timing.physics_seconds = physics_time.count();
    results.timing.render_seconds = render_time.count();
    results.timing.io_seconds = io_time.count();

    results.variance_history = variance_tracker_.getHistory();
    results.spread_history = variance_tracker_.getSpreadHistory();
    results.final_spread_ratio = variance_tracker_.getFinalSpread().spread_ratio;

    // Save metadata, config copy, and statistics
    saveMetadata(results);
    if (!config_path.empty()) {
        saveConfigCopy(config_path);
    }
    // Save extended analysis or basic variance depending on mode
    if (config_.analysis.enabled && !analysis_tracker_.getHistory().empty()) {
        saveAnalysisCSV(analysis_tracker_.getHistory());
    } else if (!results.variance_history.empty()) {
        saveVarianceCSV(results.variance_history, max_values, results.spread_history);
    }

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
        std::cout << "=== Simulation Stopped Early (white detected) ===\n";
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
    std::cout << "Video:       " << video_duration << "s @ " << config_.output.video_fps
              << " FPS (" << simulation_speed << "x speed)\n";
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

    std::cout << std::setprecision(4);
    if (results.boom_frame) {
        std::cout << "Boom:        frame " << *results.boom_frame
                  << " (var=" << results.boom_variance << ")\n";
    }
    if (results.white_frame) {
        std::cout << "White:       frame " << *results.white_frame
                  << " (var=" << results.white_variance << ")\n";
    }
    std::cout << "Spread:      " << std::setprecision(2) << (results.final_spread_ratio * 100)
              << "% above horizontal\n";

    std::cout << "\nOutput: " << output_path << "\n";

    return results;
}

ProbeResults Simulation::runProbe(ProgressCallback progress) {
    // Physics-only simulation for parameter evaluation
    // No GL, no rendering, no I/O - just physics + variance/spread tracking

    ProbeResults results;

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

    // Reset variance tracker
    variance_tracker_.reset();

    // Detection thresholds from config
    auto const& detect = config_.detection;
    VarianceUtils::ThresholdResults detection;

    // Time per frame for boom_seconds calculation
    double const frame_duration = config_.simulation.duration_seconds / total_frames;

    // Main physics loop
    for (int frame = 0; frame < total_frames; ++frame) {
        // Step physics
        stepPendulums(pendulums, states, substeps, dt, thread_count);

        // Extract angles for variance and spread tracking
        std::vector<double> angle1s, angle2s;
        angle1s.reserve(pendulum_count);
        angle2s.reserve(pendulum_count);
        for (auto const& state : states) {
            angle1s.push_back(state.th1);
            angle2s.push_back(state.th2);
        }

        // Update variance and spread tracking
        variance_tracker_.updateWithSpread(angle2s, angle1s);

        // Update boom/white detection
        VarianceUtils::updateDetection(detection, variance_tracker_, detect.boom_threshold,
                                       detect.boom_confirmation, detect.white_threshold,
                                       detect.white_confirmation);

        // Store boom info as soon as detected
        if (detection.boom_frame.has_value() && !results.boom_frame.has_value()) {
            results.boom_frame = detection.boom_frame;
            results.boom_variance = detection.boom_variance;
            results.boom_seconds = *detection.boom_frame * frame_duration;
        }

        results.frames_completed = frame + 1;

        // Progress callback
        if (progress) {
            progress(frame + 1, total_frames);
        }
    }

    // Populate final results
    results.success = true;
    results.final_variance = variance_tracker_.getCurrentVariance();

    // Get final spread metrics
    SpreadMetrics const& spread = variance_tracker_.getFinalSpread();
    results.final_spread_ratio = spread.spread_ratio;
    results.angle1_mean = spread.angle1_mean;
    results.angle1_variance = spread.angle1_variance;

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
