#include "config.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
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
    if (str == "plasma")
        return ColorScheme::Plasma;
    if (str == "viridis")
        return ColorScheme::Viridis;
    if (str == "inferno")
        return ColorScheme::Inferno;
    if (str == "sunset")
        return ColorScheme::Sunset;
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

BoomDetectionMethod parseBoomDetectionMethod(std::string const& str) {
    if (str == "max_causticness")
        return BoomDetectionMethod::MaxCausticness;
    if (str == "first_peak_percent")
        return BoomDetectionMethod::FirstPeakPercent;
    if (str == "derivative_peak")
        return BoomDetectionMethod::DerivativePeak;
    if (str == "threshold_crossing")
        return BoomDetectionMethod::ThresholdCrossing;
    if (str == "second_derivative_peak")
        return BoomDetectionMethod::SecondDerivativePeak;
    std::cerr << "Unknown boom detection method: " << str << ", using max_causticness\n";
    return BoomDetectionMethod::MaxCausticness;
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
        config.physics.initial_velocity1 = get_or(*physics, "initial_velocity1", config.physics.initial_velocity1);
        config.physics.initial_velocity2 = get_or(*physics, "initial_velocity2", config.physics.initial_velocity2);
    }

    // Simulation
    if (auto sim = tbl["simulation"].as_table()) {
        config.simulation.pendulum_count = get_or(*sim, "pendulum_count", config.simulation.pendulum_count);
        if (sim->contains("angle_variation_deg")) {
            config.simulation.angle_variation = deg2rad(get_or(*sim, "angle_variation_deg", 0.0));
        }
        config.simulation.duration_seconds = get_or(*sim, "duration_seconds", config.simulation.duration_seconds);
        config.simulation.total_frames = get_or(*sim, "total_frames", config.simulation.total_frames);
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
        config.post_process.reinhard_white_point = get_or(*pp, "reinhard_white_point", config.post_process.reinhard_white_point);
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

    // Metrics parameters
    if (auto metrics_tbl = tbl["metrics"].as_table()) {
        config.metrics.min_sectors = get_or(*metrics_tbl, "min_sectors", config.metrics.min_sectors);
        config.metrics.max_sectors = get_or(*metrics_tbl, "max_sectors", config.metrics.max_sectors);
        config.metrics.target_per_sector = get_or(*metrics_tbl, "target_per_sector", config.metrics.target_per_sector);
        config.metrics.min_grid = get_or(*metrics_tbl, "min_grid", config.metrics.min_grid);
        config.metrics.max_grid = get_or(*metrics_tbl, "max_grid", config.metrics.max_grid);
        config.metrics.target_per_cell = get_or(*metrics_tbl, "target_per_cell", config.metrics.target_per_cell);
        config.metrics.max_radius = get_or(*metrics_tbl, "max_radius", config.metrics.max_radius);
        config.metrics.cv_normalization = get_or(*metrics_tbl, "cv_normalization", config.metrics.cv_normalization);
        config.metrics.log_ratio_normalization = get_or(*metrics_tbl, "log_ratio_normalization", config.metrics.log_ratio_normalization);
        config.metrics.min_spread_threshold = get_or(*metrics_tbl, "min_spread_threshold", config.metrics.min_spread_threshold);
        config.metrics.gini_chaos_baseline = get_or(*metrics_tbl, "gini_chaos_baseline", config.metrics.gini_chaos_baseline);
        config.metrics.gini_baseline_divisor = get_or(*metrics_tbl, "gini_baseline_divisor", config.metrics.gini_baseline_divisor);
        config.metrics.log_inverse_baseline = get_or(*metrics_tbl, "log_inverse_baseline", config.metrics.log_inverse_baseline);
        config.metrics.log_inverse_divisor = get_or(*metrics_tbl, "log_inverse_divisor", config.metrics.log_inverse_divisor);
    }

    // Boom detection parameters
    if (auto boom_tbl = tbl["boom_detection"].as_table()) {
        auto method_str = get_string_or(*boom_tbl, "method", "");
        if (!method_str.empty()) {
            config.boom.method = parseBoomDetectionMethod(method_str);
        }
        config.boom.offset_seconds = get_or(*boom_tbl, "offset_seconds", config.boom.offset_seconds);
        config.boom.peak_percent_threshold = get_or(*boom_tbl, "peak_percent_threshold", config.boom.peak_percent_threshold);
        config.boom.min_peak_prominence = get_or(*boom_tbl, "min_peak_prominence", config.boom.min_peak_prominence);
        config.boom.smoothing_window = get_or(*boom_tbl, "smoothing_window", config.boom.smoothing_window);
        config.boom.crossing_threshold = get_or(*boom_tbl, "crossing_threshold", config.boom.crossing_threshold);
        config.boom.crossing_confirmation = get_or(*boom_tbl, "crossing_confirmation", config.boom.crossing_confirmation);
        auto metric_name = get_string_or(*boom_tbl, "metric_name", "");
        if (!metric_name.empty()) {
            config.boom.metric_name = metric_name;
        }
    }

    // Detection thresholds
    if (auto detect = tbl["detection"].as_table()) {
        config.detection.boom_threshold = get_or(*detect, "boom_threshold", config.detection.boom_threshold);
        config.detection.boom_confirmation = get_or(*detect, "boom_confirmation", config.detection.boom_confirmation);
        if (detect->contains("chaos_threshold")) {
            config.detection.chaos_threshold = get_or(*detect, "chaos_threshold", config.detection.chaos_threshold);
        } else if (detect->contains("white_threshold")) {
            config.detection.chaos_threshold = get_or(*detect, "white_threshold", config.detection.chaos_threshold);
        }
        if (detect->contains("chaos_confirmation")) {
            config.detection.chaos_confirmation = get_or(*detect, "chaos_confirmation", config.detection.chaos_confirmation);
        } else if (detect->contains("white_confirmation")) {
            config.detection.chaos_confirmation = get_or(*detect, "white_confirmation", config.detection.chaos_confirmation);
        }
        if (detect->contains("early_stop_after_chaos")) {
            config.detection.early_stop_after_chaos = get_or(*detect, "early_stop_after_chaos", config.detection.early_stop_after_chaos);
        } else if (detect->contains("early_stop_after_white")) {
            config.detection.early_stop_after_chaos = get_or(*detect, "early_stop_after_white", config.detection.early_stop_after_chaos);
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
            config.output.save_simulation_data = get_or(*out, "save_simulation_data", config.output.save_simulation_data);
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
        if (base_path.empty()) base_path = ".";

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
                            std::cerr << "Error parsing included config " << full_path << ": " << err.description() << "\n";
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
    if (config.color.start < 0 || config.color.start > 1 ||
        config.color.end < 0 || config.color.end > 1) {
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
        // Detection parameters
        else if (section == "detection") {
            if (param == "boom_threshold") {
                detection.boom_threshold = std::stod(value);
            } else if (param == "boom_confirmation") {
                detection.boom_confirmation = std::stoi(value);
            } else if (param == "chaos_threshold" || param == "white_threshold") {
                detection.chaos_threshold = std::stod(value);
            } else if (param == "chaos_confirmation" || param == "white_confirmation") {
                detection.chaos_confirmation = std::stoi(value);
            } else if (param == "early_stop_after_chaos" || param == "early_stop_after_white") {
                detection.early_stop_after_chaos = (value == "true" || value == "1");
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
        // Metrics parameters
        else if (section == "metrics") {
            if (param == "min_sectors") {
                metrics.min_sectors = std::stoi(value);
            } else if (param == "max_sectors") {
                metrics.max_sectors = std::stoi(value);
            } else if (param == "target_per_sector") {
                metrics.target_per_sector = std::stoi(value);
            } else if (param == "min_grid") {
                metrics.min_grid = std::stoi(value);
            } else if (param == "max_grid") {
                metrics.max_grid = std::stoi(value);
            } else if (param == "target_per_cell") {
                metrics.target_per_cell = std::stoi(value);
            } else if (param == "max_radius") {
                metrics.max_radius = std::stod(value);
            } else if (param == "cv_normalization") {
                metrics.cv_normalization = std::stod(value);
            } else if (param == "log_ratio_normalization") {
                metrics.log_ratio_normalization = std::stod(value);
            } else if (param == "min_spread_threshold") {
                metrics.min_spread_threshold = std::stod(value);
            } else if (param == "gini_chaos_baseline") {
                metrics.gini_chaos_baseline = std::stod(value);
            } else if (param == "gini_baseline_divisor") {
                metrics.gini_baseline_divisor = std::stod(value);
            } else if (param == "log_inverse_baseline") {
                metrics.log_inverse_baseline = std::stod(value);
            } else if (param == "log_inverse_divisor") {
                metrics.log_inverse_divisor = std::stod(value);
            } else {
                std::cerr << "Unknown metrics parameter: " << param << "\n";
                return false;
            }
        }
        // Boom detection parameters
        else if (section == "boom_detection" || section == "boom") {
            if (param == "method") {
                boom.method = parseBoomDetectionMethod(value);
            } else if (param == "offset_seconds") {
                boom.offset_seconds = std::stod(value);
            } else if (param == "peak_percent_threshold") {
                boom.peak_percent_threshold = std::stod(value);
            } else if (param == "min_peak_prominence") {
                boom.min_peak_prominence = std::stod(value);
            } else if (param == "smoothing_window") {
                boom.smoothing_window = std::stoi(value);
            } else if (param == "crossing_threshold") {
                boom.crossing_threshold = std::stod(value);
            } else if (param == "crossing_confirmation") {
                boom.crossing_confirmation = std::stoi(value);
            } else if (param == "metric_name") {
                boom.metric_name = value;
            } else {
                std::cerr << "Unknown boom_detection parameter: " << param << "\n";
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

namespace {
std::string colorSchemeToString(ColorScheme scheme) {
    switch (scheme) {
    case ColorScheme::Spectrum: return "spectrum";
    case ColorScheme::Rainbow: return "rainbow";
    case ColorScheme::Heat: return "heat";
    case ColorScheme::Cool: return "cool";
    case ColorScheme::Monochrome: return "monochrome";
    case ColorScheme::Plasma: return "plasma";
    case ColorScheme::Viridis: return "viridis";
    case ColorScheme::Inferno: return "inferno";
    case ColorScheme::Sunset: return "sunset";
    }
    return "spectrum";
}

std::string outputFormatToString(OutputFormat format) {
    switch (format) {
    case OutputFormat::PNG: return "png";
    case OutputFormat::Video: return "video";
    }
    return "png";
}

std::string toneMapToString(ToneMapOperator op) {
    switch (op) {
    case ToneMapOperator::None: return "none";
    case ToneMapOperator::Reinhard: return "reinhard";
    case ToneMapOperator::ReinhardExtended: return "reinhard_extended";
    case ToneMapOperator::ACES: return "aces";
    case ToneMapOperator::Logarithmic: return "logarithmic";
    }
    return "none";
}

std::string normalizationToString(NormalizationMode mode) {
    switch (mode) {
    case NormalizationMode::PerFrame: return "per_frame";
    case NormalizationMode::ByCount: return "by_count";
    }
    return "per_frame";
}

std::string physicsQualityToString(PhysicsQuality quality) {
    switch (quality) {
    case PhysicsQuality::Low: return "low";
    case PhysicsQuality::Medium: return "medium";
    case PhysicsQuality::High: return "high";
    case PhysicsQuality::Ultra: return "ultra";
    case PhysicsQuality::Custom: return "custom";
    }
    return "high";
}

std::string boomDetectionMethodToString(BoomDetectionMethod method) {
    switch (method) {
    case BoomDetectionMethod::MaxCausticness: return "max_causticness";
    case BoomDetectionMethod::FirstPeakPercent: return "first_peak_percent";
    case BoomDetectionMethod::DerivativePeak: return "derivative_peak";
    case BoomDetectionMethod::ThresholdCrossing: return "threshold_crossing";
    case BoomDetectionMethod::SecondDerivativePeak: return "second_derivative_peak";
    }
    return "max_causticness";
}
} // namespace

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
    file << "physics_quality = \"" << physicsQualityToString(simulation.physics_quality) << "\"\n";
    file << "max_dt = " << simulation.max_dt << "\n";
    file << "\n";

    // Render section
    file << "[render]\n";
    file << "width = " << render.width << "\n";
    file << "height = " << render.height << "\n";
    file << "\n";

    // Post-process section
    file << "[post_process]\n";
    file << "tone_map = \"" << toneMapToString(post_process.tone_map) << "\"\n";
    file << "exposure = " << post_process.exposure << "\n";
    file << "contrast = " << post_process.contrast << "\n";
    file << "gamma = " << post_process.gamma << "\n";
    file << "normalization = \"" << normalizationToString(post_process.normalization) << "\"\n";
    file << "\n";

    // Color section
    file << "[color]\n";
    file << "scheme = \"" << colorSchemeToString(color.scheme) << "\"\n";
    file << "start = " << color.start << "\n";
    file << "end = " << color.end << "\n";
    file << "\n";

    // Metrics section
    file << "[metrics]\n";
    file << "min_sectors = " << metrics.min_sectors << "\n";
    file << "max_sectors = " << metrics.max_sectors << "\n";
    file << "target_per_sector = " << metrics.target_per_sector << "\n";
    file << "min_grid = " << metrics.min_grid << "\n";
    file << "max_grid = " << metrics.max_grid << "\n";
    file << "target_per_cell = " << metrics.target_per_cell << "\n";
    file << "max_radius = " << metrics.max_radius << "\n";
    file << "cv_normalization = " << metrics.cv_normalization << "\n";
    file << "log_ratio_normalization = " << metrics.log_ratio_normalization << "\n";
    file << "min_spread_threshold = " << metrics.min_spread_threshold << "\n";
    file << "gini_chaos_baseline = " << metrics.gini_chaos_baseline << "\n";
    file << "gini_baseline_divisor = " << metrics.gini_baseline_divisor << "\n";
    file << "log_inverse_baseline = " << metrics.log_inverse_baseline << "\n";
    file << "log_inverse_divisor = " << metrics.log_inverse_divisor << "\n";
    file << "\n";

    // Boom detection section
    file << "[boom_detection]\n";
    file << "method = \"" << boomDetectionMethodToString(boom.method) << "\"\n";
    file << "offset_seconds = " << boom.offset_seconds << "\n";
    file << "peak_percent_threshold = " << boom.peak_percent_threshold << "\n";
    file << "min_peak_prominence = " << boom.min_peak_prominence << "\n";
    file << "smoothing_window = " << boom.smoothing_window << "\n";
    file << "crossing_threshold = " << boom.crossing_threshold << "\n";
    file << "crossing_confirmation = " << boom.crossing_confirmation << "\n";
    file << "metric_name = \"" << boom.metric_name << "\"\n";
    file << "\n";

    // Detection section
    // Note: boom_threshold and boom_confirmation are deprecated (boom uses max causticness)
    file << "[detection]\n";
    file << "chaos_threshold = " << detection.chaos_threshold << "\n";
    file << "chaos_confirmation = " << detection.chaos_confirmation << "\n";
    file << "early_stop_after_chaos = " << (detection.early_stop_after_chaos ? "true" : "false")
         << "\n";
    file << "\n";

    // Output section
    file << "[output]\n";
    file << "format = \"" << outputFormatToString(output.format) << "\"\n";
    file << "directory = \"" << output.directory << "\"\n";
    file << "filename_prefix = \"" << output.filename_prefix << "\"\n";
    file << "video_codec = \"" << output.video_codec << "\"\n";
    file << "video_crf = " << output.video_crf << "\n";
    file << "video_fps = " << output.video_fps << "\n";
    file << "save_simulation_data = " << (output.save_simulation_data ? "true" : "false")
         << "\n";
}
