#pragma once

#include <cmath>
#include <string>

// Conversion utilities
inline double deg2rad(double degrees) {
    return degrees * M_PI / 180.0;
}
inline double rad2deg(double radians) {
    return radians * 180.0 / M_PI;
}

struct PhysicsParams {
    double gravity = 9.81;
    double length1 = 1.0;
    double length2 = 1.0;
    double mass1 = 1.0;
    double mass2 = 1.0;
    double initial_angle1 = deg2rad(-32.2); // radians
    double initial_angle2 = deg2rad(-32.0); // radians
    double initial_velocity1 = 0.0;
    double initial_velocity2 = 0.0;
};

// Physics simulation quality presets
// Each maps to a max_dt value that determines simulation accuracy
enum class PhysicsQuality {
    Low,    // max_dt = 0.020 (~100 steps/period, visible artifacts)
    Medium, // max_dt = 0.012 (~167 steps/period, acceptable)
    High,   // max_dt = 0.007 (~286 steps/period, gold standard)
    Ultra,  // max_dt = 0.003 (~667 steps/period, overkill but perfect)
    Custom  // Use explicit max_dt value
};

// Get max_dt value for a quality preset
inline double qualityToMaxDt(PhysicsQuality quality) {
    switch (quality) {
    case PhysicsQuality::Low: return 0.020;
    case PhysicsQuality::Medium: return 0.012;
    case PhysicsQuality::High: return 0.007;
    case PhysicsQuality::Ultra: return 0.003;
    case PhysicsQuality::Custom: return 0.007; // Default to high if somehow used
    }
    return 0.007;
}

struct SimulationParams {
    int pendulum_count = 100000;
    double angle_variation = deg2rad(0.1); // radians
    double duration_seconds = 11.0;        // physical simulation time
    int total_frames = 660;                // number of frames to output

    // Physics quality control (replaces substeps_per_frame)
    // Either use a preset quality level, or specify max_dt directly
    PhysicsQuality physics_quality = PhysicsQuality::High;
    double max_dt = 0.007; // Maximum physics timestep (seconds)

    // Compute substeps needed to achieve max_dt constraint
    int substeps() const {
        double frame_dt = duration_seconds / total_frames;
        return std::max(1, static_cast<int>(std::ceil(frame_dt / max_dt)));
    }

    // Physics timestep: frame_dt / substeps (always <= max_dt)
    double dt() const { return duration_seconds / (total_frames * substeps()); }
};

struct RenderParams {
    int width = 2160;
    int height = 2160;
    int thread_count = 0; // 0 = auto
};

enum class ToneMapOperator { None, Reinhard, ReinhardExtended, ACES, Logarithmic };

struct PostProcessParams {
    // Tone mapping operator for HDR -> SDR conversion
    ToneMapOperator tone_map = ToneMapOperator::None;
    double reinhard_white_point = 1.0; // Only used with ReinhardExtended

    // Standard post-processing parameters:
    // exposure: Brightness in stops (0 = no change, +1 = 2x brighter, -1 = 2x darker)
    // contrast: Centered at 0.5 (1.0 = no change, >1 = more contrast)
    // gamma: Display gamma (2.2 for sRGB, 1.0 for linear)
    double exposure = 0.0;
    double contrast = 1.0;
    double gamma = 2.2;
};

enum class ColorScheme { Spectrum, Rainbow, Heat, Cool, Monochrome };

struct ColorParams {
    ColorScheme scheme = ColorScheme::Spectrum;
    double start = 0.0; // Range start [0, 1]
    double end = 1.0;   // Range end [0, 1]
};

// Thresholds for detecting events from variance data
struct DetectionParams {
    // "Boom" threshold: when variance exceeds this, chaos has begun
    double boom_threshold = 0.1; // radians^2
    int boom_confirmation = 10;  // Consecutive frames above threshold

    // "White" threshold: when variance exceeds this, full chaos/noise
    double white_threshold = 700.0; // radians^2
    int white_confirmation = 10;    // Consecutive frames above threshold

    // Early stopping
    bool early_stop_after_white = false;
};

enum class OutputFormat { PNG, Video };

struct OutputParams {
    OutputFormat format = OutputFormat::PNG;
    std::string directory = "output";
    std::string filename_prefix = "frame";
    std::string video_codec = "libx264";
    int video_crf = 23;
    int video_fps = 60; // Only affects video encoding, not simulation
};

struct Config {
    PhysicsParams physics;
    SimulationParams simulation;
    RenderParams render;
    PostProcessParams post_process;
    ColorParams color;
    DetectionParams detection;
    OutputParams output;

    // Load from TOML file
    static Config load(std::string const& path);

    // Load with defaults
    static Config defaults();
};
