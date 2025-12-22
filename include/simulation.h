#pragma once

#include "analysis_tracker.h"
#include "color_scheme.h"
#include "config.h"
#include "gl_renderer.h"
#include "headless_gl.h"
#include "pendulum.h"
#include "probe_results.h"
#include "variance_tracker.h"
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

// Scoring results for quality ranking
struct SimulationScore {
    double peak_causticness = 0.0;    // Maximum causticness value observed
    double average_causticness = 0.0; // Average over sample points
    int best_frame = -1;              // Frame with peak causticness
    std::vector<double> samples;      // Causticness at 0.5s intervals after boom
};

// Simulation results
struct SimulationResults {
    int frames_completed = 0;
    std::optional<int> boom_frame;
    double boom_variance = 0.0;
    std::optional<int> white_frame;
    double white_variance = 0.0;
    double final_spread_ratio = 0.0; // Fraction of pendulums above horizontal at end
    TimingStats timing;
    std::vector<double> variance_history;
    std::vector<SpreadMetrics> spread_history;
    std::string output_directory; // Where video/frames were saved
    std::string video_path;       // Full path to video (if format is video)
    SimulationScore score;        // Quality score for ranking
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
    ProbeResults runProbe(ProgressCallback progress = nullptr);

private:
    Config config_;
    HeadlessGL gl_;
    GLRenderer renderer_;
    ColorSchemeGenerator color_gen_;
    VarianceTracker variance_tracker_;
    AnalysisTracker analysis_tracker_;
    std::string run_directory_;

    void initializePendulums(std::vector<Pendulum>& pendulums);

    void stepPendulums(std::vector<Pendulum>& pendulums, std::vector<PendulumState>& states,
                       int substeps, double dt, int thread_count);

    void savePNG(std::vector<uint8_t> const& data, int width, int height, int frame);

    // Output directory management
    std::string createRunDirectory();
    void saveConfigCopy(std::string const& config_path);
    void saveMetadata(SimulationResults const& results);
    void saveVarianceCSV(std::vector<double> const& variance, std::vector<float> const& max_values,
                         std::vector<SpreadMetrics> const& spread);
    void saveAnalysisCSV(std::vector<FrameAnalysis> const& analysis);
};
