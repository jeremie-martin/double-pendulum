#include "batch_generator.h"

#include "simulation.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <json.hpp>
#include <sstream>
#include <toml.hpp>

using json = nlohmann::json;

BatchConfig BatchConfig::load(std::string const& path) {
    BatchConfig config;

    try {
        auto tbl = toml::parse_file(path);

        // Batch settings
        if (auto batch = tbl["batch"].as_table()) {
            if (auto dir = batch->get("output_directory")) {
                config.output_directory = dir->value<std::string>().value_or("batch_output");
            }
            if (auto cnt = batch->get("count")) {
                config.count = cnt->value<int>().value_or(10);
            }
        }

        // Physics ranges
        if (auto ranges = tbl["physics_ranges"].as_table()) {
            if (auto arr = ranges->get("initial_angle1_deg")->as_array()) {
                if (arr->size() >= 2) {
                    config.angle1_range.min = arr->at(0).value<double>().value_or(-180.0);
                    config.angle1_range.max = arr->at(1).value<double>().value_or(180.0);
                }
            }
            if (auto arr = ranges->get("initial_angle2_deg")->as_array()) {
                if (arr->size() >= 2) {
                    config.angle2_range.min = arr->at(0).value<double>().value_or(-180.0);
                    config.angle2_range.max = arr->at(1).value<double>().value_or(180.0);
                }
            }
            if (auto arr = ranges->get("angle_variation_deg")) {
                if (auto a = arr->as_array(); a && a->size() >= 2) {
                    config.variation_range.min = a->at(0).value<double>().value_or(0.05);
                    config.variation_range.max = a->at(1).value<double>().value_or(0.2);
                }
            }
        }

        // Base config path
        std::string base_config_path = "config/default.toml";
        if (auto base = tbl["batch"].as_table()) {
            if (auto p = base->get("base_config")) {
                base_config_path = p->value<std::string>().value_or(base_config_path);
            }
        }
        config.base_config = Config::load(base_config_path);

        // Music settings
        if (auto music = tbl["music"].as_table()) {
            if (auto db = music->get("database")) {
                config.music_database = db->value<std::string>().value_or("music");
            }
            if (auto rand = music->get("random_selection")) {
                config.random_music = rand->value<bool>().value_or(true);
            }
            if (auto track = music->get("track_id")) {
                config.fixed_track_id = track->value<std::string>().value_or("");
            }
        }

    } catch (toml::parse_error const& err) {
        std::cerr << "Error parsing batch config: " << err.description() << "\n";
    }

    return config;
}

void BatchProgress::save(std::filesystem::path const& path) const {
    json j;
    j["total"] = total;
    j["completed"] = completed;
    j["failed"] = failed;
    j["completed_ids"] = completed_ids;
    j["failed_ids"] = failed_ids;

    std::ofstream out(path);
    if (out) {
        out << j.dump(2) << "\n";
    }
}

BatchProgress BatchProgress::load(std::filesystem::path const& path) {
    BatchProgress progress;

    std::ifstream in(path);
    if (!in) {
        return progress;
    }

    try {
        json j = json::parse(in);

        if (j.contains("total") && j["total"].is_number()) {
            progress.total = j["total"].get<int>();
        }
        if (j.contains("completed") && j["completed"].is_number()) {
            progress.completed = j["completed"].get<int>();
        }
        if (j.contains("failed") && j["failed"].is_number()) {
            progress.failed = j["failed"].get<int>();
        }
        if (j.contains("completed_ids") && j["completed_ids"].is_array()) {
            progress.completed_ids = j["completed_ids"].get<std::vector<std::string>>();
        }
        if (j.contains("failed_ids") && j["failed_ids"].is_array()) {
            progress.failed_ids = j["failed_ids"].get<std::vector<std::string>>();
        }
    } catch (const json::exception& e) {
        std::cerr << "Error parsing progress file: " << e.what() << "\n";
    }

    return progress;
}

BatchGenerator::BatchGenerator(BatchConfig const& config)
    : config_(config), rng_(std::random_device{}()) {}

void BatchGenerator::setupBatchDirectory() {
    // Create timestamp-based batch directory
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);

    std::ostringstream dir_name;
    dir_name << config_.output_directory << "/batch_" << std::put_time(&tm, "%Y%m%d_%H%M%S");

    batch_dir_ = dir_name.str();
    std::filesystem::create_directories(batch_dir_);

    std::cout << "Batch output directory: " << batch_dir_ << "\n";
}

void BatchGenerator::run() {
    setupBatchDirectory();

    // Load music database
    if (!music_.load(config_.music_database)) {
        std::cerr << "Warning: Could not load music database\n";
    }

    progress_.total = config_.count;
    progress_.completed = 0;
    progress_.failed = 0;

    std::cout << "\n=== Starting Batch Generation ===\n";
    std::cout << "Total videos to generate: " << config_.count << "\n\n";

    for (int i = 0; i < config_.count; ++i) {
        std::cout << "\n--- Video " << (i + 1) << "/" << config_.count << " ---\n";

        if (generateOne(i)) {
            progress_.completed++;
        } else {
            progress_.failed++;
        }

        saveProgress();
    }

    std::cout << "\n=== Batch Complete ===\n";
    std::cout << "Completed: " << progress_.completed << "/" << progress_.total << "\n";
    std::cout << "Failed: " << progress_.failed << "\n";
    std::cout << "Output: " << batch_dir_ << "\n";
}

void BatchGenerator::resume() {
    // Find latest batch directory
    std::filesystem::path latest_batch;
    std::filesystem::file_time_type latest_time{};

    for (auto const& entry : std::filesystem::directory_iterator(config_.output_directory)) {
        if (entry.is_directory() && entry.path().filename().string().starts_with("batch_")) {
            auto progress_file = entry.path() / "progress.json";
            if (std::filesystem::exists(progress_file)) {
                auto time = std::filesystem::last_write_time(progress_file);
                if (time > latest_time) {
                    latest_time = time;
                    latest_batch = entry.path();
                }
            }
        }
    }

    if (latest_batch.empty()) {
        std::cerr << "No resumable batch found in " << config_.output_directory << "\n";
        return;
    }

    batch_dir_ = latest_batch;
    std::cout << "Resuming batch: " << batch_dir_ << "\n";

    if (!loadProgress()) {
        std::cerr << "Failed to load progress file\n";
        return;
    }

    // Load music database
    if (!music_.load(config_.music_database)) {
        std::cerr << "Warning: Could not load music database\n";
    }

    int start_index = progress_.completed + progress_.failed;
    int remaining = config_.count - start_index;

    std::cout << "\n=== Resuming Batch Generation ===\n";
    std::cout << "Already completed: " << progress_.completed << "\n";
    std::cout << "Already failed: " << progress_.failed << "\n";
    std::cout << "Remaining: " << remaining << "\n\n";

    for (int i = start_index; i < config_.count; ++i) {
        std::cout << "\n--- Video " << (i + 1) << "/" << config_.count << " ---\n";

        if (generateOne(i)) {
            progress_.completed++;
        } else {
            progress_.failed++;
        }

        saveProgress();
    }

    std::cout << "\n=== Batch Complete ===\n";
    std::cout << "Completed: " << progress_.completed << "/" << progress_.total << "\n";
    std::cout << "Failed: " << progress_.failed << "\n";
}

bool BatchGenerator::generateOne(int index) {
    try {
        // Generate random config
        Config config = generateRandomConfig();

        // Set output directory for this video
        std::ostringstream video_dir;
        video_dir << batch_dir_.string() << "/video_" << std::setfill('0') << std::setw(4) << index;
        config.output.directory = video_dir.str();
        config.output.format = OutputFormat::Video;

        std::cout << "Initial angles: " << rad2deg(config.physics.initial_angle1) << ", "
                  << rad2deg(config.physics.initial_angle2) << " deg\n";

        // Run simulation
        Simulation sim(config);
        auto results = sim.run([](int current, int total) {
            std::cout << "\rFrame " << current << "/" << total << std::flush;
        });

        // Mux with music if we have tracks and a boom frame
        if (auto track = pickMusicTrack(); track && results.boom_frame) {
            std::filesystem::path video_path = results.video_path;
            std::filesystem::path output_path =
                video_path.parent_path() / (video_path.stem().string() + "_with_music.mp4");

            std::cout << "\nAdding music: " << track->title << "\n";
            if (MusicManager::muxWithAudio(video_path, track->filepath, output_path,
                                           *results.boom_frame, track->drop_time_ms,
                                           config.output.video_fps)) {
                // Replace original with muxed version
                std::filesystem::remove(video_path);
                std::filesystem::rename(output_path, video_path);
            }
        }

        std::string video_id = "video_" + std::to_string(index);
        progress_.completed_ids.push_back(video_id);

        return true;

    } catch (std::exception const& e) {
        std::cerr << "Error generating video " << index << ": " << e.what() << "\n";
        std::string video_id = "video_" + std::to_string(index);
        progress_.failed_ids.push_back(video_id);
        return false;
    }
}

Config BatchGenerator::generateRandomConfig() {
    Config config = config_.base_config;

    // Randomize angles within ranges
    std::uniform_real_distribution<double> angle1_dist(config_.angle1_range.min,
                                                       config_.angle1_range.max);
    std::uniform_real_distribution<double> angle2_dist(config_.angle2_range.min,
                                                       config_.angle2_range.max);
    std::uniform_real_distribution<double> variation_dist(config_.variation_range.min,
                                                          config_.variation_range.max);

    config.physics.initial_angle1 = deg2rad(angle1_dist(rng_));
    config.physics.initial_angle2 = deg2rad(angle2_dist(rng_));
    config.simulation.angle_variation = deg2rad(variation_dist(rng_));

    return config;
}

std::optional<MusicTrack> BatchGenerator::pickMusicTrack() {
    if (music_.tracks().empty()) {
        return std::nullopt;
    }

    if (config_.random_music) {
        return music_.randomTrack();
    } else if (!config_.fixed_track_id.empty()) {
        return music_.getTrack(config_.fixed_track_id);
    }

    return std::nullopt;
}

void BatchGenerator::saveProgress() {
    auto progress_path = batch_dir_ / "progress.json";
    progress_.save(progress_path);
}

bool BatchGenerator::loadProgress() {
    auto progress_path = batch_dir_ / "progress.json";
    if (!std::filesystem::exists(progress_path)) {
        return false;
    }

    progress_ = BatchProgress::load(progress_path);
    return true;
}
