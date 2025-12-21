#pragma once

#include <cmath>
#include <string>

// Conversion utilities
inline double deg2rad(double degrees) { return degrees * M_PI / 180.0; }
inline double rad2deg(double radians) { return radians * 180.0 / M_PI; }

struct PhysicsParams {
    double gravity = 9.81;
    double length1 = 1.0;
    double length2 = 1.0;
    double mass1 = 1.0;
    double mass2 = 1.0;
    double initial_angle1 = deg2rad(-32.2);  // radians
    double initial_angle2 = deg2rad(-32.0);  // radians
    double initial_velocity1 = 0.0;
    double initial_velocity2 = 0.0;
};

struct SimulationParams {
    int pendulum_count = 100000;
    double angle_variation = deg2rad(0.1);  // radians
    double duration = 11.0;                  // seconds
    int fps = 60;
    int substeps_per_frame = 20;

    double dt() const { return duration / (fps * substeps_per_frame); }
    int total_frames() const { return static_cast<int>(duration * fps); }
};

struct RenderParams {
    int width = 2160;
    int height = 2160;
    int thread_count = 0;  // 0 = auto
};

struct PostProcessParams {
    double gamma = 1.05;
    double target_brightness = 0.5;
    double contrast = 1.0;
    bool auto_normalize = true;
};

enum class ColorScheme {
    Spectrum,
    Rainbow,
    Heat,
    Cool,
    Monochrome
};

struct ColorParams {
    ColorScheme scheme = ColorScheme::Spectrum;
    double wavelength_start = 380.0;
    double wavelength_end = 780.0;
};

struct BoomDetectionParams {
    bool enabled = false;
    double variance_threshold = 0.1;
    int confirmation_frames = 10;
    bool early_stop = false;
};

enum class OutputFormat {
    PNG,
    Video
};

struct OutputParams {
    OutputFormat format = OutputFormat::PNG;
    std::string directory = "output";
    std::string filename_prefix = "frame";
    std::string video_codec = "libx264";
    int video_crf = 23;
};

struct Config {
    PhysicsParams physics;
    SimulationParams simulation;
    RenderParams render;
    PostProcessParams post_process;
    ColorParams color;
    BoomDetectionParams boom_detection;
    OutputParams output;

    // Load from TOML file
    static Config load(std::string const& path);

    // Load with defaults
    static Config defaults();
};
