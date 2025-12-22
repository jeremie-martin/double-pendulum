#pragma once

#include "config.h"
#include "music_manager.h"

#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Batch generation mode
enum class BatchMode {
    Random, // Random sampling from ranges (original behavior)
    Grid    // Cartesian product of discrete value sets
};

// Grid configuration for parameter sweeps
struct GridConfig {
    // Map of parameter paths to discrete values
    // e.g., {"simulation.pendulum_count": ["10000", "100000"]}
    std::map<std::string, std::vector<std::string>> param_sets;

    // Compute all combinations (Cartesian product)
    std::vector<std::map<std::string, std::string>> expandCombinations() const;

    // Total number of combinations
    size_t totalCombinations() const;
};

// Batch configuration loaded from TOML
struct BatchConfig {
    std::string output_directory = "batch_output";
    int count = 10; // Only used in Random mode

    // Batch mode
    BatchMode mode = BatchMode::Random;

    // Grid configuration (for Grid mode)
    GridConfig grid;

    // Physics parameter ranges (for Random mode)
    struct Range {
        double min = 0.0;
        double max = 0.0;
    };

    Range angle1_range{-180.0, 180.0};
    Range angle2_range{-180.0, 180.0};
    Range variation_range{0.05, 0.2};

    // Base config (for parameters not being varied)
    Config base_config;

    // Music settings
    std::string music_database = "music";
    bool random_music = true;
    std::string fixed_track_id; // If random_music is false

    static BatchConfig load(std::string const& path);
};

// Result of a single run (for summary)
struct RunResult {
    std::string name;           // Folder name or video_XXXX
    std::string video_path;     // Path to video file
    bool success = false;
    std::optional<int> boom_frame;
    double duration_seconds = 0.0;
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

    // Grid mode: pre-computed combinations
    std::vector<std::map<std::string, std::string>> grid_combinations_;

    // Setup batch directory
    void setupBatchDirectory();

    // Generate a single video (random mode)
    bool generateOne(int index);

    // Generate a single video (grid mode)
    bool generateOneGrid(int index, std::map<std::string, std::string> const& params);

    // Generate randomized config
    Config generateRandomConfig();

    // Generate config from grid params
    Config generateGridConfig(std::map<std::string, std::string> const& params);

    // Generate folder name from grid params
    std::string generateGridFolderName(std::map<std::string, std::string> const& params) const;

    // Pick a random music track
    std::optional<MusicTrack> pickMusicTrack();

    // Save progress after each video
    void saveProgress();

    // Load existing progress for resume
    bool loadProgress();

    // Create symlink to video in batch root folder
    void createVideoSymlink(std::string const& video_path, std::string const& link_name);

    // Print batch completion summary
    void printSummary() const;
};
