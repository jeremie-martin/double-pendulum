#pragma once

#include "metrics/analyzer.h"
#include "metrics/boom_detection.h"
#include "metrics/signal_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "metrics/probe_filter.h"
#include "optimize/prediction_target.h"
#include "pendulum.h"  // For PendulumState in feedPhysicsFrame

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace metrics {

// Configuration for a probe phase
struct ProbePhaseConfig {
    bool enabled = true;
    int pendulum_count = 1000;
    int total_frames = 0;     // 0 = use base config
    double max_dt = 0.0;      // 0 = use base config

    // For render phase
    bool has_rendering = false;
    int render_width = 270;
    int render_height = 270;

    // Early termination (designed for future)
    bool early_exit_after_boom = false;
    int frames_after_boom = 30;
};

// Results from a probe phase
struct ProbePhaseResults {
    bool completed = false;
    bool passed_filter = false;
    std::string rejection_reason;

    // Frame info
    int frames_completed = 0;
    double duration_seconds = 0.0;

    // Events
    std::optional<int> boom_frame;
    double boom_seconds = 0.0;
    std::optional<int> chaos_frame;
    double chaos_seconds = 0.0;

    // Final metrics
    double final_variance = 0.0;
    double final_uniformity = 0.0;  // Distribution uniformity (0=concentrated, 1=uniform)

    // Scores from analyzers (matches SimulationResults.score naming)
    SimulationScore score;

    // Multi-target predictions (new)
    std::vector<optimize::PredictionResult> predictions;

    // Helpers
    bool hasBoom() const { return boom_frame.has_value(); }
    bool hasChaos() const { return chaos_frame.has_value(); }

    // Get prediction by target name
    std::optional<optimize::PredictionResult> getPrediction(std::string const& name) const {
        for (auto const& p : predictions) {
            if (p.target_name == name) return p;
        }
        return std::nullopt;
    }
};

// Multi-phase probe pipeline
class ProbePipeline {
public:
    using ProgressCallback = std::function<void(int frame, int total)>;
    using TerminationCheck = std::function<bool()>;

    ProbePipeline();
    ~ProbePipeline();

    // Configure phases
    void setPhase1Config(ProbePhaseConfig const& config);
    void setPhase2Config(ProbePhaseConfig const& config);

    // Configure filtering
    void setPhase1Filter(ProbeFilter const& filter);
    void setPhase2Filter(ProbeFilter const& filter);

    // Configure event detection (boom uses max causticness, not threshold)
    void setChaosThreshold(double threshold);
    void setChaosConfirmation(int frames);
    void setBoomParams(optimize::FrameDetectionParams const& params);

    // Multi-target prediction configuration (new)
    // If targets are set, these override the boom_params for predictions
    void setTargets(std::vector<optimize::PredictionTarget> const& targets) {
        prediction_targets_ = targets;
    }
    std::vector<optimize::PredictionTarget> const& getTargets() const {
        return prediction_targets_;
    }

    // Configure analyzers
    void enableSignalAnalyzer(bool enable = true);
    // Legacy alias
    void enableCausticnessAnalyzer(bool enable = true) { enableSignalAnalyzer(enable); }
    // Set frame duration for time-based calculations.
    // Must be positive for boom detection to work in finalizePhase().
    void setFrameDuration(double seconds) {
        if (seconds > 0.0) {
            frame_duration_ = seconds;
        }
    }

    // Callbacks
    void setProgressCallback(ProgressCallback callback);
    void setTerminationCheck(TerminationCheck callback);

    // Get current configuration
    ProbePhaseConfig const& getPhase1Config() const { return phase1_config_; }
    ProbePhaseConfig const& getPhase2Config() const { return phase2_config_; }
    bool isPhase2Enabled() const { return phase2_config_.enabled; }

    // Access internal components (for advanced use)
    MetricsCollector& getCollector() { return collector_; }
    EventDetector& getEventDetector() { return event_detector_; }

    // Run the pipeline
    // Phase 1: Physics-only simulation
    // Phase 2 (optional): Low-res render simulation
    // Returns results from the final phase
    ProbePhaseResults run(/* would need simulation parameters */);

    // Run individual phases (for external simulation control)
    void reset();
    void beginPhase1();
    void beginPhase2();

    // Feed frame data to the pipeline
    // Prefer the PendulumState version for full metrics including spatial_concentration
    void feedPhysicsFrame(std::vector<PendulumState> const& states,
                          double total_energy = 0.0);
    // Legacy angle-only version (does not compute position-based metrics)
    void feedPhysicsFrame(std::vector<double> const& angle1s,
                          std::vector<double> const& angle2s,
                          double total_energy = 0.0);
    void feedGPUFrame(GPUMetricsBundle const& gpu_metrics);

    // Finalize a phase and get results
    ProbePhaseResults finalizePhase();

    // Get intermediate results
    MetricsCollector const& getMetrics() const { return collector_; }
    EventDetector const& getEvents() const { return event_detector_; }
    SimulationScore getScores() const;

private:
    // Configuration
    ProbePhaseConfig phase1_config_;
    ProbePhaseConfig phase2_config_;
    ProbeFilter phase1_filter_;
    ProbeFilter phase2_filter_;

    // Detection parameters (boom uses max causticness, not threshold)
    double chaos_threshold_ = 700.0;
    int chaos_confirmation_ = 10;
    optimize::FrameDetectionParams boom_params_;  // Frame detection config for boom

    // Multi-target predictions (new)
    std::vector<optimize::PredictionTarget> prediction_targets_;

    // Internal state
    MetricsCollector collector_;
    EventDetector event_detector_;

    // Analyzers
    bool signal_analyzer_enabled_ = true;
    std::unique_ptr<SignalAnalyzer> signal_analyzer_;

    // Callbacks
    ProgressCallback progress_callback_;
    TerminationCheck termination_check_;

    // Current phase
    int current_phase_ = 0;
    int current_frame_ = 0;
    double frame_duration_ = 0.0;

    void setupEventDetector();
    void runAnalyzers();
    ProbePhaseResults buildResults(ProbeFilter const& filter);
};

} // namespace metrics
