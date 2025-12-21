#pragma once

#include "config.h"
#include "pendulum.h"
#include "renderer.h"
#include "color_scheme.h"
#include "post_process.h"
#include "boom_detector.h"
#include "video_writer.h"

#include <vector>
#include <functional>

// Progress callback: (current_frame, total_frames)
using ProgressCallback = std::function<void(int, int)>;

// Timing results for profiling
struct TimingStats {
    double total_seconds = 0.0;
    double physics_seconds = 0.0;
    double render_seconds = 0.0;
    double io_seconds = 0.0;
};

// Simulation results
struct SimulationResults {
    int frames_completed = 0;
    std::optional<int> boom_frame;
    double boom_variance = 0.0;
    std::optional<int> white_frame;
    double white_variance = 0.0;
    TimingStats timing;
    std::vector<double> variance_history;
};

class Simulation {
public:
    explicit Simulation(Config const& config);

    // Run simulation with streaming mode (memory efficient)
    void run(ProgressCallback progress = nullptr);

private:
    Config config_;
    Renderer renderer_;
    ColorSchemeGenerator color_gen_;
    PostProcessor post_processor_;
    BoomDetector boom_detector_;
    std::string run_directory_;

    void initializePendulums(std::vector<Pendulum>& pendulums);

    void stepPendulums(std::vector<Pendulum>& pendulums,
                       std::vector<PendulumState>& states,
                       int substeps, double dt, int thread_count);

    void savePNG(std::vector<uint8_t> const& data, int width, int height, int frame);

    // Output directory management
    std::string createRunDirectory();
    void saveConfigCopy(std::string const& config_path);
    void saveMetadata(SimulationResults const& results);
    void saveVarianceCSV(std::vector<double> const& variance);
};
