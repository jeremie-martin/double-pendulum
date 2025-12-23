#pragma once

#include "metrics/analyzer.h"
#include "metrics/boom_analyzer.h"
#include "metrics/boom_detection.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "metrics/probe_filter.h"

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

    // Scores from analyzers
    SimulationScore scores;

    // Quality metrics
    std::optional<BoomQuality> boom_quality;

    // Helpers
    bool hasBoom() const { return boom_frame.has_value(); }
    bool hasChaos() const { return chaos_frame.has_value(); }
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

    // Configure event detection
    void setBoomThreshold(double threshold);
    void setBoomConfirmation(int frames);
    void setChaosThreshold(double threshold);
    void setChaosConfirmation(int frames);

    // Configure analyzers
    void enableBoomAnalyzer(bool enable = true);
    void enableCausticnessAnalyzer(bool enable = true);
    void setFrameDuration(double seconds) { frame_duration_ = seconds; }

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

    // Detection parameters
    // Note: boom_threshold_ and boom_confirmation_ are deprecated (boom uses max causticness)
    double boom_threshold_ = 0.1;    // Unused, kept for API compatibility
    int boom_confirmation_ = 10;     // Unused
    double chaos_threshold_ = 700.0; // Used for chaos detection
    int chaos_confirmation_ = 10;

    // Internal state
    MetricsCollector collector_;
    EventDetector event_detector_;

    // Analyzers
    bool boom_analyzer_enabled_ = true;
    bool causticness_analyzer_enabled_ = true;
    std::unique_ptr<BoomAnalyzer> boom_analyzer_;
    std::unique_ptr<CausticnessAnalyzer> causticness_analyzer_;

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
