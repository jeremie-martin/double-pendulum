#include "simulation.h"
#include "music_manager.h"
#include <iostream>
#include <string>
#include <cstring>

void printUsage(char const* program) {
    std::cout << "Double Pendulum Simulation\n\n"
              << "Usage:\n"
              << "  " << program << " [config.toml]           Run simulation\n"
              << "  " << program << " --add-music <video> <track-id> <boom-frame> <fps>\n"
              << "                                      Add music to existing video\n"
              << "  " << program << " --list-tracks           List available music tracks\n"
              << "  " << program << " -h, --help              Show this help\n\n"
              << "Examples:\n"
              << "  " << program << " config/default.toml\n"
              << "  " << program << " --add-music output/run_xxx/video.mp4 petrunko 32 60\n"
              << "  " << program << " --list-tracks\n";
}

int runSimulation(std::string const& config_path) {
    // Load configuration
    std::cout << "Loading config from: " << config_path << "\n";
    Config config = Config::load(config_path);

    // Print config summary
    std::cout << "\nSimulation parameters:\n"
              << "  Pendulums: " << config.simulation.pendulum_count << "\n"
              << "  Duration: " << config.simulation.duration_seconds << "s\n"
              << "  Frames: " << config.simulation.total_frames << "\n"
              << "  Video FPS: " << config.output.video_fps << "\n"
              << "  Resolution: " << config.render.width << "x" << config.render.height << "\n"
              << "  Output: " << config.output.directory << "/\n\n";

    // Run simulation
    Simulation sim(config);
    sim.run();

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
                  << "    Drop: " << track.drop_time_ms << "ms ("
                  << track.dropTimeSeconds() << "s)\n\n";
    }

    return 0;
}

int addMusic(std::string const& video_path, std::string const& track_id,
             int boom_frame, int fps) {
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
    std::filesystem::path output = video.parent_path() /
        (video.stem().string() + "_with_music" + video.extension().string());

    bool success = MusicManager::muxWithAudio(
        video_path,
        track->filepath,
        output,
        boom_frame,
        track->drop_time_ms,
        fps
    );

    return success ? 0 : 1;
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
    return runSimulation(arg);
}
