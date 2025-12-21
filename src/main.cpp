#include "simulation.h"
#include <iostream>
#include <string>

void printUsage(char const* program) {
    std::cout << "Double Pendulum Simulation\n\n"
              << "Usage: " << program << " [config.toml]\n\n"
              << "If no config file is specified, uses config/default.toml\n"
              << "or built-in defaults if that doesn't exist.\n";
}

int main(int argc, char* argv[]) {
    // Parse command line
    std::string config_path = "config/default.toml";

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        config_path = arg;
    }

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
