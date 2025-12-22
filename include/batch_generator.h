#pragma once

#include "config.h"
#include "music_manager.h"
#include "preset_library.h"
#include "probe_results.h"

#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Filter criteria for probe validation
// Used to reject simulations with unsuitable parameters before full rendering
struct FilterCriteria {
    double min_boom_seconds = 0.0; // Minimum boom time (0 = no minimum)
    double max_boom_seconds = 0.0; // Maximum boom time (0 = no maximum)
    double min_spread_ratio = 0.0; // Minimum spread ratio (0 = no requirement)
    bool require_boom = true;      // Reject simulations with no detectable boom
    bool require_valid_music = true; // Fail if no music track has drop > boom time

    // Check if filtering is enabled (any non-default values)
    bool isEnabled() const {
        return min_boom_seconds > 0.0 || max_boom_seconds > 0.0 || min_spread_ratio > 0.0 ||
               require_boom;
    }
};

// Evaluates probe results against filter criteria
class ProbeFilter {
public:
    explicit ProbeFilter(FilterCriteria const& criteria) : criteria_(criteria) {}

    // Check if probe results pass all filter criteria
    bool passes(ProbeResults const& results) const {
        // Check boom requirement
        if (criteria_.require_boom && !results.boom_frame.has_value()) {
            return false;
        }

        // Check boom time range
        if (results.boom_frame.has_value()) {
            if (criteria_.min_boom_seconds > 0.0 &&
                results.boom_seconds < criteria_.min_boom_seconds) {
                return false;
            }
            if (criteria_.max_boom_seconds > 0.0 &&
                results.boom_seconds > criteria_.max_boom_seconds) {
                return false;
            }
        }

        // Check spread ratio
        if (criteria_.min_spread_ratio > 0.0 &&
            results.final_spread_ratio < criteria_.min_spread_ratio) {
            return false;
        }

        return true;
    }

    // Get human-readable rejection reason
    std::string rejectReason(ProbeResults const& results) const {
        if (criteria_.require_boom && !results.boom_frame.has_value()) {
            return "no boom detected";
        }

        if (results.boom_frame.has_value()) {
            if (criteria_.min_boom_seconds > 0.0 &&
                results.boom_seconds < criteria_.min_boom_seconds) {
                return "boom too early (" + std::to_string(results.boom_seconds) + "s < " +
                       std::to_string(criteria_.min_boom_seconds) + "s)";
            }
            if (criteria_.max_boom_seconds > 0.0 &&
                results.boom_seconds > criteria_.max_boom_seconds) {
                return "boom too late (" + std::to_string(results.boom_seconds) + "s > " +
                       std::to_string(criteria_.max_boom_seconds) + "s)";
            }
        }

        if (criteria_.min_spread_ratio > 0.0 &&
            results.final_spread_ratio < criteria_.min_spread_ratio) {
            return "spread too low (" + std::to_string(results.final_spread_ratio) + " < " +
                   std::to_string(criteria_.min_spread_ratio) + ")";
        }

        return "unknown";
    }

private:
    FilterCriteria criteria_;
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
    double duration_seconds = 0.0;
    double final_spread_ratio = 0.0;  // Spread at end of simulation
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
    ProbeFilter filter_;

    // Setup batch directory
    void setupBatchDirectory();

    // Generate a single video
    bool generateOne(int index);

    // Generate randomized config
    Config generateRandomConfig();

    // Run probe simulation and check if it passes filter criteria
    // Returns pair of (passes, probe_results)
    std::pair<bool, ProbeResults> runProbe(Config const& config);

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
