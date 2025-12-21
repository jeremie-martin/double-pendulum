#include "simulation.h"

#include <thread>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <memory>
#include <chrono>
#include <fstream>
#include <ctime>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;

Simulation::Simulation(Config const& config)
    : config_(config),
      renderer_(config.render.width, config.render.height),
      color_gen_(config.color),
      post_processor_(config.post_process) {}

std::string Simulation::createRunDirectory() {
    // Generate timestamp-based directory name
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);

    std::ostringstream dir_name;
    dir_name << config_.output.directory << "/run_"
             << std::put_time(&tm, "%Y%m%d_%H%M%S");

    std::string path = dir_name.str();
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
        std::filesystem::copy_file(
            original_path,
            run_directory_ + "/config.toml",
            std::filesystem::copy_options::overwrite_existing
        );
    }
}

void Simulation::saveMetadata(SimulationResults const& results) {
    std::ofstream out(run_directory_ + "/metadata.json");
    if (!out) return;

    // Get current time as ISO string
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    std::ostringstream time_str;
    time_str << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");

    out << std::fixed << std::setprecision(4);
    out << "{\n";
    out << "  \"version\": \"1.0\",\n";
    out << "  \"created_at\": \"" << time_str.str() << "\",\n";
    out << "  \"config\": {\n";
    out << "    \"duration_seconds\": " << config_.simulation.duration_seconds << ",\n";
    out << "    \"total_frames\": " << config_.simulation.total_frames << ",\n";
    out << "    \"video_fps\": " << config_.output.video_fps << ",\n";
    out << "    \"pendulum_count\": " << config_.simulation.pendulum_count << ",\n";
    out << "    \"width\": " << config_.render.width << ",\n";
    out << "    \"height\": " << config_.render.height << "\n";
    out << "  },\n";
    out << "  \"results\": {\n";
    out << "    \"frames_completed\": " << results.frames_completed << ",\n";
    if (results.boom_frame) {
        out << "    \"boom_frame\": " << *results.boom_frame << ",\n";
        out << "    \"boom_variance\": " << results.boom_variance << ",\n";
    } else {
        out << "    \"boom_frame\": null,\n";
        out << "    \"boom_variance\": null,\n";
    }
    if (results.white_frame) {
        out << "    \"white_frame\": " << *results.white_frame << ",\n";
        out << "    \"white_variance\": " << results.white_variance << "\n";
    } else {
        out << "    \"white_frame\": null,\n";
        out << "    \"white_variance\": null\n";
    }
    out << "  },\n";
    out << "  \"timing\": {\n";
    out << "    \"total_seconds\": " << results.timing.total_seconds << ",\n";
    out << "    \"physics_seconds\": " << results.timing.physics_seconds << ",\n";
    out << "    \"render_seconds\": " << results.timing.render_seconds << ",\n";
    out << "    \"io_seconds\": " << results.timing.io_seconds << "\n";
    out << "  }\n";
    out << "}\n";
}

void Simulation::saveVarianceCSV(std::vector<double> const& variance) {
    std::ofstream out(run_directory_ + "/variance.csv");
    if (!out) return;

    out << "frame,variance\n";
    out << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < variance.size(); ++i) {
        out << i << "," << variance[i] << "\n";
    }
}

void Simulation::run(ProgressCallback progress) {
    int const pendulum_count = config_.simulation.pendulum_count;
    int const total_frames = config_.simulation.total_frames;
    int const substeps = config_.simulation.substeps_per_frame;
    double const dt = config_.simulation.dt();
    int const width = config_.render.width;
    int const height = config_.render.height;

    // Create run directory with timestamp
    run_directory_ = createRunDirectory();
    std::cout << "Output directory: " << run_directory_ << "\n";

    // Timing accumulators
    Duration physics_time{0};
    Duration render_time{0};
    Duration io_time{0};
    auto total_start = Clock::now();

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
        video_writer = std::make_unique<VideoWriter>(
            width, height, config_.output.video_fps, config_.output);
        if (!video_writer->open(video_path)) {
            std::cerr << "Failed to open video writer\n";
            return;
        }
    }

    // Allocate buffers
    std::vector<PendulumState> states(pendulum_count);
    Image image(width, height);
    std::vector<uint8_t> rgb_buffer(width * height * 3);

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

        // Track variance (always enabled - cheap operation)
        std::vector<double> angles;
        angles.reserve(pendulum_count);
        for (auto const& state : states) {
            angles.push_back(state.th2);  // Track second pendulum angle
        }
        variance_tracker_.update(angles);

        // Check for boom detection (external logic)
        if (!results.boom_frame.has_value()) {
            int boom = VarianceUtils::checkThresholdCrossing(
                variance_tracker_.getHistory(),
                detect.boom_threshold,
                detect.boom_confirmation
            );
            if (boom >= 0) {
                results.boom_frame = boom;
                results.boom_variance = variance_tracker_.getVarianceAt(boom);
            }
        }

        // Check for white detection (only after boom)
        if (results.boom_frame.has_value() && !results.white_frame.has_value()) {
            int white = VarianceUtils::checkThresholdCrossing(
                variance_tracker_.getHistory(),
                detect.white_threshold,
                detect.white_confirmation
            );
            if (white >= 0) {
                results.white_frame = white;
                results.white_variance = variance_tracker_.getVarianceAt(white);

                // Early stop if configured
                if (detect.early_stop_after_white) {
                    results.frames_completed = frame + 1;
                    early_stopped = true;
                    break;
                }
            }
        }

        // Render timing
        auto render_start = Clock::now();
        image.clear();
        for (int i = 0; i < pendulum_count; ++i) {
            renderer_.render_pendulum(image, states[i], colors[i]);
        }
        post_processor_.apply(image);
        image.to_rgb8(rgb_buffer);
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
            std::cout << "\rFrame " << std::setw(4) << (frame + 1)
                      << "/" << total_frames << std::flush;
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

    // Save metadata and variance
    saveMetadata(results);
    if (!results.variance_history.empty()) {
        saveVarianceCSV(results.variance_history);
    }

    // Determine output path for display
    std::string output_path;
    if (config_.output.format == OutputFormat::Video) {
        output_path = run_directory_ + "/video.mp4";
    } else {
        output_path = run_directory_ + "/frames/";
    }

    // Print results
    std::cout << "\n\n";
    if (early_stopped) {
        std::cout << "=== Simulation Stopped Early (white detected) ===\n";
    } else {
        std::cout << "=== Simulation Complete ===\n";
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Frames:      " << results.frames_completed << "/" << total_frames;
    if (early_stopped) std::cout << " (early stop)";
    std::cout << "\n";
    std::cout << "Pendulums:   " << pendulum_count << "\n";
    std::cout << "Total time:  " << results.timing.total_seconds << "s\n";
    std::cout << "  Physics:   " << std::setw(5) << results.timing.physics_seconds << "s ("
              << std::setw(4) << (results.timing.physics_seconds / results.timing.total_seconds * 100) << "%)\n";
    std::cout << "  Render:    " << std::setw(5) << results.timing.render_seconds << "s ("
              << std::setw(4) << (results.timing.render_seconds / results.timing.total_seconds * 100) << "%)\n";
    std::cout << "  I/O:       " << std::setw(5) << results.timing.io_seconds << "s ("
              << std::setw(4) << (results.timing.io_seconds / results.timing.total_seconds * 100) << "%)\n";

    std::cout << std::setprecision(4);
    if (results.boom_frame) {
        std::cout << "Boom:        frame " << *results.boom_frame
                  << " (var=" << results.boom_variance << ")\n";
    }
    if (results.white_frame) {
        std::cout << "White:       frame " << *results.white_frame
                  << " (var=" << results.white_variance << ")\n";
    }

    std::cout << "\nOutput: " << output_path << "\n";
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
            config_.physics.gravity,
            config_.physics.length1,
            config_.physics.length2,
            config_.physics.mass1,
            config_.physics.mass2,
            th1,
            config_.physics.initial_angle2,
            config_.physics.initial_velocity1,
            config_.physics.initial_velocity2
        );
    }
}

void Simulation::stepPendulums(std::vector<Pendulum>& pendulums,
                               std::vector<PendulumState>& states,
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
    path << run_directory_ << "/frames/"
         << config_.output.filename_prefix
         << std::setfill('0') << std::setw(4) << frame << ".png";

    stbi_write_png(path.str().c_str(), width, height, 3, data.data(), width * 3);
}
