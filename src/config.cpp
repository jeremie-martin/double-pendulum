#include "config.h"

#include <filesystem>
#include <iostream>
#include <toml.hpp>

namespace {

ColorScheme parseColorScheme(std::string const& str) {
    if (str == "spectrum")
        return ColorScheme::Spectrum;
    if (str == "rainbow")
        return ColorScheme::Rainbow;
    if (str == "heat")
        return ColorScheme::Heat;
    if (str == "cool")
        return ColorScheme::Cool;
    if (str == "monochrome")
        return ColorScheme::Monochrome;
    std::cerr << "Unknown color scheme: " << str << ", using spectrum\n";
    return ColorScheme::Spectrum;
}

OutputFormat parseOutputFormat(std::string const& str) {
    if (str == "png")
        return OutputFormat::PNG;
    if (str == "video")
        return OutputFormat::Video;
    std::cerr << "Unknown output format: " << str << ", using png\n";
    return OutputFormat::PNG;
}

ToneMapOperator parseToneMapOperator(std::string const& str) {
    if (str == "none")
        return ToneMapOperator::None;
    if (str == "reinhard")
        return ToneMapOperator::Reinhard;
    if (str == "reinhard_extended")
        return ToneMapOperator::ReinhardExtended;
    if (str == "aces")
        return ToneMapOperator::ACES;
    if (str == "logarithmic")
        return ToneMapOperator::Logarithmic;
    std::cerr << "Unknown tone map operator: " << str << ", using none\n";
    return ToneMapOperator::None;
}

NormalizationMode parseNormalizationMode(std::string const& str) {
    if (str == "per_frame")
        return NormalizationMode::PerFrame;
    if (str == "by_count")
        return NormalizationMode::ByCount;
    std::cerr << "Unknown normalization mode: " << str << ", using per_frame\n";
    return NormalizationMode::PerFrame;
}

PhysicsQuality parsePhysicsQuality(std::string const& str) {
    if (str == "low")
        return PhysicsQuality::Low;
    if (str == "medium")
        return PhysicsQuality::Medium;
    if (str == "high")
        return PhysicsQuality::High;
    if (str == "ultra")
        return PhysicsQuality::Ultra;
    std::cerr << "Unknown physics quality: " << str << ", using high\n";
    return PhysicsQuality::High;
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

Config Config::load(std::string const& path) {
    Config config;

    if (!std::filesystem::exists(path)) {
        std::cerr << "Config file not found: " << path << ", using defaults\n";
        return config;
    }

    try {
        auto tbl = toml::parse_file(path);

        // Physics
        if (auto physics = tbl["physics"].as_table()) {
            config.physics.gravity = get_or(*physics, "gravity", 9.81);
            config.physics.length1 = get_or(*physics, "length1", 1.0);
            config.physics.length2 = get_or(*physics, "length2", 1.0);
            config.physics.mass1 = get_or(*physics, "mass1", 1.0);
            config.physics.mass2 = get_or(*physics, "mass2", 1.0);
            config.physics.initial_angle1 = deg2rad(get_or(*physics, "initial_angle1_deg", -32.2));
            config.physics.initial_angle2 = deg2rad(get_or(*physics, "initial_angle2_deg", -32.0));
            config.physics.initial_velocity1 = get_or(*physics, "initial_velocity1", 0.0);
            config.physics.initial_velocity2 = get_or(*physics, "initial_velocity2", 0.0);
        }

        // Simulation
        if (auto sim = tbl["simulation"].as_table()) {
            config.simulation.pendulum_count = get_or(*sim, "pendulum_count", 100000);
            config.simulation.angle_variation = deg2rad(get_or(*sim, "angle_variation_deg", 0.1));
            config.simulation.duration_seconds = get_or(*sim, "duration_seconds", 11.0);
            config.simulation.total_frames = get_or(*sim, "total_frames", 660);

            // Physics quality: either use a preset or specify max_dt directly
            auto quality_str = get_string_or(*sim, "physics_quality", "");
            if (!quality_str.empty()) {
                config.simulation.physics_quality = parsePhysicsQuality(quality_str);
                config.simulation.max_dt = qualityToMaxDt(config.simulation.physics_quality);
            }
            // max_dt overrides quality preset if specified
            if (sim->contains("max_dt")) {
                config.simulation.max_dt = get_or(*sim, "max_dt", 0.007);
                config.simulation.physics_quality = PhysicsQuality::Custom;
            }
        }

        // Render
        if (auto render = tbl["render"].as_table()) {
            config.render.width = get_or(*render, "width", 2160);
            config.render.height = get_or(*render, "height", 2160);
            config.render.thread_count = get_or(*render, "thread_count", 0);
        }

        // Post-process
        if (auto pp = tbl["post_process"].as_table()) {
            auto tone_map_str = get_string_or(*pp, "tone_map", "none");
            config.post_process.tone_map = parseToneMapOperator(tone_map_str);
            config.post_process.reinhard_white_point = get_or(*pp, "reinhard_white_point", 1.0);
            config.post_process.exposure = get_or(*pp, "exposure", 0.0);
            config.post_process.contrast = get_or(*pp, "contrast", 1.0);
            config.post_process.gamma = get_or(*pp, "gamma", 2.2);
            auto norm_str = get_string_or(*pp, "normalization", "per_frame");
            config.post_process.normalization = parseNormalizationMode(norm_str);
        }

        // Color
        if (auto color = tbl["color"].as_table()) {
            auto scheme_str = get_string_or(*color, "scheme", "spectrum");
            config.color.scheme = parseColorScheme(scheme_str);
            config.color.start = get_or(*color, "start", 0.0);
            config.color.end = get_or(*color, "end", 1.0);
        }

        // Detection thresholds
        if (auto detect = tbl["detection"].as_table()) {
            config.detection.boom_threshold = get_or(*detect, "boom_threshold", 0.1);
            config.detection.boom_confirmation = get_or(*detect, "boom_confirmation", 10);
            config.detection.white_threshold = get_or(*detect, "white_threshold", 700.0);
            config.detection.white_confirmation = get_or(*detect, "white_confirmation", 10);
            config.detection.early_stop_after_white =
                get_or(*detect, "early_stop_after_white", false);
        }

        // Output
        if (auto out = tbl["output"].as_table()) {
            auto format_str = get_string_or(*out, "format", "png");
            config.output.format = parseOutputFormat(format_str);
            config.output.directory = get_string_or(*out, "directory", "output");
            config.output.filename_prefix = get_string_or(*out, "filename_prefix", "frame");
            config.output.video_codec = get_string_or(*out, "video_codec", "libx264");
            config.output.video_crf = get_or(*out, "video_crf", 23);
            config.output.video_fps = get_or(*out, "video_fps", 60);
        }

        // Analysis mode
        if (auto analysis_tbl = tbl["analysis"].as_table()) {
            config.analysis.enabled = get_or(*analysis_tbl, "enabled", false);
        }

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
        // Detection parameters
        else if (section == "detection") {
            if (param == "boom_threshold") {
                detection.boom_threshold = std::stod(value);
            } else if (param == "boom_confirmation") {
                detection.boom_confirmation = std::stoi(value);
            } else if (param == "white_threshold") {
                detection.white_threshold = std::stod(value);
            } else if (param == "white_confirmation") {
                detection.white_confirmation = std::stoi(value);
            } else if (param == "early_stop_after_white") {
                detection.early_stop_after_white = (value == "true" || value == "1");
            } else {
                std::cerr << "Unknown detection parameter: " << param << "\n";
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
