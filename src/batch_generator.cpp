#include "batch_generator.h"

#include "simulation.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <json.hpp>
#include <regex>
#include <sstream>
#include <toml.hpp>

using json = nlohmann::json;

// Parameter name abbreviations for folder naming
static std::map<std::string, std::string> const PARAM_ABBREVIATIONS = {
    {"simulation.pendulum_count", "n"},
    {"simulation.angle_variation_deg", "var"},
    {"simulation.duration_seconds", "dur"},
    {"simulation.total_frames", "frames"},
    {"simulation.physics_quality", "qual"},
    {"physics.initial_angle1_deg", "a1"},
    {"physics.initial_angle2_deg", "a2"},
    {"physics.gravity", "g"},
    {"physics.length1", "L1"},
    {"physics.length2", "L2"},
    {"physics.mass1", "m1"},
    {"physics.mass2", "m2"},
    {"render.width", "w"},
    {"render.height", "h"},
    {"post_process.exposure", "exp"},
    {"post_process.gamma", "gam"},
    {"post_process.contrast", "con"},
    {"post_process.tone_map", "tm"},
    {"post_process.normalization", "norm"},
    {"color.scheme", "col"},
    {"color.start", "cstart"},
    {"color.end", "cend"},
    {"detection.boom_threshold", "boom"},
    {"detection.white_threshold", "white"},
    {"output.format", "fmt"},
    {"output.video_fps", "fps"},
};

// Sanitize value for use in folder name
static std::string sanitizeValue(std::string const& value) {
    std::string result;
    for (char c : value) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') {
            result += c;
        } else if (c == ' ') {
            result += '_';
        }
    }
    // Trim trailing zeros after decimal point for floats
    if (result.find('.') != std::string::npos) {
        while (result.back() == '0') result.pop_back();
        if (result.back() == '.') result.pop_back();
    }
    return result;
}

// Get abbreviated parameter name
static std::string abbreviateParam(std::string const& param) {
    auto it = PARAM_ABBREVIATIONS.find(param);
    if (it != PARAM_ABBREVIATIONS.end()) {
        return it->second;
    }
    // Fallback: use last component
    auto dot = param.rfind('.');
    return dot != std::string::npos ? param.substr(dot + 1) : param;
}

// GridConfig implementation
std::vector<std::map<std::string, std::string>> GridConfig::expandCombinations() const {
    std::vector<std::map<std::string, std::string>> result;

    if (param_sets.empty()) {
        return result;
    }

    // Start with one empty combination
    result.push_back({});

    // For each parameter, expand all existing combinations
    for (auto const& [param, values] : param_sets) {
        std::vector<std::map<std::string, std::string>> new_result;

        for (auto const& combo : result) {
            for (auto const& value : values) {
                auto new_combo = combo;
                new_combo[param] = value;
                new_result.push_back(new_combo);
            }
        }

        result = std::move(new_result);
    }

    return result;
}

size_t GridConfig::totalCombinations() const {
    if (param_sets.empty()) return 0;

    size_t total = 1;
    for (auto const& [param, values] : param_sets) {
        total *= values.size();
    }
    return total;
}

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
            // Parse mode
            if (auto mode_str = batch->get("mode")) {
                std::string mode = mode_str->value<std::string>().value_or("random");
                if (mode == "grid") {
                    config.mode = BatchMode::Grid;
                } else {
                    config.mode = BatchMode::Random;
                }
            }
        }

        // Grid parameters (for Grid mode)
        if (auto grid_tbl = tbl["grid"].as_table()) {
            for (auto const& [key, node] : *grid_tbl) {
                if (auto arr = node.as_array()) {
                    std::vector<std::string> values;
                    for (auto const& elem : *arr) {
                        if (auto s = elem.value<std::string>()) {
                            values.push_back(*s);
                        } else if (auto i = elem.value<int64_t>()) {
                            values.push_back(std::to_string(*i));
                        } else if (auto d = elem.value<double>()) {
                            std::ostringstream oss;
                            oss << std::setprecision(6) << *d;
                            values.push_back(oss.str());
                        } else if (auto b = elem.value<bool>()) {
                            values.push_back(*b ? "true" : "false");
                        }
                    }
                    if (!values.empty()) {
                        config.grid.param_sets[std::string(key)] = values;
                    }
                }
            }
        }

        // Physics ranges (for Random mode)
        if (auto ranges = tbl["physics_ranges"].as_table()) {
            if (auto node = ranges->get("initial_angle1_deg")) {
                if (auto arr = node->as_array(); arr && arr->size() >= 2) {
                    config.angle1_range.min = arr->at(0).value<double>().value_or(-180.0);
                    config.angle1_range.max = arr->at(1).value<double>().value_or(180.0);
                }
            }
            if (auto node = ranges->get("initial_angle2_deg")) {
                if (auto arr = node->as_array(); arr && arr->size() >= 2) {
                    config.angle2_range.min = arr->at(0).value<double>().value_or(-180.0);
                    config.angle2_range.max = arr->at(1).value<double>().value_or(180.0);
                }
            }
            if (auto node = ranges->get("angle_variation_deg")) {
                if (auto arr = node->as_array(); arr && arr->size() >= 2) {
                    config.variation_range.min = arr->at(0).value<double>().value_or(0.05);
                    config.variation_range.max = arr->at(1).value<double>().value_or(0.2);
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

    std::cout << "\n=== Starting Batch Generation ===\n";

    if (config_.mode == BatchMode::Grid) {
        // Grid mode: generate all combinations
        grid_combinations_ = config_.grid.expandCombinations();
        progress_.total = static_cast<int>(grid_combinations_.size());

        if (grid_combinations_.empty()) {
            std::cerr << "Error: No grid parameters specified\n";
            return;
        }

        std::cout << "Mode: Grid (parameter sweep)\n";
        std::cout << "Grid parameters:\n";
        for (auto const& [param, values] : config_.grid.param_sets) {
            std::cout << "  " << param << ": [";
            for (size_t i = 0; i < values.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << values[i];
            }
            std::cout << "]\n";
        }
        std::cout << "Total combinations: " << progress_.total << "\n\n";

        progress_.completed = 0;
        progress_.failed = 0;

        for (size_t i = 0; i < grid_combinations_.size(); ++i) {
            std::cout << "\n--- Combination " << (i + 1) << "/" << grid_combinations_.size() << " ---\n";

            if (generateOneGrid(static_cast<int>(i), grid_combinations_[i])) {
                progress_.completed++;
            } else {
                progress_.failed++;
            }

            saveProgress();
        }
    } else {
        // Random mode: original behavior
        progress_.total = config_.count;
        progress_.completed = 0;
        progress_.failed = 0;

        std::cout << "Mode: Random (sampling from ranges)\n";
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
    }

    printSummary();
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

    printSummary();
}

bool BatchGenerator::generateOne(int index) {
    auto start_time = std::chrono::steady_clock::now();
    std::ostringstream name_stream;
    name_stream << "video_" << std::setfill('0') << std::setw(4) << index;
    std::string video_name = name_stream.str();

    try {
        // Generate random config
        Config config = generateRandomConfig();

        // Set output directory for this video (skip run_ subdirectory)
        config.output.directory = batch_dir_.string() + "/" + video_name;
        config.output.format = OutputFormat::Video;
        config.output.skip_run_subdirectory = true;

        std::cout << "Initial angles: " << rad2deg(config.physics.initial_angle1) << ", "
                  << rad2deg(config.physics.initial_angle2) << " deg\n";

        // Run simulation
        Simulation sim(config);
        auto results = sim.run([](int current, int total) {
            std::cout << "\rFrame " << current << "/" << total << std::flush;
        });

        // Mux with music if we have tracks and a boom frame
        std::string final_video_path = results.video_path;
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

        auto end_time = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end_time - start_time).count();

        // Track result and create symlink
        RunResult result{video_name, final_video_path, true, results.boom_frame, duration};
        progress_.results.push_back(result);
        progress_.completed_ids.push_back(video_name);

        // Create symlink to video in batch root
        createVideoSymlink(final_video_path, video_name + ".mp4");

        return true;

    } catch (std::exception const& e) {
        std::cerr << "Error generating video " << index << ": " << e.what() << "\n";

        auto end_time = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end_time - start_time).count();

        RunResult result{video_name, "", false, std::nullopt, duration};
        progress_.results.push_back(result);
        progress_.failed_ids.push_back(video_name);
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

Config BatchGenerator::generateGridConfig(std::map<std::string, std::string> const& params) {
    Config config = config_.base_config;

    // Apply each parameter override
    for (auto const& [key, value] : params) {
        if (!config.applyOverride(key, value)) {
            std::cerr << "Warning: Failed to apply grid parameter: " << key << " = " << value << "\n";
        }
    }

    return config;
}

std::string BatchGenerator::generateGridFolderName(std::map<std::string, std::string> const& params) const {
    std::ostringstream name;
    bool first = true;

    for (auto const& [param, value] : params) {
        if (!first) name << "_";
        first = false;

        name << abbreviateParam(param) << sanitizeValue(value);
    }

    return name.str();
}

bool BatchGenerator::generateOneGrid(int index, std::map<std::string, std::string> const& params) {
    auto start_time = std::chrono::steady_clock::now();
    std::string folder_name = generateGridFolderName(params);

    try {
        // Generate config from grid params
        Config config = generateGridConfig(params);

        // Set output directory (skip run_ subdirectory)
        config.output.directory = batch_dir_.string() + "/" + folder_name;
        config.output.format = OutputFormat::Video;
        config.output.skip_run_subdirectory = true;

        std::cout << "Parameters:";
        for (auto const& [key, value] : params) {
            std::cout << " " << abbreviateParam(key) << "=" << value;
        }
        std::cout << "\n";
        std::cout << "Output: " << folder_name << "/\n";

        // Run simulation
        Simulation sim(config);
        auto results = sim.run([](int current, int total) {
            std::cout << "\rFrame " << current << "/" << total << std::flush;
        });

        // Mux with music if we have tracks and a boom frame
        std::string final_video_path = results.video_path;
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

        auto end_time = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end_time - start_time).count();

        // Track result and create symlink
        RunResult result{folder_name, final_video_path, true, results.boom_frame, duration};
        progress_.results.push_back(result);
        progress_.completed_ids.push_back(folder_name);

        // Create symlink to video in batch root
        createVideoSymlink(final_video_path, folder_name + ".mp4");

        return true;

    } catch (std::exception const& e) {
        std::cerr << "Error generating combination " << index << ": " << e.what() << "\n";

        auto end_time = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end_time - start_time).count();

        RunResult result{folder_name, "", false, std::nullopt, duration};
        progress_.results.push_back(result);
        progress_.failed_ids.push_back(folder_name);
        return false;
    }
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

void BatchGenerator::createVideoSymlink(std::string const& video_path, std::string const& link_name) {
    if (video_path.empty()) return;

    std::filesystem::path target(video_path);
    if (!std::filesystem::exists(target)) return;

    std::filesystem::path link_path = batch_dir_ / link_name;

    // Remove existing symlink if present
    if (std::filesystem::exists(link_path) || std::filesystem::is_symlink(link_path)) {
        std::filesystem::remove(link_path);
    }

    // Create relative symlink
    std::filesystem::path relative_target = std::filesystem::relative(target, batch_dir_);
    std::error_code ec;
    std::filesystem::create_symlink(relative_target, link_path, ec);
    if (ec) {
        std::cerr << "Warning: Could not create symlink: " << ec.message() << "\n";
    }
}

void BatchGenerator::printSummary() const {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                          BATCH COMPLETE                              ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";

    // Calculate totals
    double total_time = 0.0;
    for (auto const& r : progress_.results) {
        total_time += r.duration_seconds;
    }

    std::cout << "║  Total: " << std::setw(4) << progress_.total
              << "    Completed: " << std::setw(4) << progress_.completed
              << "    Failed: " << std::setw(4) << progress_.failed;
    int padding = 17;
    for (int i = 0; i < padding; ++i) std::cout << " ";
    std::cout << "║\n";

    std::cout << "║  Total time: " << std::fixed << std::setprecision(1) << total_time << "s";
    if (progress_.completed > 0) {
        std::cout << "    Avg: " << std::setprecision(1) << (total_time / progress_.completed) << "s/run";
    }
    std::cout << std::string(70 - 14 - std::to_string(static_cast<int>(total_time)).length() - 20, ' ') << "║\n";

    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Name                                      Status   Boom    Time    ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";

    for (auto const& r : progress_.results) {
        std::cout << "║  ";

        // Name (truncate if too long)
        std::string name = r.name;
        if (name.length() > 40) {
            name = name.substr(0, 37) + "...";
        }
        std::cout << std::left << std::setw(40) << name << "  ";

        // Status
        if (r.success) {
            std::cout << "\033[32m" << std::setw(6) << "OK" << "\033[0m" << "   ";
        } else {
            std::cout << "\033[31m" << std::setw(6) << "FAIL" << "\033[0m" << "   ";
        }

        // Boom frame
        if (r.boom_frame) {
            std::cout << std::right << std::setw(5) << *r.boom_frame;
        } else {
            std::cout << std::setw(5) << "-";
        }
        std::cout << "   ";

        // Duration
        std::cout << std::fixed << std::setprecision(1) << std::setw(5) << r.duration_seconds << "s";
        std::cout << "  ║\n";
    }

    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Output: " << std::left << std::setw(60) << batch_dir_.string().substr(0, 60) << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";
}
