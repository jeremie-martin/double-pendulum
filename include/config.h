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

    // Duration of one frame in seconds
    double frameDuration() const { return duration_seconds / total_frames; }
};

struct RenderParams {
    int width = 2160;
    int height = 2160;
    int thread_count = 0; // 0 = auto
};

enum class ToneMapOperator { None, Reinhard, ReinhardExtended, ACES, Logarithmic };

// Normalization mode for HDR rendering
// Controls how accumulated pixel values are normalized before exposure/tonemapping
enum class NormalizationMode {
    PerFrame,  // Normalize to per-frame max (default, auto-adjusts brightness)
    ByCount    // Normalize by pendulum count (consistent across different counts)
};

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

    // Normalization mode
    // PerFrame: each frame normalized to its own max (brightness auto-adjusts)
    // ByCount: divide by pendulum count (consistent results regardless of count)
    NormalizationMode normalization = NormalizationMode::PerFrame;
};

enum class ColorScheme { Spectrum, Rainbow, Heat, Cool, Monochrome, Plasma, Viridis, Inferno, Sunset };

struct ColorParams {
    ColorScheme scheme = ColorScheme::Spectrum;
    double start = 0.0; // Range start [0, 1]
    double end = 1.0;   // Range end [0, 1]
};

// Metric computation parameters - controls how causticness metrics are calculated
// All defaults match previous hardcoded values for backward compatibility
struct MetricParams {
    // Sector-based algorithm parameters (for angular causticness, tip causticness, etc.)
    // Sectors scale with N to maintain N-independence: num_sectors = clamp(N/target, min, max)
    int min_sectors = 8;
    int max_sectors = 72;
    int target_per_sector = 40;

    // Grid-based spatial metrics (for spatial_concentration)
    int min_grid = 4;
    int max_grid = 32;
    int target_per_cell = 40;

    // Normalization thresholds
    double max_radius = 2.0;               // Maximum tip radius (L1 + L2 for unit lengths)
    double cv_normalization = 1.5;         // CV divisor for 0-1 normalization
    double log_ratio_normalization = 2.0;  // Log ratio divisor for P90/P10 metric
    double min_spread_threshold = 0.05;    // Minimum spread to compute coherence metrics

    // Gini baseline adjustment (subtracts chaos noise floor from distance-based Gini)
    double gini_chaos_baseline = 0.35;
    double gini_baseline_divisor = 0.65;

    // Local coherence parameters (for min/median ratio metric)
    double log_inverse_baseline = 1.0;
    double log_inverse_divisor = 2.5;
};

// Boom detection method
enum class BoomDetectionMethod {
    MaxCausticness,       // Find frame with max causticness (peak visual richness)
    FirstPeakPercent,     // Find first peak >= X% of max (boom onset)
    DerivativePeak,       // When d(causticness)/dt is maximum (steepest transition)
    ThresholdCrossing,    // First frame where metric crosses threshold (fraction of max)
    SecondDerivativePeak  // When d²(causticness)/dt² is maximum (acceleration peak)
};

// Boom detection parameters - controls how boom frame is identified
struct BoomDetectionParams {
    BoomDetectionMethod method = BoomDetectionMethod::MaxCausticness;

    // For MaxCausticness: offset from peak (visual alignment)
    double offset_seconds = 0.3;

    // For FirstPeakPercent: threshold as fraction of max peak
    double peak_percent_threshold = 0.6;

    // For all peak detection: minimum prominence to count as peak
    double min_peak_prominence = 0.05;

    // For DerivativePeak and SecondDerivativePeak: smoothing window size (frames)
    int smoothing_window = 5;

    // For ThresholdCrossing: threshold as fraction of max value
    double crossing_threshold = 0.3;

    // For ThresholdCrossing: require this many consecutive frames above threshold
    int crossing_confirmation = 3;

    // Which metric to use for detection
    std::string metric_name = "angular_causticness";
};

// Thresholds for detecting events from variance data
struct DetectionParams {
    // DEPRECATED: boom_threshold and boom_confirmation are no longer used.
    // Boom is now detected via max angular causticness in boom_detection.h
    // Kept for config file backwards compatibility.
    double boom_threshold = 0.1; // radians^2 (unused)
    int boom_confirmation = 10;  // (unused)

    // "Chaos" threshold: when variance exceeds this, full chaos/noise
    double chaos_threshold = 700.0; // radians^2
    int chaos_confirmation = 10;    // Consecutive frames above threshold

    // Early stopping
    bool early_stop_after_chaos = false;
};

enum class OutputFormat { PNG, Video };

// Output directory mode (internal use only - not configurable via TOML)
// This is set programmatically by batch mode; single runs always use Timestamped
enum class OutputMode {
    Timestamped,  // Create run_YYYYMMDD_HHMMSS subdirectory (default for single runs)
    Direct        // Write directly to output.directory (used by batch mode)
};

struct OutputParams {
    OutputFormat format = OutputFormat::PNG;
    std::string directory = "output";
    std::string filename_prefix = "frame";
    std::string video_codec = "libx264";
    int video_crf = 23;
    int video_fps = 60; // Only affects video encoding, not simulation
    OutputMode mode = OutputMode::Timestamped;  // Internal: set by batch mode, not TOML

    // Save raw simulation data for metric iteration
    // When enabled, saves simulation_data.bin alongside video/frames
    bool save_simulation_data = false;
};

// Analysis mode parameters for extended statistics
struct AnalysisParams {
    bool enabled = false;
};

struct Config {
    PhysicsParams physics;
    SimulationParams simulation;
    RenderParams render;
    PostProcessParams post_process;
    ColorParams color;
    MetricParams metrics;
    BoomDetectionParams boom;
    DetectionParams detection;
    OutputParams output;
    AnalysisParams analysis;

    // Load from TOML file
    static Config load(std::string const& path);

    // Load with defaults
    static Config defaults();

    // Save to TOML file (for batch mode - saves actual resolved parameters)
    void save(std::string const& path) const;

    // Apply a parameter override from CLI (e.g., "simulation.pendulum_count", "100000")
    // Returns true if the key was recognized and applied
    bool applyOverride(std::string const& key, std::string const& value);
};
