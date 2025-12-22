#include "config.h"
#include <toml.hpp>
#include <iostream>
#include <filesystem>

namespace {

ColorScheme parseColorScheme(std::string const& str) {
    if (str == "spectrum") return ColorScheme::Spectrum;
    if (str == "rainbow") return ColorScheme::Rainbow;
    if (str == "heat") return ColorScheme::Heat;
    if (str == "cool") return ColorScheme::Cool;
    if (str == "monochrome") return ColorScheme::Monochrome;
    std::cerr << "Unknown color scheme: " << str << ", using spectrum\n";
    return ColorScheme::Spectrum;
}

OutputFormat parseOutputFormat(std::string const& str) {
    if (str == "png") return OutputFormat::PNG;
    if (str == "video") return OutputFormat::Video;
    std::cerr << "Unknown output format: " << str << ", using png\n";
    return OutputFormat::PNG;
}

// Safe value extraction helpers
template<typename T>
T get_or(toml::table const& tbl, std::string_view key, T default_val) {
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
            config.simulation.substeps_per_frame = get_or(*sim, "substeps_per_frame", 20);
        }

        // Render
        if (auto render = tbl["render"].as_table()) {
            config.render.width = get_or(*render, "width", 2160);
            config.render.height = get_or(*render, "height", 2160);
            config.render.thread_count = get_or(*render, "thread_count", 0);
        }

        // Post-process
        if (auto pp = tbl["post_process"].as_table()) {
            config.post_process.gamma = get_or(*pp, "gamma", 1.05);
            config.post_process.target_brightness = get_or(*pp, "target_brightness", 0.5);
            config.post_process.contrast = get_or(*pp, "contrast", 1.0);
            config.post_process.auto_normalize = get_or(*pp, "auto_normalize", true);
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
            config.detection.early_stop_after_white = get_or(*detect, "early_stop_after_white", false);
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

    } catch (toml::parse_error const& err) {
        std::cerr << "Error parsing config: " << err.description() << "\n";
        std::cerr << "Using defaults\n";
    }

    return config;
}
