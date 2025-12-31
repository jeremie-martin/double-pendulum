#pragma once

#include "color_scheme.h"
#include "config.h"
#include "gl_renderer.h"
#include "headless_gl.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "metrics/probe_pipeline.h"
#include "optimize/prediction_target.h"
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
    double boom_causticness = 0.0;   // Peak causticness at boom frame
    std::optional<int> chaos_frame;
    double chaos_variance = 0.0;
    double final_uniformity = 0.0;   // Distribution uniformity (0=concentrated, 1=uniform on disk)
    TimingStats timing;
    std::vector<double> variance_history;
    std::vector<metrics::SpreadMetrics> spread_history;
    std::string output_directory; // Where video/frames were saved
    std::string video_path;       // Full path to video (if format is video)
    metrics::SimulationScore score;  // Quality scores from analyzers

    // Multi-target predictions (new)
    std::vector<optimize::PredictionResult> predictions;

    // Preset names for metadata (set by batch generator, empty for single-run)
    std::string color_preset_name;
    std::string post_process_preset_name;
    std::string theme_name;  // Set when using theme presets

    // Convenience accessors for backward compatibility
    std::optional<int> getBoomFrame() const {
        // First check predictions
        for (auto const& p : predictions) {
            if (p.target_name == "boom" && p.isFrame() && p.valid()) {
                return p.predicted_frame;
            }
        }
        // Fall back to legacy field
        return boom_frame;
    }

    std::optional<int> getChaosFrame() const {
        // First check predictions
        for (auto const& p : predictions) {
            if (p.target_name == "chaos" && p.isFrame() && p.valid()) {
                return p.predicted_frame;
            }
        }
        // Fall back to legacy field
        return chaos_frame;
    }

    std::optional<double> getBoomQuality() const {
        for (auto const& p : predictions) {
            if (p.target_name == "boom_quality" && p.isScore()) {
                return p.predicted_score;
            }
        }
        return std::nullopt;
    }
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

    // Run probe and collect all pendulum states for ML-based boom detection
    // Returns state data as contiguous float array: [frames][pendulums][8]
    // 8 values per pendulum: x1, y1, x2, y2, th1, th2, w1, w2
    // This is used by phase 2 probe to send data to the boom detection server
    std::vector<float> runProbeCollectStates(ProgressCallback progress = nullptr);

private:
    Config config_;
    HeadlessGL gl_;
    GLRenderer renderer_;
    ColorSchemeGenerator color_gen_;
    metrics::MetricsCollector metrics_collector_;
    metrics::EventDetector event_detector_;
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
