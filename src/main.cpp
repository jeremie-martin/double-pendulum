#include "batch_generator.h"
#include "enum_strings.h"
#include "simulation.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <toml.hpp>
#include <utility>
#include <vector>

// Check if a config file is a batch config (has [batch] section)
bool isBatchConfig(std::string const& path) {
    try {
        auto tbl = toml::parse_file(path);
        return tbl["batch"].is_table();
    } catch (...) {
        return false;
    }
}

void printUsage(char const* program) {
    std::cout << "Double Pendulum Simulation (GPU)\n\n"
              << "Usage:\n"
              << "  " << program << " [config.toml] [options]  Run simulation or batch\n"
              << "  " << program << " -h, --help              Show this help\n\n"
              << "Config auto-detection:\n"
              << "  If config contains [batch] section, runs batch generation\n"
              << "  Otherwise, runs single simulation\n\n"
              << "Options:\n"
              << "  --set <key>=<value>    Override config parameter (can be used multiple times)\n"
              << "  --analysis             Enable analysis mode (extended statistics)\n"
              << "  --save-data            Save raw simulation data for metric iteration\n"
              << "  --resume               Resume interrupted batch\n\n"
              << "Parameter keys use dot notation: section.parameter\n"
              << "  Sections: physics, simulation, render, post_process, color, detection, output\n\n"
              << "Examples:\n"
              << "  " << program << " config/default.toml\n"
              << "  " << program << " config/default.toml --set simulation.pendulum_count=50000\n"
              << "  " << program << " config/default.toml --set post_process.exposure=2.0 --analysis\n"
              << "  " << program << " config/batch.toml\n"
              << "  " << program << " config/batch.toml --resume\n"
              << "  " << program << " config/batch.toml --set render.width=1920\n";
}

// Parsed command-line options
struct CLIOptions {
    std::string config_path = "config/default.toml";
    std::vector<std::pair<std::string, std::string>> overrides;
    bool analysis = false;
    bool save_data = false;  // Save raw simulation data for metric iteration
};

int runSimulation(CLIOptions const& opts) {
    // Load configuration
    std::cout << "Loading config from: " << opts.config_path << "\n";
    Config config = Config::load(opts.config_path);

    // Apply command-line overrides
    for (auto const& [key, value] : opts.overrides) {
        if (!config.applyOverride(key, value)) {
            return 1;
        }
        std::cout << "Override: " << key << " = " << value << "\n";
    }

    // Apply analysis flag
    if (opts.analysis) {
        config.analysis.enabled = true;
        std::cout << "Analysis mode: enabled\n";
    }

    // Apply save-data flag
    if (opts.save_data) {
        config.output.save_simulation_data = true;
        std::cout << "Saving simulation data: enabled\n";
    }

    // Calculate derived values
    double video_duration =
        static_cast<double>(config.simulation.total_frames) / config.output.video_fps;
    int total_physics_steps = config.simulation.total_frames * config.simulation.substeps();

    // Print comprehensive config summary
    std::cout << "\n=== Double Pendulum Simulation ===\n\n";

    std::cout << "Physics:\n"
              << "  Gravity:        " << config.physics.gravity << " m/s^2\n"
              << "  Lengths:        L1=" << config.physics.length1 << "m, L2=" << config.physics.length2 << "m\n"
              << "  Masses:         M1=" << config.physics.mass1 << "kg, M2=" << config.physics.mass2 << "kg\n"
              << "  Initial angles: th1=" << rad2deg(config.physics.initial_angle1) << " deg, "
              << "th2=" << rad2deg(config.physics.initial_angle2) << " deg\n"
              << "  Angle spread:   +/- " << rad2deg(config.simulation.angle_variation) << " deg\n\n";

    std::cout << "Simulation:\n"
              << "  Pendulums:      " << config.simulation.pendulum_count << "\n"
              << "  Duration:       " << config.simulation.duration_seconds << "s physics -> "
              << config.simulation.total_frames << " frames @ " << config.output.video_fps << " FPS ("
              << std::fixed << std::setprecision(1) << video_duration << "s video)\n"
              << "  Quality:        " << toString(config.simulation.physics_quality)
              << " (max_dt=" << std::setprecision(1) << (config.simulation.max_dt * 1000) << "ms)\n"
              << "  Substeps:       " << config.simulation.substeps() << " per frame"
              << " (dt=" << std::setprecision(2) << (config.simulation.dt() * 1000) << "ms)\n"
              << "  Total steps:    " << total_physics_steps << "\n\n";

    std::cout << "Rendering:\n"
              << "  Resolution:     " << config.render.width << "x" << config.render.height << "\n"
              << "  Color scheme:   " << toString(config.color.scheme) << "\n"
              << "  Tone mapping:   " << toString(config.post_process.tone_map) << "\n"
              << "  Normalization:  " << toString(config.post_process.normalization) << "\n"
              << "  Exposure:       " << std::showpos << std::setprecision(1) << config.post_process.exposure
              << std::noshowpos << " stops\n"
              << "  Gamma:          " << config.post_process.gamma << "\n\n";

    std::cout << "Output:\n"
              << "  Directory:      " << config.output.directory << "/\n"
              << "  Format:         " << (config.output.format == OutputFormat::Video ? "video" : "png") << "\n";
    if (config.analysis.enabled) {
        std::cout << "  Analysis:       enabled\n";
    }
    std::cout << "\n";

    // Run simulation
    Simulation sim(config);
    auto results = sim.run(nullptr, opts.config_path);

    if (results.frames_completed == 0) {
        std::cerr << "Simulation failed\n";
        return 1;
    }

    return 0;
}

// Parse --set key=value argument
std::optional<std::pair<std::string, std::string>> parseSetArg(std::string const& arg) {
    auto eq_pos = arg.find('=');
    if (eq_pos == std::string::npos) {
        std::cerr << "Invalid --set argument (missing '='): " << arg << "\n";
        return std::nullopt;
    }
    return std::make_pair(arg.substr(0, eq_pos), arg.substr(eq_pos + 1));
}

int runBatch(std::string const& batch_config_path, bool resume,
             std::vector<std::pair<std::string, std::string>> const& overrides) {
    std::cout << "Loading batch config from: " << batch_config_path << "\n";
    BatchConfig config = BatchConfig::load(batch_config_path);

    // Apply overrides to base config
    for (auto const& [key, value] : overrides) {
        if (!config.base_config.applyOverride(key, value)) {
            return 1;
        }
        std::cout << "Override: " << key << " = " << value << "\n";
    }

    BatchGenerator generator(config);

    if (resume) {
        generator.resume();
    } else {
        generator.run();
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        // Default: run simulation with default config
        CLIOptions opts;
        return runSimulation(opts);
    }

    std::string arg = argv[1];

    if (arg == "-h" || arg == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    // Treat argument as config path
    std::string config_path = arg;
    bool resume = false;
    std::vector<std::pair<std::string, std::string>> overrides;

    // Parse remaining options
    CLIOptions opts;
    opts.config_path = config_path;

    for (int i = 2; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "--resume") {
            resume = true;
        } else if (opt == "--set" && i + 1 < argc) {
            auto parsed = parseSetArg(argv[++i]);
            if (!parsed) return 1;
            opts.overrides.push_back(*parsed);
            overrides.push_back(*parsed);
        } else if (opt == "--analysis") {
            opts.analysis = true;
        } else if (opt == "--save-data") {
            opts.save_data = true;
        }
    }

    // Auto-detect: if config has [batch] section, run batch mode
    if (isBatchConfig(config_path)) {
        return runBatch(config_path, resume, overrides);
    }

    return runSimulation(opts);
}
