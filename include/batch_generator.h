#pragma once

#include "config.h"
#include "music_manager.h"

#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Batch configuration loaded from TOML
struct BatchConfig {
    std::string output_directory = "batch_output";
    int count = 10;

    // Physics parameter ranges
    struct Range {
        double min = 0.0;
        double max = 0.0;
    };

    Range angle1_range{-180.0, 180.0};
    Range angle2_range{-180.0, 180.0};
    Range variation_range{0.05, 0.2};

    // Base config (for parameters not being randomized)
    Config base_config;

    // Music settings
    std::string music_database = "music";
    bool random_music = true;
    std::string fixed_track_id; // If random_music is false

    static BatchConfig load(std::string const& path);
};

// Progress tracking for batch operations
struct BatchProgress {
    int total = 0;
    int completed = 0;
    int failed = 0;
    std::vector<std::string> completed_ids;
    std::vector<std::string> failed_ids;

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

    // Setup batch directory
    void setupBatchDirectory();

    // Generate a single video
    bool generateOne(int index);

    // Generate randomized config
    Config generateRandomConfig();

    // Pick a random music track
    std::optional<MusicTrack> pickMusicTrack();

    // Save progress after each video
    void saveProgress();

    // Load existing progress for resume
    bool loadProgress();
};
