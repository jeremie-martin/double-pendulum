#include "batch_generator.h"
#include "music_manager.h"
#include "simulation.h"

#include <filesystem>
#include <iostream>
#include <string>

void printUsage(char const* program) {
    std::cout << "Double Pendulum Simulation (GPU)\n\n"
              << "Usage:\n"
              << "  " << program << " [config.toml]           Run simulation\n"
              << "  " << program << " [config.toml] --music <track-id|random>\n"
              << "                                      Run simulation with music\n"
              << "  " << program << " --batch <batch.toml>    Run batch generation\n"
              << "  " << program << " --batch <batch.toml> --resume\n"
              << "                                      Resume batch generation\n"
              << "  " << program << " --add-music <video> <track-id> <boom-frame> <fps>\n"
              << "                                      Add music to existing video\n"
              << "  " << program << " --list-tracks           List available music tracks\n"
              << "  " << program << " -h, --help              Show this help\n\n"
              << "Examples:\n"
              << "  " << program << " config/default.toml\n"
              << "  " << program << " config/default.toml --music random\n"
              << "  " << program << " config/default.toml --music petrunko\n"
              << "  " << program << " --batch config/batch.toml\n"
              << "  " << program << " --batch config/batch.toml --resume\n"
              << "  " << program << " --add-music output/run_xxx/video.mp4 petrunko 32 60\n"
              << "  " << program << " --list-tracks\n";
}

int runSimulation(std::string const& config_path, std::string const& music_track = "") {
    // Load configuration
    std::cout << "Loading config from: " << config_path << "\n";
    Config config = Config::load(config_path);

    // Calculate video duration
    double video_duration =
        static_cast<double>(config.simulation.total_frames) / config.output.video_fps;

    // Print config summary
    std::cout << "\nSimulation parameters:\n"
              << "  Pendulums: " << config.simulation.pendulum_count << "\n"
              << "  Physics duration: " << config.simulation.duration_seconds << "s\n"
              << "  Frames: " << config.simulation.total_frames << "\n"
              << "  Video: " << video_duration << "s @ " << config.output.video_fps << " FPS\n"
              << "  Resolution: " << config.render.width << "x" << config.render.height << "\n"
              << "  Output: " << config.output.directory << "/\n"
              << "  Renderer: GPU\n";

    if (!music_track.empty()) {
        std::cout << "  Music: " << music_track << "\n";
    }
    std::cout << "\n";

    // Run simulation
    Simulation sim(config);
    auto results = sim.run();

    if (results.frames_completed == 0) {
        std::cerr << "Simulation failed\n";
        return 1;
    }

    // Add music if requested and we have a video with boom_frame
    if (!music_track.empty() && !results.video_path.empty() && results.boom_frame) {
        MusicManager music;
        if (!music.load("music")) {
            std::cerr << "Failed to load music database\n";
            return 1;
        }

        std::optional<MusicTrack> track;
        if (music_track == "random") {
            if (music.trackCount() > 0) {
                track = music.randomTrack();
            }
        } else {
            track = music.getTrack(music_track);
        }

        if (!track) {
            std::cerr << "Track not found: " << music_track << "\n";
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

int runBatch(std::string const& batch_config_path, bool resume) {
    std::cout << "Loading batch config from: " << batch_config_path << "\n";
    BatchConfig config = BatchConfig::load(batch_config_path);

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
        return runSimulation("config/default.toml");
    }

    std::string arg = argv[1];

    if (arg == "-h" || arg == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    if (arg == "--list-tracks") {
        return listTracks();
    }

    if (arg == "--batch") {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " --batch <batch.toml> [--resume]\n";
            return 1;
        }
        std::string batch_config = argv[2];
        bool resume = (argc >= 4 && std::string(argv[3]) == "--resume");
        return runBatch(batch_config, resume);
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

    // Otherwise, treat argument as config path
    // Check for --music option
    std::string music_track;

    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--music" && i + 1 < argc) {
            music_track = argv[i + 1];
        }
    }

    return runSimulation(arg, music_track);
}
