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
        std::string base_path = std::filesystem::path(path).parent_path().string();
        if (base_path.empty())
            base_path = ".";

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

        // Process includes from batch config and merge into base_config
        // This allows batch.toml to include best_params.toml directly
        if (auto includes = tbl["include"].as_array()) {
            for (auto const& inc : *includes) {
                if (auto inc_path = inc.value<std::string>()) {
                    std::filesystem::path full_path;
                    if (std::filesystem::path(*inc_path).is_absolute()) {
                        full_path = *inc_path;
                    } else {
                        full_path = std::filesystem::path(base_path) / *inc_path;
                    }
                    if (std::filesystem::exists(full_path)) {
                        // Load included config and merge into base_config
                        Config included = Config::load(full_path.string());
                        // Merge metric configs (included values override base)
                        for (auto const& [name, cfg] : included.metric_configs) {
                            config.base_config.metric_configs[name] = cfg;
                        }
                        // Merge targets (included targets override base if same name)
                        for (auto const& target : included.targets) {
                            auto it =
                                std::find_if(config.base_config.targets.begin(),
                                             config.base_config.targets.end(),
                                             [&](auto const& t) { return t.name == target.name; });
                            if (it != config.base_config.targets.end()) {
                                *it = target; // Override existing
                            } else {
                                config.base_config.targets.push_back(target); // Add new
                            }
                        }
                    } else {
                        std::cerr << "Warning: Batch include not found: " << full_path << "\n";
                    }
                }
            }
        }

        // Probe settings (Phase 1)
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

            // Phase 2: ML-based boom detection
            if (auto phase2 = probe->get("phase2")) {
                if (auto phase2_tbl = phase2->as_table()) {
                    if (auto enabled = phase2_tbl->get("enabled")) {
                        config.phase2_enabled = enabled->value<bool>().value_or(false);
                    }
                    if (auto pc = phase2_tbl->get("pendulum_count")) {
                        config.phase2_pendulum_count = pc->value<int>().value_or(2000);
                    }
                    if (auto socket = phase2_tbl->get("socket")) {
                        config.boom_socket_path = socket->value<std::string>().value_or("");
                    }
                }
            }
        }

        // Filter criteria (new target-based system)
        if (auto filter = tbl["filter"].as_table()) {
            // General constraints (non-target)
            if (auto min_unif = filter->get("min_uniformity")) {
                config.filter.min_uniformity = min_unif->value<double>().value_or(0.0);
            }

            // Target-based constraints: [filter.targets.X]
            if (auto targets = filter->get("targets")) {
                if (auto targets_tbl = targets->as_table()) {
                    for (auto const& [name, constraint_node] : *targets_tbl) {
                        if (auto constraint = constraint_node.as_table()) {
                            TargetConstraint tc;
                            tc.target_name = name;

                            if (auto req = constraint->get("required")) {
                                tc.required = req->value<bool>().value_or(false);
                            }

                            // Frame target constraints
                            if (auto min_s = constraint->get("min_seconds")) {
                                tc.min_seconds = min_s->value<double>();
                            }
                            if (auto max_s = constraint->get("max_seconds")) {
                                tc.max_seconds = max_s->value<double>();
                            }

                            // Score target constraints
                            if (auto min_sc = constraint->get("min_score")) {
                                tc.min_score = min_sc->value<double>();
                            }
                            if (auto max_sc = constraint->get("max_score")) {
                                tc.max_score = max_sc->value<double>();
                            }

                            config.filter.target_constraints.push_back(tc);
                        }
                    }
                }
            }
        }

        // Load preset library if specified
        if (auto batch = tbl["batch"].as_table()) {
            if (auto presets_path = batch->get("presets")) {
                std::string path = presets_path->value<std::string>().value_or("");
                if (!path.empty()) {
                    config.presets = PresetLibrary::load(path);
                }
            }
        }

        // Randomization settings (which presets to randomly select from)
        if (auto randomize = tbl["randomize"].as_table()) {
            // Color preset names
            if (auto color_arr = randomize->get("color_presets")) {
                if (auto arr = color_arr->as_array()) {
                    for (auto const& item : *arr) {
                        if (auto name = item.value<std::string>()) {
                            config.color_preset_names.push_back(*name);
                        }
                    }
                }
            }
            // Post-process preset names
            if (auto pp_arr = randomize->get("post_process_presets")) {
                if (auto arr = pp_arr->as_array()) {
                    for (auto const& item : *arr) {
                        if (auto name = item.value<std::string>()) {
                            config.post_process_preset_names.push_back(*name);
                        }
                    }
                }
            }
        }

        // Remote transfer settings
        if (auto transfer = tbl["transfer"].as_table()) {
            if (auto enabled = transfer->get("enabled")) {
                config.transfer.enabled = enabled->value<bool>().value_or(false);
            }
            if (auto host = transfer->get("host")) {
                config.transfer.host = host->value<std::string>().value_or("");
            }
            if (auto path = transfer->get("remote_path")) {
                config.transfer.remote_path = path->value<std::string>().value_or("");
            }
            if (auto key = transfer->get("identity_file")) {
                config.transfer.identity_file = key->value<std::string>().value_or("");
            }
            if (auto del = transfer->get("delete_after_transfer")) {
                config.transfer.delete_after_transfer = del->value<bool>().value_or(false);
            }
            if (auto timeout = transfer->get("timeout_seconds")) {
                config.transfer.timeout_seconds = timeout->value<int>().value_or(300);
            }
        }

    } catch (toml::parse_error const& err) {
        std::cerr << "Error parsing batch config: " << err.description() << "\n";
    }

    // Validate that all filter target constraints reference existing targets
    for (auto const& constraint : config.filter.target_constraints) {
        bool found = false;
        for (auto const& target : config.base_config.targets) {
            if (target.name == constraint.target_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Warning: Filter references non-existent target '"
                      << constraint.target_name << "'\n";
            std::cerr << "  Add [targets." << constraint.target_name
                      << "] to your base config or an included file.\n";
            std::cerr << "  Available targets: ";
            if (config.base_config.targets.empty()) {
                std::cerr << "(none defined)";
            } else {
                for (size_t i = 0; i < config.base_config.targets.size(); ++i) {
                    if (i > 0)
                        std::cerr << ", ";
                    std::cerr << config.base_config.targets[i].name;
                }
            }
            std::cerr << "\n";
        }
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
    : config_(config), rng_(std::random_device{}()), filter_(config.filter.toProbeFilter()) {}

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

    // Generate unique video name with timestamp to avoid conflicts when
    // transferring to a shared watch directory from multiple batch runs
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now = *std::localtime(&time_t_now);

    std::ostringstream name_stream;
    name_stream << "video_" << std::setfill('0') << std::setw(4) << index
                << "_" << std::put_time(&tm_now, "%Y%m%d_%H%M%S");
    std::string video_name = name_stream.str();

    try {
        Config config;
        int probe_retries = 0;
        metrics::ProbePhaseResults probe_result;
        std::optional<int> ml_boom_frame;  // Boom frame from ML phase 2 (if enabled)

        // Phase 1 + Phase 2: Probe validation (if enabled)
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

                // Run phase 1 probe (physics only, fast)
                auto [passes, result] = runProbe(config);
                probe_result = result;

                if (!passes) {
                    std::cout << "REJECT: " << result.rejection_reason << "\n";
                    continue;  // Try next random config
                }

                std::cout << "P1 OK";

                // Phase 2: ML-based boom detection (if enabled)
                if (config_.phase2_enabled) {
                    std::cout << " | P2: ";
                    std::cout.flush();

                    auto [accepted, boom_frame] = runPhase2Probe(config);

                    if (!accepted) {
                        std::cout << "REJECT (ML)\n";
                        continue;  // Try next random config
                    }

                    ml_boom_frame = boom_frame;
                    double boom_seconds = boom_frame * config.simulation.frameDuration();
                    std::cout << "OK (boom=" << std::setprecision(2) << boom_seconds << "s)\n";
                } else {
                    // No phase 2 - use phase 1 results
                    std::cout << " (boom=" << std::setprecision(2) << result.boom_seconds
                              << "s, uniformity=" << std::setprecision(2) << result.final_uniformity
                              << ")\n";
                }

                probe_retries = retry;
                found_valid = true;
                break;
            }

            if (!found_valid) {
                std::cerr << "Max probe retries exceeded for video " << index << "\n";

                auto end_time = std::chrono::steady_clock::now();
                double duration = std::chrono::duration<double>(end_time - start_time).count();

                RunResult result{video_name,
                                 "",
                                 false,
                                 std::nullopt,
                                 0.0,
                                 std::nullopt,
                                 0.0,
                                 0.0,
                                 duration,
                                 0.0,
                                 config_.max_probe_retries + 1,
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

        auto end_time = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end_time - start_time).count();

        // Calculate simulation speed (physics time / video time)
        double video_duration =
            static_cast<double>(config.simulation.total_frames) / config.output.video_fps;
        double simulation_speed = config.simulation.duration_seconds / video_duration;

        // Track result and create symlink
        // Use actual simulation uniformity (more accurate than probe uniformity)
        // Extract chaos and quality from predictions
        double chaos_seconds = 0.0;
        std::optional<int> chaos_frame = results.getChaosFrame();
        if (chaos_frame) {
            chaos_seconds = *chaos_frame * config.simulation.frameDuration();
        }
        double boom_quality = results.getBoomQuality().value_or(0.0);

        // Use ML-detected boom frame from phase 2 if available, otherwise use simulation's detection
        std::optional<int> final_boom_frame = ml_boom_frame.has_value() ? ml_boom_frame : results.boom_frame;
        double boom_sim_seconds =
            final_boom_frame ? *final_boom_frame * config.simulation.frameDuration() : 0.0;

        // If phase 2 provided boom frame, update the metadata.json file
        if (ml_boom_frame.has_value()) {
            std::string metadata_path = config.output.directory + "/metadata.json";
            std::ifstream in(metadata_path);
            if (in) {
                json metadata = json::parse(in);
                in.close();

                // Update boom_frame and boom_seconds in metadata
                metadata["results"]["boom_frame"] = *ml_boom_frame;
                metadata["results"]["boom_seconds"] = boom_sim_seconds;

                std::ofstream out(metadata_path);
                if (out) {
                    out << metadata.dump(2) << "\n";
                }
            }
        }

        RunResult result{video_name,
                         results.video_path,
                         true,
                         final_boom_frame,
                         boom_sim_seconds,
                         chaos_frame,
                         chaos_seconds,
                         boom_quality,
                         duration,
                         results.final_uniformity,
                         probe_retries,
                         simulation_speed};
        progress_.results.push_back(result);
        progress_.completed_ids.push_back(video_name);

        // Create symlink to video in batch root
        createVideoSymlink(results.video_path, video_name + ".mp4");

        // Transfer files to remote host (if configured)
        // Note: Failures are logged but do not fail the batch
        transferFiles(config.output.directory, video_name);

        return true;

    } catch (std::exception const& e) {
        std::cerr << "Error generating video " << index << ": " << e.what() << "\n";

        auto end_time = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end_time - start_time).count();

        RunResult result{video_name, "",  false,    std::nullopt, 0.0, std::nullopt,
                         0.0,        0.0, duration, 0.0,          0,   1.0};
        progress_.results.push_back(result);
        progress_.failed_ids.push_back(video_name);
        return false;
    }
}

Config BatchGenerator::generateRandomConfig() {
    Config config = config_.base_config;

    // -------------------------------------------------------------------------
    // ⚠️⚠️⚠️  WARNING: ANGLE RANGE INTERPRETATION HACK  ⚠️⚠️⚠️
    //
    // The batch config specifies:
    //
    //   initial_angle1_deg = [150.0, 180.0]
    //
    // but this is interpreted *symmetrically* around zero in code:
    //
    //   +[150°, 180°]  OR  -[150°, 180°]
    //
    // This allows starting the first pendulum "nearly vertical and above the
    // horizontal" on BOTH sides, while keeping the config human-readable.
    //
    // IMPORTANT:
    // - This is a SPECIAL CASE for angle1 only.
    // - The config itself does NOT express this symmetry.
    // - Anyone reading the TOML alone would assume a single-sided range.
    //
    // 🚨 TODO (BIG): Replace this with a proper angle domain system:
    //   - Support wrapped ranges (e.g. [150°, -150°])
    //   - Or explicit symmetry flags (e.g. symmetric = true)
    //   - Or a dedicated "near_vertical" semantic mode
    //   - Or angle distributions instead of linear ranges
    //
    // Until then, this hack is intentional and relied upon.
    // -------------------------------------------------------------------------

    // --- Angle 1: symmetric sampling around ±180° ----------------------------
    {
        std::uniform_real_distribution<double> base_dist(config_.angle1_range.min,
                                                         config_.angle1_range.max);

        std::bernoulli_distribution side_dist(0.5); // true = +side, false = -side

        double angle_deg = base_dist(rng_);
        if (!side_dist(rng_)) {
            angle_deg = -angle_deg;
        }

        config.physics.initial_angle1 = deg2rad(angle_deg);
    }

    // --- Angle 2: normal linear range (no symmetry hack) ----------------------
    {
        std::uniform_real_distribution<double> angle2_dist(config_.angle2_range.min,
                                                           config_.angle2_range.max);
        config.physics.initial_angle2 = deg2rad(angle2_dist(rng_));
    }

    // --- Angle variation -----------------------------------------------------
    {
        std::uniform_real_distribution<double> variation_dist(config_.variation_range.min,
                                                              config_.variation_range.max);
        config.simulation.angle_variation = deg2rad(variation_dist(rng_));
    }

    // --- Velocities ----------------------------------------------------------
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

    // --- Color preset --------------------------------------------------------
    if (!config_.color_preset_names.empty()) {
        std::uniform_int_distribution<size_t> dist(0, config_.color_preset_names.size() - 1);
        std::string const& name = config_.color_preset_names[dist(rng_)];
        if (auto preset = config_.presets.getColor(name)) {
            config.color = *preset;
            config.selected_color_preset_name = name;
        } else {
            std::cerr << "Warning: Color preset '" << name << "' not found in library\n";
        }
    }

    // --- Post-process preset -------------------------------------------------
    if (!config_.post_process_preset_names.empty()) {
        std::uniform_int_distribution<size_t> dist(0, config_.post_process_preset_names.size() - 1);
        std::string const& name = config_.post_process_preset_names[dist(rng_)];
        if (auto preset = config_.presets.getPostProcess(name)) {
            config.post_process = *preset;
            config.selected_post_process_preset_name = name;
        } else {
            std::cerr << "Warning: Post-process preset '" << name << "' not found in library\n";
        }
    }

    return config;
}

std::pair<bool, metrics::ProbePhaseResults> BatchGenerator::runProbe(Config const& config) {
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
    metrics::ProbePhaseResults results = sim.runProbe();

    // Create a minimal collector for uniformity metric evaluation
    metrics::MetricsCollector collector;
    collector.registerStandardMetrics();

    // Push final uniformity as a single-point metric for filter evaluation
    if (results.final_uniformity > 0.0) {
        collector.beginFrame(0);
        collector.setMetric(metrics::MetricNames::CircularSpread, results.final_uniformity);
        collector.endFrame();
    }

    // Evaluate filter with predictions from the probe
    // Note: Target constraints are evaluated against predictions, not events
    metrics::EventDetector events; // Empty - legacy events not used with target constraints
    auto filter_result = filter_.evaluate(collector, events, results.score, results.predictions);
    results.passed_filter = filter_result.passed;
    results.rejection_reason = filter_result.reason;

    return {filter_result.passed, results};
}

std::pair<bool, int> BatchGenerator::runPhase2Probe(Config const& config) {
    // Phase 2: Run simulation with 2000 pendulums and send to ML boom detection server
    // Returns (accepted, boom_frame) where boom_frame is -1 if rejected

    if (config_.boom_socket_path.empty()) {
        std::cerr << "Error: Phase 2 enabled but boom_socket_path not configured\n";
        return {false, -1};
    }

    // Create config for phase 2 probe (full duration, 2000 pendulums)
    Config phase2_config = config;
    phase2_config.simulation.pendulum_count = config_.phase2_pendulum_count;
    // Use full frame count and duration from base config (no probe shortcuts)

    // Run simulation and collect all states
    Simulation sim(phase2_config);
    auto states = sim.runProbeCollectStates();

    int frames = phase2_config.simulation.total_frames;
    int pendulums = phase2_config.simulation.pendulum_count;

    // Send to boom detection server
    try {
        boom::BoomClient client(config_.boom_socket_path);
        auto result = client.predictBinary(states.data(), frames, pendulums);

        if (!result.ok) {
            std::cerr << "Boom server error: " << result.error_message << "\n";
            return {false, -1};
        }

        if (result.accepted) {
            return {true, result.boom_frame};
        } else {
            return {false, -1};
        }
    } catch (std::exception const& e) {
        std::cerr << "Failed to connect to boom server: " << e.what() << "\n";
        return {false, -1};
    }
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

bool BatchGenerator::transferFiles(std::string const& video_dir,
                                   std::string const& video_name) {
    if (!config_.transfer.isValid()) {
        return true; // Not enabled, nothing to do
    }

    std::cout << "Transferring to " << config_.transfer.host << "... " << std::flush;

    // Expand ~ in identity file path
    std::string identity_file = config_.transfer.identity_file;
    if (!identity_file.empty() && identity_file[0] == '~') {
        char const* home = getenv("HOME");
        if (home) {
            identity_file = std::string(home) + identity_file.substr(1);
        }
    }

    // Build SSH options string (shared between ssh and scp)
    std::ostringstream ssh_opts;
    ssh_opts << "-o BatchMode=yes -o ConnectTimeout="
             << std::min(config_.transfer.timeout_seconds, 30);
    if (!identity_file.empty()) {
        ssh_opts << " -i \"" << identity_file << "\"";
    }

    // Create remote directory first
    std::string remote_video_dir = config_.transfer.remote_path + "/" + video_name;
    {
        std::ostringstream mkdir_cmd;
        mkdir_cmd << "ssh " << ssh_opts.str() << " "
                  << config_.transfer.host << " "
                  << "\"mkdir -p '" << remote_video_dir << "'\" 2>&1";

        FILE* pipe = popen(mkdir_cmd.str().c_str(), "r");
        if (!pipe) {
            std::cerr << "\nError: Failed to create remote directory\n";
            return false;
        }

        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }

        int result = pclose(pipe);
        if (result != 0) {
            std::cerr << "\nFailed to create remote directory: "
                      << (output.empty() ? "unknown error" : output);
            return false;
        }
    }

    // Files to transfer (metadata.json MUST be last - it's the "ready" signal for the watcher)
    std::vector<std::string> files = {"video_raw.mp4", "metadata.json"};
    bool all_success = true;

    for (auto const& filename : files) {
        std::string local_path = video_dir + "/" + filename;

        // Check if file exists
        if (!std::filesystem::exists(local_path)) {
            // video_raw.mp4 might be named video.mp4 in some cases
            if (filename == "video_raw.mp4") {
                local_path = video_dir + "/video.mp4";
                if (!std::filesystem::exists(local_path)) {
                    std::cerr << "\nWarning: No video file found in " << video_dir << "\n";
                    continue;
                }
            } else {
                std::cerr << "\nWarning: File not found: " << local_path << "\n";
                continue;
            }
        }

        // Build SCP command
        std::ostringstream scp_cmd;
        scp_cmd << "scp -q " << ssh_opts.str()
                << " -o ConnectTimeout=" << config_.transfer.timeout_seconds
                << " \"" << local_path << "\""
                << " \"" << config_.transfer.host << ":" << remote_video_dir << "/";

        // Keep the original filename for video.mp4 -> video_raw.mp4 mapping
        if (filename == "video_raw.mp4" && local_path.find("video.mp4") != std::string::npos) {
            scp_cmd << "video.mp4";
        } else {
            scp_cmd << filename;
        }
        scp_cmd << "\" 2>&1";

        FILE* pipe = popen(scp_cmd.str().c_str(), "r");
        if (!pipe) {
            std::cerr << "\nError: Failed to execute SCP command\n";
            all_success = false;
            continue;
        }

        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }

        int result = pclose(pipe);
        if (result != 0) {
            std::cerr << "\nSCP failed for " << filename << ": "
                      << (output.empty() ? "unknown error" : output);
            all_success = false;
        }
    }

    if (all_success) {
        std::cout << "OK\n";

        // Delete local files if configured and all transfers succeeded
        if (config_.transfer.delete_after_transfer) {
            std::error_code ec;
            std::filesystem::remove_all(video_dir, ec);
            if (ec) {
                std::cerr << "Warning: Could not delete " << video_dir
                          << ": " << ec.message() << "\n";
            }
        }
    }

    return all_success;
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
    std::cout << "║  Name                              Status  Boom(s) Uniform  Retries  Time  ║\n";
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

        // Uniformity
        if (r.success && r.final_uniformity > 0) {
            std::cout << std::right << std::fixed << std::setprecision(2) << std::setw(6)
                      << r.final_uniformity << "  ";
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
