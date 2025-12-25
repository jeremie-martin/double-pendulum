#pragma once

#include "config.h"
#include "metrics/probe_filter.h"
#include "metrics/probe_pipeline.h"
#include "music_manager.h"
#include "optimize/prediction_target.h"
#include "preset_library.h"

#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

// =============================================================================
// Target-Based Filter Constraints
// =============================================================================
//
// Constraints for prediction targets defined in [targets.X] sections.
// These link filter criteria to specific targets by name.
//
// Example TOML:
//   [filter.targets.boom]
//   min_seconds = 7.0
//   max_seconds = 14.0
//   required = true
//
//   [filter.targets.boom_quality]
//   min_score = 0.6
//
// =============================================================================
struct TargetConstraint {
    std::string target_name;  // Links to [targets.X] by name

    // For frame targets (boom, chaos)
    std::optional<double> min_seconds;
    std::optional<double> max_seconds;

    // For score targets (boom_quality)
    std::optional<double> min_score;
    std::optional<double> max_score;

    // Common
    bool required = false;  // If true, target must produce a valid result
};

// =============================================================================
// Filter Criteria for Batch Probe Validation
// =============================================================================
//
// This is the config-level filter specification, parsed from TOML [filter] section.
// It maps to metrics::ProbeFilter for actual evaluation.
//
// HOW IT WORKS:
// 1. User specifies criteria in batch config TOML:
//      [filter]
//      min_uniformity = 0.9
//      require_valid_music = true
//
//      [filter.targets.boom]
//      min_seconds = 7.0
//      max_seconds = 14.0
//      required = true
//
// 2. BatchConfig::load() parses these into a FilterCriteria struct
//
// 3. FilterCriteria::toProbeFilter() converts to metrics::ProbeFilter
//
// 4. During probe phase, filter.evaluate() with predictions determines pass/fail
//
// CRITERIA TYPES:
//   [filter.targets.X]   → Target constraint (references [targets.X])
//   min_uniformity       → Metric threshold (final uniformity value)
//   require_valid_music  → Music sync check (handled separately)
//
// =============================================================================
struct FilterCriteria {
    // Target-based constraints (new system)
    std::vector<TargetConstraint> target_constraints;

    // General constraints (not target-based)
    double min_uniformity = 0.0;     // Minimum uniformity (0 = no requirement, 0.9 recommended)
    bool require_valid_music = false; // Fail if no music track has drop > boom time

    // Check if filtering is enabled (any non-default values)
    bool isEnabled() const {
        return !target_constraints.empty() || min_uniformity > 0.0;
    }

    // Convert to metrics::ProbeFilter
    metrics::ProbeFilter toProbeFilter() const {
        metrics::ProbeFilter filter;

        // General constraints
        if (min_uniformity > 0.0) {
            filter.addMetricThreshold(metrics::MetricNames::CircularSpread, min_uniformity);
        }

        // Target constraints
        for (auto const& tc : target_constraints) {
            filter.addTargetConstraint(tc.target_name, tc.required,
                                        tc.min_seconds, tc.max_seconds,
                                        tc.min_score, tc.max_score);
        }

        return filter;
    }
};

// Batch configuration loaded from TOML
struct BatchConfig {
    std::string output_directory = "batch_output";
    int count = 10;

    // Physics parameter ranges for randomization
    struct Range {
        double min = 0.0;
        double max = 0.0;
    };

    Range angle1_range{-180.0, 180.0};
    Range angle2_range{-180.0, 180.0};
    Range variation_range{0.05, 0.2};
    Range velocity1_range{0.0, 0.0};  // Initial angular velocity ranges (rad/s)
    Range velocity2_range{0.0, 0.0};  // Zero range = no randomization

    // Base config (for parameters not being varied)
    Config base_config;

    // Music settings
    std::string music_database = "music";
    bool random_music = true;
    std::string fixed_track_id; // If random_music is false

    // Probe settings for pre-filtering
    int probe_pendulum_count = 1000;  // Pendulum count for fast probing
    int probe_total_frames = 0;       // Frame count for probing (0 = use base_config)
    double probe_max_dt = 0.0;        // Max timestep for probing (0 = use base_config)
    int max_probe_retries = 10;       // Max retries before giving up on a slot
    bool probe_enabled = false;       // Enable probe-based filtering

    // Filter criteria for probe validation
    FilterCriteria filter;

    // Preset library (loaded from separate file)
    PresetLibrary presets;

    // Names of presets to randomly select from (empty = use base_config)
    std::vector<std::string> color_preset_names;
    std::vector<std::string> post_process_preset_names;

    static BatchConfig load(std::string const& path);
};

// Result of a single run (for summary)
struct RunResult {
    std::string name;           // Folder name or video_XXXX
    std::string video_path;     // Path to video file
    bool success = false;
    std::optional<int> boom_frame;
    double boom_seconds = 0.0;
    std::optional<int> chaos_frame;   // Frame when chaos detected
    double chaos_seconds = 0.0;       // Time when chaos detected
    double boom_quality = 0.0;        // Quality score (0-1)
    double duration_seconds = 0.0;
    double final_uniformity = 0.0;    // Uniformity at end of simulation (0=concentrated, 1=uniform)
    int probe_retries = 0;            // Number of probe retries before success
    double simulation_speed = 1.0;    // Real-time multiplier (physics_time / video_time)
};

// Progress tracking for batch operations
struct BatchProgress {
    int total = 0;
    int completed = 0;
    int failed = 0;
    std::vector<std::string> completed_ids;
    std::vector<std::string> failed_ids;
    std::vector<RunResult> results;  // Detailed results for summary

    void save(std::filesystem::path const& path) const;
    static BatchProgress load(std::filesystem::path const& path);
};

// Batch generator for mass production
class BatchGenerator {
public:
    explicit BatchGenerator(BatchConfig const& config);

    // Run full batch from start
    void run();

    // Resume from progress file
    void resume();

private:
    BatchConfig config_;
    MusicManager music_;
    std::mt19937 rng_;
    std::filesystem::path batch_dir_;
    BatchProgress progress_;
    metrics::ProbeFilter filter_;

    // Setup batch directory
    void setupBatchDirectory();

    // Generate a single video
    bool generateOne(int index);

    // Generate randomized config
    Config generateRandomConfig();

    // Run probe simulation and check if it passes filter criteria
    // Returns pair of (passes, probe_results)
    std::pair<bool, metrics::ProbePhaseResults> runProbe(Config const& config);

    // Pick a random music track (legacy, doesn't check boom timing)
    std::optional<MusicTrack> pickMusicTrack();

    // Pick a music track where drop_time > boom_seconds
    // Returns nullopt if no valid track found (triggers video failure/retry)
    std::optional<MusicTrack> pickMusicTrackForBoom(double boom_seconds);

    // Save progress after each video
    void saveProgress();

    // Load existing progress for resume
    bool loadProgress();

    // Create symlink to video in batch root folder
    void createVideoSymlink(std::string const& video_path, std::string const& link_name);

    // Print batch completion summary
    void printSummary() const;
};
