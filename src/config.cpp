#include "config.h"
#include "enum_utils.h"
#include "preset_library.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <toml.hpp>

namespace {

// Unified enum parsing using magic_enum
template <typename E>
E parseEnum(std::string const& str, E default_val, char const* type_name) {
    auto result = enum_utils::fromString<E>(str);
    if (result.has_value()) {
        return *result;
    }
    std::cerr << "Unknown " << type_name << ": " << str << ", using "
              << enum_utils::toString(default_val) << "\n";
    return default_val;
}

ColorScheme parseColorScheme(std::string const& str) {
    return parseEnum(str, ColorScheme::Spectrum, "color scheme");
}

OutputFormat parseOutputFormat(std::string const& str) {
    return parseEnum(str, OutputFormat::PNG, "output format");
}

ToneMapOperator parseToneMapOperator(std::string const& str) {
    return parseEnum(str, ToneMapOperator::None, "tone map operator");
}

NormalizationMode parseNormalizationMode(std::string const& str) {
    return parseEnum(str, NormalizationMode::PerFrame, "normalization mode");
}

PhysicsQuality parsePhysicsQuality(std::string const& str) {
    return parseEnum(str, PhysicsQuality::High, "physics quality");
}

// Safe value extraction helpers
template <typename T> T get_or(toml::table const& tbl, std::string_view key, T default_val) {
    if (auto node = tbl.get(key)) {
        if (auto val = node->value<T>()) {
            return *val;
        }
    }
    return default_val;
}

std::string get_string_or(toml::table const& tbl, std::string_view key, std::string default_val) {
    if (auto node = tbl.get(key)) {
        if (auto val = node->value<std::string>()) {
            return *val;
        }
    }
    return default_val;
}

} // namespace

Config Config::defaults() {
    return Config{};
}

// Load config values from a TOML table into an existing config (for include support)
static void loadConfigFromTable(Config& config, toml::table const& tbl) {
    // Physics
    if (auto physics = tbl["physics"].as_table()) {
        config.physics.gravity = get_or(*physics, "gravity", config.physics.gravity);
        config.physics.length1 = get_or(*physics, "length1", config.physics.length1);
        config.physics.length2 = get_or(*physics, "length2", config.physics.length2);
        config.physics.mass1 = get_or(*physics, "mass1", config.physics.mass1);
        config.physics.mass2 = get_or(*physics, "mass2", config.physics.mass2);
        if (physics->contains("initial_angle1_deg")) {
            config.physics.initial_angle1 = deg2rad(get_or(*physics, "initial_angle1_deg", 0.0));
        }
        if (physics->contains("initial_angle2_deg")) {
            config.physics.initial_angle2 = deg2rad(get_or(*physics, "initial_angle2_deg", 0.0));
        }
        config.physics.initial_velocity1 =
            get_or(*physics, "initial_velocity1", config.physics.initial_velocity1);
        config.physics.initial_velocity2 =
            get_or(*physics, "initial_velocity2", config.physics.initial_velocity2);
    }

    // Simulation
    if (auto sim = tbl["simulation"].as_table()) {
        config.simulation.pendulum_count =
            get_or(*sim, "pendulum_count", config.simulation.pendulum_count);
        if (sim->contains("angle_variation_deg")) {
            config.simulation.angle_variation = deg2rad(get_or(*sim, "angle_variation_deg", 0.0));
        }
        config.simulation.duration_seconds =
            get_or(*sim, "duration_seconds", config.simulation.duration_seconds);
        config.simulation.total_frames =
            get_or(*sim, "total_frames", config.simulation.total_frames);
        auto quality_str = get_string_or(*sim, "physics_quality", "");
        if (!quality_str.empty()) {
            config.simulation.physics_quality = parsePhysicsQuality(quality_str);
            config.simulation.max_dt = qualityToMaxDt(config.simulation.physics_quality);
        }
        if (sim->contains("max_dt")) {
            config.simulation.max_dt = get_or(*sim, "max_dt", config.simulation.max_dt);
            config.simulation.physics_quality = PhysicsQuality::Custom;
        }
    }

    // Render
    if (auto render = tbl["render"].as_table()) {
        config.render.width = get_or(*render, "width", config.render.width);
        config.render.height = get_or(*render, "height", config.render.height);
        config.render.thread_count = get_or(*render, "thread_count", config.render.thread_count);
    }

    // Post-process
    if (auto pp = tbl["post_process"].as_table()) {
        auto tone_map_str = get_string_or(*pp, "tone_map", "");
        if (!tone_map_str.empty()) {
            config.post_process.tone_map = parseToneMapOperator(tone_map_str);
        }
        config.post_process.reinhard_white_point =
            get_or(*pp, "reinhard_white_point", config.post_process.reinhard_white_point);
        config.post_process.exposure = get_or(*pp, "exposure", config.post_process.exposure);
        config.post_process.contrast = get_or(*pp, "contrast", config.post_process.contrast);
        config.post_process.gamma = get_or(*pp, "gamma", config.post_process.gamma);
        auto norm_str = get_string_or(*pp, "normalization", "");
        if (!norm_str.empty()) {
            config.post_process.normalization = parseNormalizationMode(norm_str);
        }
    }

    // Color
    if (auto color = tbl["color"].as_table()) {
        auto scheme_str = get_string_or(*color, "scheme", "");
        if (!scheme_str.empty()) {
            config.color.scheme = parseColorScheme(scheme_str);
        }
        config.color.start = get_or(*color, "start", config.color.start);
        config.color.end = get_or(*color, "end", config.color.end);
    }

    // Per-metric configuration (new format: [metrics.metric_name])
    if (auto metrics_tbl = tbl["metrics"].as_table()) {
        // Check for deprecated old format (has direct min_sectors, etc.)
        if (metrics_tbl->contains("min_sectors") || metrics_tbl->contains("max_sectors")) {
            throw std::runtime_error(
                "Old [metrics] format deprecated. Use [metrics.metric_name] sections.\n"
                "Example: [metrics.angular_causticness] with min_sectors, max_sectors, etc.");
        }

        // Parse each per-metric section
        for (auto const& [key, value] : *metrics_tbl) {
            if (auto metric_tbl = value.as_table()) {
                std::string metric_name(key);
                MetricConfig mc = createDefaultMetricConfig(metric_name);

                // Parse metric-specific params based on type
                MetricType type = getMetricType(metric_name);
                switch (type) {
                case MetricType::Sector: {
                    auto& p = std::get<SectorMetricParams>(mc.params);
                    p.min_sectors = get_or(*metric_tbl, "min_sectors", p.min_sectors);
                    p.max_sectors = get_or(*metric_tbl, "max_sectors", p.max_sectors);
                    p.target_per_sector =
                        get_or(*metric_tbl, "target_per_sector", p.target_per_sector);
                    break;
                }
                case MetricType::CVSector: {
                    auto& p = std::get<CVSectorMetricParams>(mc.params);
                    p.min_sectors = get_or(*metric_tbl, "min_sectors", p.min_sectors);
                    p.max_sectors = get_or(*metric_tbl, "max_sectors", p.max_sectors);
                    p.target_per_sector =
                        get_or(*metric_tbl, "target_per_sector", p.target_per_sector);
                    p.cv_normalization =
                        get_or(*metric_tbl, "cv_normalization", p.cv_normalization);
                    break;
                }
                case MetricType::LocalCoherence: {
                    auto& p = std::get<LocalCoherenceMetricParams>(mc.params);
                    p.max_radius = get_or(*metric_tbl, "max_radius", p.max_radius);
                    p.min_spread_threshold =
                        get_or(*metric_tbl, "min_spread_threshold", p.min_spread_threshold);
                    p.log_inverse_baseline =
                        get_or(*metric_tbl, "log_inverse_baseline", p.log_inverse_baseline);
                    p.log_inverse_divisor =
                        get_or(*metric_tbl, "log_inverse_divisor", p.log_inverse_divisor);
                    break;
                }
                case MetricType::None:
                    // No parameters to parse for parameter-less metrics
                    break;
                }

                config.metric_configs[metric_name] = mc;
            }
        }
    }

    // Boom detection - which metric to use
    if (auto boom_tbl = tbl["boom_detection"].as_table()) {
        auto active_metric = get_string_or(*boom_tbl, "active_metric", "");
        if (!active_metric.empty()) {
            config.boom_metric = active_metric;
        }
    }

    // Multi-target configuration (new format: [targets.X])
    if (auto targets_tbl = tbl["targets"].as_table()) {
        for (auto const& [key, value] : *targets_tbl) {
            if (auto target_tbl = value.as_table()) {
                TargetConfig tc;
                tc.name = std::string(key);
                tc.type = get_string_or(*target_tbl, "type", "frame");
                tc.metric = get_string_or(*target_tbl, "metric", "");
                if (tc.metric.empty()) {
                    std::cerr << "Warning: Target '" << tc.name << "' has no metric specified\n";
                }
                tc.method = get_string_or(*target_tbl, "method", "max_value");

                // Frame detection parameters
                tc.offset_seconds = get_or(*target_tbl, "offset_seconds", tc.offset_seconds);
                tc.peak_percent_threshold =
                    get_or(*target_tbl, "peak_percent_threshold", tc.peak_percent_threshold);
                tc.min_peak_prominence =
                    get_or(*target_tbl, "min_peak_prominence", tc.min_peak_prominence);
                tc.smoothing_window = get_or(*target_tbl, "smoothing_window", tc.smoothing_window);
                tc.crossing_threshold =
                    get_or(*target_tbl, "crossing_threshold", tc.crossing_threshold);
                tc.crossing_confirmation =
                    get_or(*target_tbl, "crossing_confirmation", tc.crossing_confirmation);

                // Score weights (for composite scoring)
                if (auto weights_arr = target_tbl->get("weights")) {
                    if (auto weights_array = weights_arr->as_array()) {
                        for (auto const& w : *weights_array) {
                            if (auto w_tbl = w.as_table()) {
                                auto score_name = get_string_or(*w_tbl, "name", "");
                                auto weight = get_or(*w_tbl, "weight", 1.0);
                                if (!score_name.empty()) {
                                    tc.weights.emplace_back(score_name, weight);
                                }
                            }
                        }
                    }
                }

                // Check if a target with this name already exists (from includes)
                // If so, override it; otherwise add as new
                auto it = std::find_if(config.targets.begin(), config.targets.end(),
                                       [&](auto const& t) { return t.name == tc.name; });
                if (it != config.targets.end()) {
                    *it = tc; // Override existing target
                } else {
                    config.targets.push_back(tc); // Add new target
                }
            }
        }
    }

    // Output
    if (auto out = tbl["output"].as_table()) {
        auto format_str = get_string_or(*out, "format", "");
        if (!format_str.empty()) {
            config.output.format = parseOutputFormat(format_str);
        }
        auto dir = get_string_or(*out, "directory", "");
        if (!dir.empty()) {
            config.output.directory = dir;
        }
        auto prefix = get_string_or(*out, "filename_prefix", "");
        if (!prefix.empty()) {
            config.output.filename_prefix = prefix;
        }
        auto codec = get_string_or(*out, "video_codec", "");
        if (!codec.empty()) {
            config.output.video_codec = codec;
        }
        config.output.video_crf = get_or(*out, "video_crf", config.output.video_crf);
        config.output.video_fps = get_or(*out, "video_fps", config.output.video_fps);
        if (out->contains("save_simulation_data")) {
            config.output.save_simulation_data =
                get_or(*out, "save_simulation_data", config.output.save_simulation_data);
        }
    }

    // Analysis mode
    if (auto analysis_tbl = tbl["analysis"].as_table()) {
        config.analysis.enabled = get_or(*analysis_tbl, "enabled", config.analysis.enabled);
    }
}

Config Config::load(std::string const& path) {
    Config config;

    if (!std::filesystem::exists(path)) {
        std::cerr << "Config file not found: " << path << ", using defaults\n";
        return config;
    }

    try {
        auto tbl = toml::parse_file(path);
        std::string base_path = std::filesystem::path(path).parent_path().string();
        if (base_path.empty())
            base_path = ".";

        // Process includes first (they provide base values that can be overridden)
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
                        try {
                            auto inc_tbl = toml::parse_file(full_path.string());
                            loadConfigFromTable(config, inc_tbl);
                        } catch (toml::parse_error const& err) {
                            std::cerr << "Error parsing included config " << full_path << ": "
                                      << err.description() << "\n";
                        }
                    } else {
                        std::cerr << "Warning: Included config not found: " << full_path << "\n";
                    }
                }
            }
        }

        // Load values from this file (override includes)
        loadConfigFromTable(config, tbl);

    } catch (toml::parse_error const& err) {
        std::cerr << "Error parsing config: " << err.description() << "\n";
        std::cerr << "Using defaults\n";
    }

    // Minimal validation - only catch obviously broken values
    Config defaults;
    if (config.simulation.pendulum_count <= 0) {
        std::cerr << "Warning: pendulum_count must be positive, using default ("
                  << defaults.simulation.pendulum_count << ")\n";
        config.simulation.pendulum_count = defaults.simulation.pendulum_count;
    }
    if (config.simulation.total_frames <= 0) {
        std::cerr << "Warning: total_frames must be positive, using default ("
                  << defaults.simulation.total_frames << ")\n";
        config.simulation.total_frames = defaults.simulation.total_frames;
    }
    if (config.simulation.max_dt <= 0) {
        std::cerr << "Warning: max_dt must be positive, using default ("
                  << defaults.simulation.max_dt << ")\n";
        config.simulation.max_dt = defaults.simulation.max_dt;
    }
    if (config.render.width <= 0) {
        std::cerr << "Warning: width must be positive, using default (" << defaults.render.width
                  << ")\n";
        config.render.width = defaults.render.width;
    }
    if (config.render.height <= 0) {
        std::cerr << "Warning: height must be positive, using default (" << defaults.render.height
                  << ")\n";
        config.render.height = defaults.render.height;
    }

    // Physics validation
    if (config.physics.mass1 <= 0 || config.physics.mass2 <= 0) {
        std::cerr << "Warning: masses must be positive, using defaults\n";
        config.physics.mass1 = defaults.physics.mass1;
        config.physics.mass2 = defaults.physics.mass2;
    }
    if (config.physics.length1 <= 0 || config.physics.length2 <= 0) {
        std::cerr << "Warning: lengths must be positive, using defaults\n";
        config.physics.length1 = defaults.physics.length1;
        config.physics.length2 = defaults.physics.length2;
    }

    // Post-processing validation
    if (config.post_process.gamma <= 0) {
        std::cerr << "Warning: gamma must be positive, using default ("
                  << defaults.post_process.gamma << ")\n";
        config.post_process.gamma = defaults.post_process.gamma;
    }
    if (config.post_process.contrast < 0) {
        std::cerr << "Warning: contrast cannot be negative, using default ("
                  << defaults.post_process.contrast << ")\n";
        config.post_process.contrast = defaults.post_process.contrast;
    }

    // Color range validation
    if (config.color.start < 0 || config.color.start > 1 || config.color.end < 0 ||
        config.color.end > 1) {
        std::cerr << "Warning: color range values must be in [0,1], using defaults\n";
        config.color.start = defaults.color.start;
        config.color.end = defaults.color.end;
    }

    return config;
}

bool Config::applyOverride(std::string const& key, std::string const& value) {
    // Parse dot-notation key (e.g., "simulation.pendulum_count")
    auto dot_pos = key.find('.');
    if (dot_pos == std::string::npos) {
        std::cerr << "Invalid parameter key (missing section): " << key << "\n";
        return false;
    }

    std::string section = key.substr(0, dot_pos);
    std::string param = key.substr(dot_pos + 1);

    try {
        // Physics parameters
        if (section == "physics") {
            if (param == "gravity") {
                physics.gravity = std::stod(value);
            } else if (param == "length1") {
                physics.length1 = std::stod(value);
            } else if (param == "length2") {
                physics.length2 = std::stod(value);
            } else if (param == "mass1") {
                physics.mass1 = std::stod(value);
            } else if (param == "mass2") {
                physics.mass2 = std::stod(value);
            } else if (param == "initial_angle1_deg") {
                physics.initial_angle1 = deg2rad(std::stod(value));
            } else if (param == "initial_angle2_deg") {
                physics.initial_angle2 = deg2rad(std::stod(value));
            } else if (param == "initial_velocity1") {
                physics.initial_velocity1 = std::stod(value);
            } else if (param == "initial_velocity2") {
                physics.initial_velocity2 = std::stod(value);
            } else {
                std::cerr << "Unknown physics parameter: " << param << "\n";
                return false;
            }
        }
        // Simulation parameters
        else if (section == "simulation") {
            if (param == "pendulum_count") {
                simulation.pendulum_count = std::stoi(value);
            } else if (param == "angle_variation_deg") {
                simulation.angle_variation = deg2rad(std::stod(value));
            } else if (param == "duration_seconds") {
                simulation.duration_seconds = std::stod(value);
            } else if (param == "total_frames") {
                simulation.total_frames = std::stoi(value);
            } else if (param == "physics_quality") {
                simulation.physics_quality = parsePhysicsQuality(value);
                simulation.max_dt = qualityToMaxDt(simulation.physics_quality);
            } else if (param == "max_dt") {
                simulation.max_dt = std::stod(value);
                simulation.physics_quality = PhysicsQuality::Custom;
            } else {
                std::cerr << "Unknown simulation parameter: " << param << "\n";
                return false;
            }
        }
        // Render parameters
        else if (section == "render") {
            if (param == "width") {
                render.width = std::stoi(value);
            } else if (param == "height") {
                render.height = std::stoi(value);
            } else if (param == "thread_count") {
                render.thread_count = std::stoi(value);
            } else {
                std::cerr << "Unknown render parameter: " << param << "\n";
                return false;
            }
        }
        // Post-process parameters
        else if (section == "post_process") {
            if (param == "tone_map") {
                post_process.tone_map = parseToneMapOperator(value);
            } else if (param == "reinhard_white_point") {
                post_process.reinhard_white_point = std::stod(value);
            } else if (param == "exposure") {
                post_process.exposure = std::stod(value);
            } else if (param == "contrast") {
                post_process.contrast = std::stod(value);
            } else if (param == "gamma") {
                post_process.gamma = std::stod(value);
            } else if (param == "normalization") {
                post_process.normalization = parseNormalizationMode(value);
            } else {
                std::cerr << "Unknown post_process parameter: " << param << "\n";
                return false;
            }
        }
        // Color parameters
        else if (section == "color") {
            if (param == "scheme") {
                color.scheme = parseColorScheme(value);
            } else if (param == "start") {
                color.start = std::stod(value);
            } else if (param == "end") {
                color.end = std::stod(value);
            } else {
                std::cerr << "Unknown color parameter: " << param << "\n";
                return false;
            }
        }
        // Output parameters
        else if (section == "output") {
            if (param == "format") {
                output.format = parseOutputFormat(value);
            } else if (param == "directory") {
                output.directory = value;
            } else if (param == "filename_prefix") {
                output.filename_prefix = value;
            } else if (param == "video_codec") {
                output.video_codec = value;
            } else if (param == "video_crf") {
                output.video_crf = std::stoi(value);
            } else if (param == "video_fps") {
                output.video_fps = std::stoi(value);
            } else if (param == "save_simulation_data") {
                output.save_simulation_data = (value == "true" || value == "1");
            } else {
                std::cerr << "Unknown output parameter: " << param << "\n";
                return false;
            }
        }
        // Analysis parameters
        else if (section == "analysis") {
            if (param == "enabled") {
                analysis.enabled = (value == "true" || value == "1");
            } else {
                std::cerr << "Unknown analysis parameter: " << param << "\n";
                return false;
            }
        }
        // Per-metric parameters (format: metrics.metric_name.param or
        // metrics.metric_name.boom.param)
        else if (section == "metrics") {
            // Parse: metrics.angular_causticness.min_sectors -> metric_name=angular_causticness,
            // param=min_sectors Or: metrics.angular_causticness.boom.method ->
            // metric_name=angular_causticness, boom_param=method
            auto second_dot = param.find('.');
            if (second_dot == std::string::npos) {
                std::cerr << "Invalid metrics key format. Use metrics.metric_name.param\n";
                return false;
            }

            std::string metric_name = param.substr(0, second_dot);
            std::string rest = param.substr(second_dot + 1);

            // Get or create metric config
            MetricConfig& mc = getOrCreateMetricConfig(metric_name);
            MetricType type = getMetricType(metric_name);

            // Metric-specific parameter
            bool handled = false;
            {
                switch (type) {
                case MetricType::Sector: {
                    auto& p = std::get<SectorMetricParams>(mc.params);
                    if (rest == "min_sectors") {
                        p.min_sectors = std::stoi(value);
                        handled = true;
                    } else if (rest == "max_sectors") {
                        p.max_sectors = std::stoi(value);
                        handled = true;
                    } else if (rest == "target_per_sector") {
                        p.target_per_sector = std::stoi(value);
                        handled = true;
                    }
                    break;
                }
                case MetricType::CVSector: {
                    auto& p = std::get<CVSectorMetricParams>(mc.params);
                    if (rest == "min_sectors") {
                        p.min_sectors = std::stoi(value);
                        handled = true;
                    } else if (rest == "max_sectors") {
                        p.max_sectors = std::stoi(value);
                        handled = true;
                    } else if (rest == "target_per_sector") {
                        p.target_per_sector = std::stoi(value);
                        handled = true;
                    } else if (rest == "cv_normalization") {
                        p.cv_normalization = std::stod(value);
                        handled = true;
                    }
                    break;
                }
                case MetricType::LocalCoherence: {
                    auto& p = std::get<LocalCoherenceMetricParams>(mc.params);
                    if (rest == "max_radius") {
                        p.max_radius = std::stod(value);
                        handled = true;
                    } else if (rest == "min_spread_threshold") {
                        p.min_spread_threshold = std::stod(value);
                        handled = true;
                    } else if (rest == "log_inverse_baseline") {
                        p.log_inverse_baseline = std::stod(value);
                        handled = true;
                    } else if (rest == "log_inverse_divisor") {
                        p.log_inverse_divisor = std::stod(value);
                        handled = true;
                    }
                    break;
                }
                case MetricType::None:
                    // Parameter-less metrics don't have params to set
                    break;
                }

                if (!handled) {
                    std::cerr << "Unknown parameter '" << rest << "' for metric " << metric_name
                              << "\n";
                    return false;
                }
            }
        }
        // Boom detection - which metric to use
        else if (section == "boom_detection") {
            if (param == "active_metric") {
                boom_metric = value;
            } else {
                std::cerr << "Unknown boom_detection parameter: " << param << "\n";
                return false;
            }
        }
        // Preset handling - apply theme/color/post_process from presets file
        else if (section == "preset") {
            // Load presets file if it exists
            if (!std::filesystem::exists(presets_file)) {
                std::cerr << "Presets file not found: " << presets_file << "\n";
                return false;
            }
            PresetLibrary presets = PresetLibrary::load(presets_file);

            if (param == "theme") {
                auto theme = presets.getTheme(value);
                if (!theme) {
                    std::cerr << "Theme not found: " << value << "\n";
                    std::cerr << "Available themes: ";
                    auto names = presets.getThemeNames();
                    for (size_t i = 0; i < std::min(names.size(), size_t(10)); ++i) {
                        if (i > 0) std::cerr << ", ";
                        std::cerr << names[i];
                    }
                    if (names.size() > 10) std::cerr << "... (" << names.size() << " total)";
                    std::cerr << "\n";
                    return false;
                }
                // Apply color from theme
                if (auto color_preset = presets.getColor(theme->color_preset_name)) {
                    color = *color_preset;
                    selected_color_preset_name = theme->color_preset_name;
                } else {
                    std::cerr << "Theme references unknown color: " << theme->color_preset_name << "\n";
                    return false;
                }
                // Apply post_process from theme
                if (auto pp_preset = presets.getPostProcess(theme->post_process_preset_name)) {
                    post_process = *pp_preset;
                    selected_post_process_preset_name = theme->post_process_preset_name;
                } else {
                    std::cerr << "Theme references unknown post_process: " << theme->post_process_preset_name << "\n";
                    return false;
                }
                selected_theme_name = value;
            } else if (param == "color") {
                auto preset = presets.getColor(value);
                if (!preset) {
                    std::cerr << "Color preset not found: " << value << "\n";
                    return false;
                }
                color = *preset;
                selected_color_preset_name = value;
            } else if (param == "post_process") {
                auto preset = presets.getPostProcess(value);
                if (!preset) {
                    std::cerr << "Post-process preset not found: " << value << "\n";
                    return false;
                }
                post_process = *preset;
                selected_post_process_preset_name = value;
            } else if (param == "file") {
                presets_file = value;
            } else {
                std::cerr << "Unknown preset parameter: " << param << "\n";
                std::cerr << "Valid options: theme, color, post_process, file\n";
                return false;
            }
        } else {
            std::cerr << "Unknown section: " << section << "\n";
            return false;
        }
    } catch (std::exception const& e) {
        std::cerr << "Error parsing value for " << key << ": " << e.what() << "\n";
        return false;
    }

    return true;
}

// Serialization uses enum_utils::toString() directly - no duplicates needed

void Config::save(std::string const& path) const {
    std::ofstream file(path);
    if (!file) {
        std::cerr << "Error: Could not open " << path << " for writing\n";
        return;
    }

    file << std::fixed << std::setprecision(6);

    // Physics section
    file << "[physics]\n";
    file << "gravity = " << physics.gravity << "\n";
    file << "length1 = " << physics.length1 << "\n";
    file << "length2 = " << physics.length2 << "\n";
    file << "mass1 = " << physics.mass1 << "\n";
    file << "mass2 = " << physics.mass2 << "\n";
    file << "initial_angle1_deg = " << rad2deg(physics.initial_angle1) << "\n";
    file << "initial_angle2_deg = " << rad2deg(physics.initial_angle2) << "\n";
    file << "initial_velocity1 = " << physics.initial_velocity1 << "\n";
    file << "initial_velocity2 = " << physics.initial_velocity2 << "\n";
    file << "\n";

    // Simulation section
    file << "[simulation]\n";
    file << "pendulum_count = " << simulation.pendulum_count << "\n";
    file << "angle_variation_deg = " << rad2deg(simulation.angle_variation) << "\n";
    file << "duration_seconds = " << simulation.duration_seconds << "\n";
    file << "total_frames = " << simulation.total_frames << "\n";
    file << "physics_quality = \"" << enum_utils::toString(simulation.physics_quality) << "\"\n";
    file << "max_dt = " << simulation.max_dt << "\n";
    file << "\n";

    // Render section
    file << "[render]\n";
    file << "width = " << render.width << "\n";
    file << "height = " << render.height << "\n";
    file << "thread_count = " << render.thread_count << "\n";
    file << "\n";

    // Post-process section
    file << "[post_process]\n";
    file << "tone_map = \"" << enum_utils::toString(post_process.tone_map) << "\"\n";
    file << "exposure = " << post_process.exposure << "\n";
    file << "contrast = " << post_process.contrast << "\n";
    file << "gamma = " << post_process.gamma << "\n";
    file << "normalization = \"" << enum_utils::toString(post_process.normalization) << "\"\n";
    file << "reinhard_white_point = " << post_process.reinhard_white_point << "\n";
    file << "\n";

    // Color section
    file << "[color]\n";
    file << "scheme = \"" << enum_utils::toString(color.scheme) << "\"\n";
    file << "start = " << color.start << "\n";
    file << "end = " << color.end << "\n";
    file << "\n";

    // Per-metric configuration sections
    for (auto const& [name, mc] : metric_configs) {
        file << "[metrics." << name << "]\n";

        // Write metric-specific params based on type
        MetricType type = getMetricType(name);
        switch (type) {
        case MetricType::Sector: {
            auto const& p = std::get<SectorMetricParams>(mc.params);
            file << "min_sectors = " << p.min_sectors << "\n";
            file << "max_sectors = " << p.max_sectors << "\n";
            file << "target_per_sector = " << p.target_per_sector << "\n";
            break;
        }
        case MetricType::CVSector: {
            auto const& p = std::get<CVSectorMetricParams>(mc.params);
            file << "min_sectors = " << p.min_sectors << "\n";
            file << "max_sectors = " << p.max_sectors << "\n";
            file << "target_per_sector = " << p.target_per_sector << "\n";
            file << "cv_normalization = " << p.cv_normalization << "\n";
            break;
        }
        case MetricType::LocalCoherence: {
            auto const& p = std::get<LocalCoherenceMetricParams>(mc.params);
            file << "max_radius = " << p.max_radius << "\n";
            file << "min_spread_threshold = " << p.min_spread_threshold << "\n";
            file << "log_inverse_baseline = " << p.log_inverse_baseline << "\n";
            file << "log_inverse_divisor = " << p.log_inverse_divisor << "\n";
            break;
        }
        case MetricType::None:
            // No parameters to write for parameter-less metrics
            break;
        }
        file << "\n";
    }

    // Multi-target configuration
    for (auto const& tc : targets) {
        file << "[targets." << tc.name << "]\n";
        file << "type = \"" << tc.type << "\"\n";
        file << "metric = \"" << tc.metric << "\"\n";
        file << "method = \"" << tc.method << "\"\n";

        if (tc.type == "frame") {
            // offset_seconds is used by all frame methods
            file << "offset_seconds = " << tc.offset_seconds << "\n";

            // Only write method-specific params
            if (tc.method == "first_peak_percent") {
                file << "peak_percent_threshold = " << tc.peak_percent_threshold << "\n";
                file << "min_peak_prominence = " << tc.min_peak_prominence << "\n";
            } else if (tc.method == "derivative_peak" || tc.method == "second_derivative_peak") {
                file << "smoothing_window = " << tc.smoothing_window << "\n";
            } else if (tc.method == "threshold_crossing") {
                file << "crossing_threshold = " << tc.crossing_threshold << "\n";
                file << "crossing_confirmation = " << tc.crossing_confirmation << "\n";
            }
            // max_value has no additional params
        }

        if (!tc.weights.empty()) {
            file << "weights = [\n";
            for (auto const& [name, weight] : tc.weights) {
                file << "  { name = \"" << name << "\", weight = " << weight << " },\n";
            }
            file << "]\n";
        }
        file << "\n";
    }

    // Output section
    file << "[output]\n";
    file << "format = \"" << enum_utils::toString(output.format) << "\"\n";
    file << "directory = \"" << output.directory << "\"\n";
    file << "filename_prefix = \"" << output.filename_prefix << "\"\n";
    file << "video_codec = \"" << output.video_codec << "\"\n";
    file << "video_crf = " << output.video_crf << "\n";
    file << "video_fps = " << output.video_fps << "\n";
    file << "save_simulation_data = " << (output.save_simulation_data ? "true" : "false") << "\n";
    file << "\n";

    // Analysis section
    file << "[analysis]\n";
    file << "enabled = " << (analysis.enabled ? "true" : "false") << "\n";
}
