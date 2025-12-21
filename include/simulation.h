#pragma once

#include "config.h"
#include "pendulum.h"
#include "renderer.h"
#include "color_scheme.h"
#include "post_process.h"
#include "boom_detector.h"
#include "video_writer.h"

#include <vector>
#include <thread>
#include <functional>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Progress callback: (current_frame, total_frames)
using ProgressCallback = std::function<void(int, int)>;

class Simulation {
public:
    explicit Simulation(Config const& config)
        : config_(config),
          renderer_(config.render.width, config.render.height),
          color_gen_(config.color),
          post_processor_(config.post_process),
          boom_detector_(config.boom_detection) {}

    // Run simulation with streaming mode (memory efficient)
    void run(ProgressCallback progress = nullptr) {
        int const pendulum_count = config_.simulation.pendulum_count;
        int const total_frames = config_.simulation.total_frames();
        int const substeps = config_.simulation.substeps_per_frame;
        double const dt = config_.simulation.dt();
        int const width = config_.render.width;
        int const height = config_.render.height;

        // Determine thread count
        int thread_count = config_.render.thread_count;
        if (thread_count <= 0) {
            thread_count = std::thread::hardware_concurrency();
        }

        // Create output directory
        std::filesystem::create_directories(config_.output.directory);

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
            std::string video_path = config_.output.directory + "/" +
                                     config_.output.filename_prefix + ".mp4";
            video_writer = std::make_unique<VideoWriter>(
                width, height, config_.simulation.fps, config_.output);
            if (!video_writer->open(video_path)) {
                std::cerr << "Failed to open video writer\n";
                return;
            }
        }

        // Allocate buffers
        std::vector<PendulumState> states(pendulum_count);
        Image image(width, height);
        std::vector<uint8_t> rgb_buffer(width * height * 3);

        // Main simulation loop (streaming mode)
        for (int frame = 0; frame < total_frames; ++frame) {
            // Step all pendulums (parallel)
            stepPendulums(pendulums, states, substeps, dt, thread_count);

            // Boom detection
            if (boom_detector_.isEnabled()) {
                std::vector<double> angles;
                angles.reserve(pendulum_count);
                for (auto const& state : states) {
                    angles.push_back(state.th2);  // Track second pendulum angle
                }

                auto boom = boom_detector_.update(angles, frame);
                if (boom) {
                    std::cout << "Boom detected at frame " << boom->frame
                              << " (variance: " << boom->variance << ")\n";
                    if (boom_detector_.shouldEarlyStop()) {
                        std::cout << "Early stopping...\n";
                        break;
                    }
                }
            }

            // Render frame
            image.clear();
            for (int i = 0; i < pendulum_count; ++i) {
                renderer_.render_pendulum(image, states[i], colors[i]);
            }

            // Post-process
            post_processor_.apply(image);

            // Output
            image.to_rgb8(rgb_buffer);

            if (config_.output.format == OutputFormat::Video) {
                video_writer->writeFrame(rgb_buffer.data());
            } else {
                savePNG(rgb_buffer, width, height, frame);
            }

            // Progress
            if (progress) {
                progress(frame + 1, total_frames);
            } else {
                std::cout << "\rFrame " << std::setw(4) << (frame + 1)
                          << "/" << total_frames << std::flush;
            }
        }

        if (video_writer) {
            video_writer->close();
        }

        std::cout << "\nSimulation complete.\n";

        if (boom_detector_.hasBoomOccurred()) {
            std::cout << "Boom occurred at frame: " << *boom_detector_.getBoomFrame() << "\n";
        }
    }

private:
    Config config_;
    Renderer renderer_;
    ColorSchemeGenerator color_gen_;
    PostProcessor post_processor_;
    BoomDetector boom_detector_;

    void initializePendulums(std::vector<Pendulum>& pendulums) {
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

    void stepPendulums(std::vector<Pendulum>& pendulums,
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

    void savePNG(std::vector<uint8_t> const& data, int width, int height, int frame) {
        std::ostringstream path;
        path << config_.output.directory << "/"
             << config_.output.filename_prefix
             << std::setfill('0') << std::setw(4) << frame << ".png";

        stbi_write_png(path.str().c_str(), width, height, 3, data.data(), width * 3);
    }
};
