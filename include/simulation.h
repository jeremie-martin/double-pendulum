#pragma once

#include "color_scheme.h"
#include "config.h"
#include "gl_renderer.h"
#include "headless_gl.h"
#include "metrics/boom_analyzer.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "metrics/probe_pipeline.h"
#include "pendulum.h"
#include "video_writer.h"

#include <functional>
#include <optional>
#include <vector>

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
    std::optional<int> chaos_frame;  // Renamed from white_frame
    double chaos_variance = 0.0;     // Renamed from white_variance
    double final_spread_ratio = 0.0; // Fraction of pendulums above horizontal at end
    TimingStats timing;
    std::vector<double> variance_history;
    std::vector<metrics::SpreadMetrics> spread_history;
    std::string output_directory; // Where video/frames were saved
    std::string video_path;       // Full path to video (if format is video)
    metrics::SimulationScore score;  // Quality scores from analyzers
};

class Simulation {
public:
    explicit Simulation(Config const& config);
    ~Simulation();

    // Run simulation with GPU rendering
    // Returns simulation results including boom_frame and output paths
    // If config_path is provided, the config file will be copied to the output directory
    SimulationResults run(ProgressCallback progress = nullptr, std::string const& config_path = "");

    // Run probe simulation (physics only, no rendering)
    // Used for quick parameter evaluation before committing to full render
    // Much faster than run() since it skips GL initialization and all I/O
    metrics::ProbePhaseResults runProbe(ProgressCallback progress = nullptr);

private:
    Config config_;
    HeadlessGL gl_;
    GLRenderer renderer_;
    ColorSchemeGenerator color_gen_;
    metrics::MetricsCollector metrics_collector_;
    metrics::EventDetector event_detector_;
    metrics::BoomAnalyzer boom_analyzer_;
    metrics::CausticnessAnalyzer causticness_analyzer_;
    std::string run_directory_;

    void initializePendulums(std::vector<Pendulum>& pendulums);

    void stepPendulums(std::vector<Pendulum>& pendulums, std::vector<PendulumState>& states,
                       int substeps, double dt, int thread_count);

    void savePNG(std::vector<uint8_t> const& data, int width, int height, int frame);

    // Output directory management
    std::string createRunDirectory();
    void saveConfigCopy(std::string const& config_path);
    void saveMetadata(SimulationResults const& results);
    void saveMetricsCSV();
};
