#include "batch_generator.h"
#include "music_manager.h"
#include "simulation.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

void printUsage(char const* program) {
    std::cout << "Double Pendulum Simulation (GPU)\n\n"
              << "Usage:\n"
              << "  " << program << " [config.toml] [options]  Run simulation\n"
              << "  " << program << " --batch <batch.toml> [options]\n"
              << "                                      Run batch generation\n"
              << "  " << program << " --add-music <video> <track-id> <boom-frame> <fps>\n"
              << "                                      Add music to existing video\n"
              << "  " << program << " --list-tracks           List available music tracks\n"
              << "  " << program << " -h, --help              Show this help\n\n"
              << "Options:\n"
              << "  --set <key>=<value>    Override config parameter (can be used multiple times)\n"
              << "  --analysis             Enable analysis mode (extended statistics)\n"
              << "  --music <track|random> Add music synced to chaos onset\n"
              << "  --resume               Resume interrupted batch (with --batch)\n\n"
              << "Parameter keys use dot notation: section.parameter\n"
              << "  Sections: physics, simulation, render, post_process, color, detection, output\n\n"
              << "Examples:\n"
              << "  " << program << " config/default.toml\n"
              << "  " << program << " config/default.toml --set simulation.pendulum_count=50000\n"
              << "  " << program << " config/default.toml --set post_process.exposure=2.0 --analysis\n"
              << "  " << program << " config/default.toml --music random\n"
              << "  " << program << " --batch config/batch.toml\n"
              << "  " << program << " --batch config/batch.toml --resume\n"
              << "  " << program << " --batch config/grid.toml --set render.width=1920\n"
              << "  " << program << " --add-music output/run_xxx/video.mp4 petrunko 32 60\n";
}

// Parsed command-line options
struct CLIOptions {
    std::string config_path = "config/default.toml";
    std::string music_track;
    std::vector<std::pair<std::string, std::string>> overrides;
    bool analysis = false;
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

    // Calculate derived values
    double video_duration =
        static_cast<double>(config.simulation.total_frames) / config.output.video_fps;
    int total_physics_steps = config.simulation.total_frames * config.simulation.substeps();

    // Get physics quality string
    auto qualityName = [](PhysicsQuality q) -> char const* {
        switch (q) {
        case PhysicsQuality::Low: return "low";
        case PhysicsQuality::Medium: return "medium";
        case PhysicsQuality::High: return "high";
        case PhysicsQuality::Ultra: return "ultra";
        case PhysicsQuality::Custom: return "custom";
        }
        return "unknown";
    };

    auto toneMapName = [](ToneMapOperator tm) -> char const* {
        switch (tm) {
        case ToneMapOperator::None: return "none";
        case ToneMapOperator::Reinhard: return "reinhard";
        case ToneMapOperator::ReinhardExtended: return "reinhard_extended";
        case ToneMapOperator::ACES: return "aces";
        case ToneMapOperator::Logarithmic: return "logarithmic";
        }
        return "unknown";
    };

    auto colorSchemeName = [](ColorScheme cs) -> char const* {
        switch (cs) {
        case ColorScheme::Spectrum: return "spectrum";
        case ColorScheme::Rainbow: return "rainbow";
        case ColorScheme::Heat: return "heat";
        case ColorScheme::Cool: return "cool";
        case ColorScheme::Monochrome: return "monochrome";
        }
        return "unknown";
    };

    auto normModeName = [](NormalizationMode nm) -> char const* {
        switch (nm) {
        case NormalizationMode::PerFrame: return "per_frame";
        case NormalizationMode::ByCount: return "by_count";
        }
        return "unknown";
    };

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
              << "  Quality:        " << qualityName(config.simulation.physics_quality)
              << " (max_dt=" << std::setprecision(1) << (config.simulation.max_dt * 1000) << "ms)\n"
              << "  Substeps:       " << config.simulation.substeps() << " per frame"
              << " (dt=" << std::setprecision(2) << (config.simulation.dt() * 1000) << "ms)\n"
              << "  Total steps:    " << total_physics_steps << "\n\n";

    std::cout << "Rendering:\n"
              << "  Resolution:     " << config.render.width << "x" << config.render.height << "\n"
              << "  Color scheme:   " << colorSchemeName(config.color.scheme) << "\n"
              << "  Tone mapping:   " << toneMapName(config.post_process.tone_map) << "\n"
              << "  Normalization:  " << normModeName(config.post_process.normalization) << "\n"
              << "  Exposure:       " << std::showpos << std::setprecision(1) << config.post_process.exposure
              << std::noshowpos << " stops\n"
              << "  Gamma:          " << config.post_process.gamma << "\n\n";

    std::cout << "Output:\n"
              << "  Directory:      " << config.output.directory << "/\n"
              << "  Format:         " << (config.output.format == OutputFormat::Video ? "video" : "png") << "\n";
    if (config.analysis.enabled) {
        std::cout << "  Analysis:       enabled\n";
    }
    if (!opts.music_track.empty()) {
        std::cout << "  Music:          " << opts.music_track << "\n";
    }
    std::cout << "\n";

    // Run simulation
    Simulation sim(config);
    auto results = sim.run(nullptr, opts.config_path);

    if (results.frames_completed == 0) {
        std::cerr << "Simulation failed\n";
        return 1;
    }

    // Add music if requested and we have a video with boom_frame
    if (!opts.music_track.empty() && !results.video_path.empty() && results.boom_frame) {
        MusicManager music;
        if (!music.load("music")) {
            std::cerr << "Failed to load music database\n";
            return 1;
        }

        std::optional<MusicTrack> track;
        if (opts.music_track == "random") {
            if (music.trackCount() > 0) {
                track = music.randomTrack();
            }
        } else {
            track = music.getTrack(opts.music_track);
        }

        if (!track) {
            std::cerr << "Track not found: " << opts.music_track << "\n";
            std::cerr << "Use --list-tracks to see available tracks\n";
            return 1;
        }

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
            std::cout << "Music added successfully!\n";
        } else {
            std::cerr << "Failed to add music\n";
            return 1;
        }
    }

    return 0;
}

int listTracks() {
    MusicManager music;
    if (!music.load("music")) {
        std::cerr << "Failed to load music database\n";
        return 1;
    }

    std::cout << "\nAvailable tracks:\n";
    for (auto const& track : music.tracks()) {
        std::cout << "  " << track.id << "\n"
                  << "    Title: " << track.title << "\n"
                  << "    File: " << track.filepath << "\n"
                  << "    Drop: " << track.drop_time_ms << "ms (" << track.dropTimeSeconds()
                  << "s)\n\n";
    }

    return 0;
}

int addMusic(std::string const& video_path, std::string const& track_id, int boom_frame, int fps) {
    MusicManager music;
    if (!music.load("music")) {
        std::cerr << "Failed to load music database\n";
        return 1;
    }

    auto track = music.getTrack(track_id);
    if (!track) {
        std::cerr << "Track not found: " << track_id << "\n";
        std::cerr << "Use --list-tracks to see available tracks\n";
        return 1;
    }

    // Generate output path
    std::filesystem::path video(video_path);
    std::filesystem::path output =
        video.parent_path() / (video.stem().string() + "_with_music" + video.extension().string());

    bool success = MusicManager::muxWithAudio(video_path, track->filepath, output, boom_frame,
                                              track->drop_time_ms, fps);

    return success ? 0 : 1;
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

    if (arg == "--list-tracks") {
        return listTracks();
    }

    if (arg == "--add-music") {
        if (argc < 6) {
            std::cerr << "Usage: " << argv[0]
                      << " --add-music <video> <track-id> <boom-frame> <fps>\n";
            return 1;
        }
        std::string video = argv[2];
        std::string track_id = argv[3];
        int boom_frame = std::stoi(argv[4]);
        int fps = std::stoi(argv[5]);
        return addMusic(video, track_id, boom_frame, fps);
    }

    if (arg == "--batch") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " --batch <batch.toml> [options]\n";
            return 1;
        }
        std::string batch_config = argv[2];
        bool resume = false;
        std::vector<std::pair<std::string, std::string>> overrides;

        for (int i = 3; i < argc; ++i) {
            std::string opt = argv[i];
            if (opt == "--resume") {
                resume = true;
            } else if (opt == "--set" && i + 1 < argc) {
                auto parsed = parseSetArg(argv[++i]);
                if (!parsed) return 1;
                overrides.push_back(*parsed);
            }
        }
        return runBatch(batch_config, resume, overrides);
    }

    // Otherwise, treat argument as config path
    CLIOptions opts;
    opts.config_path = arg;

    // Parse remaining options
    for (int i = 2; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "--music" && i + 1 < argc) {
            opts.music_track = argv[++i];
        } else if (opt == "--set" && i + 1 < argc) {
            auto parsed = parseSetArg(argv[++i]);
            if (!parsed) return 1;
            opts.overrides.push_back(*parsed);
        } else if (opt == "--analysis") {
            opts.analysis = true;
        }
    }

    return runSimulation(opts);
}
