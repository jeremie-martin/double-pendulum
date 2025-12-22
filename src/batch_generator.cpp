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
            if (auto node = ranges->get("initial_velocity1")) {
                if (auto arr = node->as_array(); arr && arr->size() >= 2) {
                    config.velocity1_range.min = arr->at(0).value<double>().value_or(0.0);
                    config.velocity1_range.max = arr->at(1).value<double>().value_or(0.0);
                }
            }
            if (auto node = ranges->get("initial_velocity2")) {
                if (auto arr = node->as_array(); arr && arr->size() >= 2) {
                    config.velocity2_range.min = arr->at(0).value<double>().value_or(0.0);
                    config.velocity2_range.max = arr->at(1).value<double>().value_or(0.0);
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

        // Probe settings
        if (auto probe = tbl["probe"].as_table()) {
            config.probe_enabled = true; // Enable probing if section exists
            if (auto pc = probe->get("pendulum_count")) {
                config.probe_pendulum_count = pc->value<int>().value_or(1000);
            }
            if (auto tf = probe->get("total_frames")) {
                config.probe_total_frames = tf->value<int>().value_or(0);
            }
            if (auto md = probe->get("max_dt")) {
                config.probe_max_dt = md->value<double>().value_or(0.0);
            }
            if (auto mr = probe->get("max_retries")) {
                config.max_probe_retries = mr->value<int>().value_or(10);
            }
            if (auto enabled = probe->get("enabled")) {
                config.probe_enabled = enabled->value<bool>().value_or(true);
            }
        }

        // Filter criteria
        if (auto filter = tbl["filter"].as_table()) {
            if (auto min_boom = filter->get("min_boom_seconds")) {
                config.filter.min_boom_seconds = min_boom->value<double>().value_or(0.0);
            }
            if (auto max_boom = filter->get("max_boom_seconds")) {
                config.filter.max_boom_seconds = max_boom->value<double>().value_or(0.0);
            }
            if (auto min_spread = filter->get("min_spread_ratio")) {
                config.filter.min_spread_ratio = min_spread->value<double>().value_or(0.0);
            }
            if (auto req_boom = filter->get("require_boom")) {
                config.filter.require_boom = req_boom->value<bool>().value_or(true);
            }
            if (auto req_music = filter->get("require_valid_music")) {
                config.filter.require_valid_music = req_music->value<bool>().value_or(true);
            }
        }

        // Color presets
        if (auto presets_arr = tbl["color_presets"].as_array()) {
            for (auto const& item : *presets_arr) {
                if (auto preset_tbl = item.as_table()) {
                    ColorParams preset;

                    // Parse scheme
                    if (auto scheme_node = preset_tbl->get("scheme")) {
                        std::string scheme_str =
                            scheme_node->value<std::string>().value_or("spectrum");
                        if (scheme_str == "rainbow") {
                            preset.scheme = ColorScheme::Rainbow;
                        } else if (scheme_str == "heat") {
                            preset.scheme = ColorScheme::Heat;
                        } else if (scheme_str == "cool") {
                            preset.scheme = ColorScheme::Cool;
                        } else if (scheme_str == "monochrome") {
                            preset.scheme = ColorScheme::Monochrome;
                        } else {
                            preset.scheme = ColorScheme::Spectrum;
                        }
                    }

                    // Parse start/end
                    if (auto start_node = preset_tbl->get("start")) {
                        preset.start = start_node->value<double>().value_or(0.0);
                    }
                    if (auto end_node = preset_tbl->get("end")) {
                        preset.end = end_node->value<double>().value_or(1.0);
                    }

                    config.color_presets.push_back(preset);
                }
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
    : config_(config), rng_(std::random_device{}()), filter_(config.filter) {}

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
    std::cout << "Total videos to generate: " << config_.count << "\n\n";

    progress_.total = config_.count;
    progress_.completed = 0;
    progress_.failed = 0;

    for (int i = 0; i < config_.count; ++i) {
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
        Config config;
        int probe_retries = 0;
        ProbeResults probe_result;

        // Phase 1: Probe validation (if enabled)
        if (config_.probe_enabled) {
            bool found_valid = false;

            for (int retry = 0; retry <= config_.max_probe_retries; ++retry) {
                // Generate random config
                config = generateRandomConfig();

                std::cout << "Probe " << (retry + 1) << "/" << (config_.max_probe_retries + 1)
                          << ": angles=" << std::fixed << std::setprecision(1)
                          << rad2deg(config.physics.initial_angle1) << ", "
                          << rad2deg(config.physics.initial_angle2) << " deg, vels=" << std::fixed
                          << std::setprecision(2) << rad2deg(config.physics.initial_velocity1)
                          << ", " << rad2deg(config.physics.initial_velocity2) << " deg/s... ";
                std::cout.flush();

                // Run probe (physics only)
                auto [passes, result] = runProbe(config);
                probe_result = result;

                if (passes) {
                    std::cout << "OK (boom=" << std::setprecision(2) << result.boom_seconds
                              << "s, spread=" << std::setprecision(2) << result.final_spread_ratio
                              << ")\n";
                    probe_retries = retry;
                    found_valid = true;
                    break;
                } else {
                    std::cout << "REJECT: " << filter_.rejectReason(result) << "\n";
                }
            }

            if (!found_valid) {
                std::cerr << "Max probe retries exceeded for video " << index << "\n";

                auto end_time = std::chrono::steady_clock::now();
                double duration = std::chrono::duration<double>(end_time - start_time).count();

                RunResult result{video_name, "",       false, std::nullopt,
                                 0.0,        duration, 0.0,   config_.max_probe_retries + 1,
                                 1.0};
                progress_.results.push_back(result);
                progress_.failed_ids.push_back(video_name);
                return false;
            }
        } else {
            // No probing - just generate random config
            config = generateRandomConfig();
        }

        // Phase 2: Full render
        // Set output directory for this video (direct mode, no timestamp subdirectory)
        config.output.directory = batch_dir_.string() + "/" + video_name;
        config.output.format = OutputFormat::Video;
        config.output.mode = OutputMode::Direct;

        // Create output directory and save resolved config
        std::filesystem::create_directories(config.output.directory);
        config.save(config.output.directory + "/config.toml");

        std::cout << "Rendering: angles=" << std::fixed << std::setprecision(1)
                  << rad2deg(config.physics.initial_angle1) << ", "
                  << rad2deg(config.physics.initial_angle2) << " deg\n";

        // Run simulation
        Simulation sim(config);
        auto results = sim.run([](int current, int total) {
            std::cout << "\rFrame " << current << "/" << total << std::flush;
        });

        // Calculate boom_seconds from boom_frame (needed for music selection)
        double boom_seconds = 0.0;
        if (results.boom_frame) {
            double frame_duration =
                config.simulation.duration_seconds / config.simulation.total_frames;
            boom_seconds = *results.boom_frame * frame_duration;
        }

        // Mux with music if we have tracks and a boom frame
        std::string final_video_path = results.video_path;
        if (results.boom_frame && music_.trackCount() > 0) {
            auto track = pickMusicTrackForBoom(boom_seconds);
            if (track) {
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
            } else if (config_.filter.require_valid_music) {
                // No valid music track found (drop not after boom) - fail and retry
                std::cout << "\nNo music track with drop > " << std::fixed << std::setprecision(1)
                          << boom_seconds << "s - failing video for retry\n";
                // Clean up the rendered video
                std::filesystem::remove(results.video_path);
                return false;
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end_time - start_time).count();

        // Calculate simulation speed (physics time / video time)
        double video_duration =
            static_cast<double>(config.simulation.total_frames) / config.output.video_fps;
        double simulation_speed = config.simulation.duration_seconds / video_duration;

        // Track result and create symlink
        // Use actual simulation spread (more accurate than probe spread)
        RunResult result{
            video_name, final_video_path,           true,          results.boom_frame, boom_seconds,
            duration,   results.final_spread_ratio, probe_retries, simulation_speed};
        progress_.results.push_back(result);
        progress_.completed_ids.push_back(video_name);

        // Create symlink to video in batch root
        createVideoSymlink(final_video_path, video_name + ".mp4");

        return true;

    } catch (std::exception const& e) {
        std::cerr << "Error generating video " << index << ": " << e.what() << "\n";

        auto end_time = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end_time - start_time).count();

        RunResult result{video_name, "", false, std::nullopt, 0.0, duration, 0.0, 0, 1.0};
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

    // Randomize velocities if range is specified (min != max or either non-zero)
    if (config_.velocity1_range.min != config_.velocity1_range.max ||
        config_.velocity1_range.min != 0.0) {
        std::uniform_real_distribution<double> vel1_dist(config_.velocity1_range.min,
                                                         config_.velocity1_range.max);
        config.physics.initial_velocity1 = vel1_dist(rng_);
    }
    if (config_.velocity2_range.min != config_.velocity2_range.max ||
        config_.velocity2_range.min != 0.0) {
        std::uniform_real_distribution<double> vel2_dist(config_.velocity2_range.min,
                                                         config_.velocity2_range.max);
        config.physics.initial_velocity2 = vel2_dist(rng_);
    }

    // Select random color preset if available
    if (!config_.color_presets.empty()) {
        std::uniform_int_distribution<size_t> preset_dist(0, config_.color_presets.size() - 1);
        size_t preset_idx = preset_dist(rng_);
        config.color = config_.color_presets[preset_idx];
    }

    return config;
}

std::pair<bool, ProbeResults> BatchGenerator::runProbe(Config const& config) {
    // Create a modified config for probing (fewer pendulums, faster settings)
    Config probe_config = config;
    probe_config.simulation.pendulum_count = config_.probe_pendulum_count;

    // Apply probe-specific frame count if specified
    if (config_.probe_total_frames > 0) {
        probe_config.simulation.total_frames = config_.probe_total_frames;
    }

    // Apply probe-specific max_dt if specified (for faster probing)
    if (config_.probe_max_dt > 0.0) {
        probe_config.simulation.max_dt = config_.probe_max_dt;
        probe_config.simulation.physics_quality = PhysicsQuality::Custom;
    }

    // Run probe simulation (physics only, no rendering)
    Simulation sim(probe_config);
    ProbeResults results = sim.runProbe();

    // Check if results pass filter criteria
    bool passes = filter_.passes(results);

    return {passes, results};
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

std::optional<MusicTrack> BatchGenerator::pickMusicTrackForBoom(double boom_seconds) {
    if (music_.tracks().empty()) {
        return std::nullopt;
    }

    // Filter: only tracks where drop happens AFTER boom
    std::vector<MusicTrack const*> valid_tracks;
    for (auto const& track : music_.tracks()) {
        if (track.dropTimeSeconds() > boom_seconds) {
            valid_tracks.push_back(&track);
        }
    }

    if (valid_tracks.empty()) {
        return std::nullopt; // No valid tracks - will trigger retry
    }

    // Random selection from valid tracks
    if (config_.random_music) {
        std::uniform_int_distribution<size_t> dist(0, valid_tracks.size() - 1);
        return *valid_tracks[dist(rng_)];
    } else if (!config_.fixed_track_id.empty()) {
        // Check if fixed track is valid for this boom time
        for (auto const* track : valid_tracks) {
            if (track->id == config_.fixed_track_id) {
                return *track;
            }
        }
        return std::nullopt; // Fixed track not valid for this boom time
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

void BatchGenerator::createVideoSymlink(std::string const& video_path,
                                        std::string const& link_name) {
    if (video_path.empty())
        return;

    std::filesystem::path target(video_path);
    if (!std::filesystem::exists(target))
        return;

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
    std::cout << "╔════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                              BATCH COMPLETE                                ║\n";
    std::cout << "╠════════════════════════════════════════════════════════════════════════════╣\n";

    // Calculate totals
    double total_time = 0.0;
    for (auto const& r : progress_.results) {
        total_time += r.duration_seconds;
    }

    std::cout << "║  Total: " << std::setw(4) << progress_.total
              << "    Completed: " << std::setw(4) << progress_.completed
              << "    Failed: " << std::setw(4) << progress_.failed << std::string(25, ' ')
              << "║\n";

    std::cout << "║  Total time: " << std::fixed << std::setprecision(1) << total_time << "s";
    if (progress_.completed > 0) {
        std::cout << "    Avg: " << std::setprecision(1) << (total_time / progress_.completed)
                  << "s/run";
    }
    std::cout << std::string(76 - 14 - std::to_string(static_cast<int>(total_time)).length() - 20,
                             ' ')
              << "║\n";

    std::cout << "╠════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Name                              Status  Boom(s)  Spread  Retries  Time  ║\n";
    std::cout << "╠════════════════════════════════════════════════════════════════════════════╣\n";

    for (auto const& r : progress_.results) {
        std::cout << "║  ";

        // Name (truncate if too long)
        std::string name = r.name;
        if (name.length() > 32) {
            name = name.substr(0, 29) + "...";
        }
        std::cout << std::left << std::setw(32) << name << "  ";

        // Status
        if (r.success) {
            std::cout << "\033[32m" << std::setw(6) << "OK" << "\033[0m" << "  ";
        } else {
            std::cout << "\033[31m" << std::setw(6) << "FAIL" << "\033[0m" << "  ";
        }

        // Boom time in seconds
        if (r.boom_frame) {
            std::cout << std::right << std::fixed << std::setprecision(1) << std::setw(6)
                      << r.boom_seconds << "  ";
        } else {
            std::cout << std::setw(6) << "-" << "  ";
        }

        // Spread ratio
        if (r.success && r.final_spread_ratio > 0) {
            std::cout << std::right << std::fixed << std::setprecision(2) << std::setw(6)
                      << r.final_spread_ratio << "  ";
        } else {
            std::cout << std::setw(6) << "-" << "  ";
        }

        // Probe retries
        std::cout << std::right << std::setw(5) << r.probe_retries << "   ";

        // Duration
        std::cout << std::fixed << std::setprecision(1) << std::setw(5) << r.duration_seconds
                  << "s";
        std::cout << " ║\n";
    }

    std::cout << "╠════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Output: " << std::left << std::setw(66) << batch_dir_.string().substr(0, 66)
              << "║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝\n";
}
