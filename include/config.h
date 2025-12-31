#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

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
    case PhysicsQuality::Low:
        return 0.020;
    case PhysicsQuality::Medium:
        return 0.012;
    case PhysicsQuality::High:
        return 0.007;
    case PhysicsQuality::Ultra:
        return 0.003;
    case PhysicsQuality::Custom:
        return 0.007; // Default to high if somehow used
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
    PerFrame, // Normalize to per-frame max (default, auto-adjusts brightness)
    ByCount   // Normalize by pendulum count (consistent across different counts)
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

enum class ColorScheme {
    // Original schemes
    Spectrum,
    Rainbow,
    Heat,
    Cool,
    Monochrome,
    Plasma,
    Viridis,
    Inferno,
    Sunset,

    // New gradient-based schemes
    Ember,      // Glowing coals: deep red -> orange -> golden -> soft white
    DeepOcean,  // Underwater: inky black -> teal -> cyan -> ice
    NeonViolet, // Moody: dark purple -> magenta -> pink glow
    Aurora,     // Northern lights: blue -> teal -> green -> warm spark
    Pearl,      // Elegant: espresso -> cream -> lilac sheen
    TurboPop,   // High-energy rainbow with dark lows
    Nebula,     // Deep space: purple -> magenta -> cyan wisps
    Blackbody,  // Heated object: dark -> red -> orange -> white -> blue tint
    Magma,      // Matplotlib Magma: black -> purple -> red -> yellow
    Cyberpunk,  // Neon: hot pink -> purple -> electric blue -> acid green
    Biolume,    // Deep sea: dark navy -> teal -> electric lime
    Gold,       // Ethereal: chocolate -> bronze -> gold -> white
    RoseGold,   // Metallics: deep rose -> rose gold -> champagne
    Twilight,   // Sunset to night: orange -> pink -> purple -> deep blue
    ForestFire, // Igniting: dark forest -> amber -> flame -> yellow

    // Curve-based schemes with unique character
    AbyssalGlow,    // Bioluminescent cyan-green from deep black
    MoltenCore,     // Volcanic incandescent with controlled peaks
    Iridescent,     // Thin-film interference shifting hues
    StellarNursery, // Cosmic emission nebula colors
    WhiskeyAmber    // Warm organic: mahogany -> amber -> honey -> cream
};

struct ColorParams {
    ColorScheme scheme = ColorScheme::Spectrum;
    double start = 0.0; // Range start [0, 1]
    double end = 1.0;   // Range end [0, 1]
};

// ============================================================================
// PER-METRIC PARAMETER STRUCTS
// Each metric type has its own parameter struct containing only relevant params.
// Frame detection settings are configured via [targets.X] sections, not here.
// ============================================================================

// For sector-based metrics: angular_causticness, tip_causticness,
// organization_causticness, r1_concentration, r2_concentration, joint_concentration
struct SectorMetricParams {
    int min_sectors = 8;
    int max_sectors = 72;
    int target_per_sector = 40;
};

// For CV-based sector metrics: cv_causticness
struct CVSectorMetricParams {
    int min_sectors = 8;
    int max_sectors = 72;
    int target_per_sector = 40;
    double cv_normalization = 1.5;
};

// For local_coherence metric
struct LocalCoherenceMetricParams {
    double max_radius = 2.0;
    double min_spread_threshold = 0.05;
    double log_inverse_baseline = 1.0;
    double log_inverse_divisor = 2.5;
};

// Variant type for unified per-metric parameter storage
using MetricParamsVariant =
    std::variant<SectorMetricParams, CVSectorMetricParams, LocalCoherenceMetricParams>;

// Configuration for a single metric (name + computation params)
struct MetricConfig {
    std::string name;
    MetricParamsVariant params;
};

// Metric type enumeration for type dispatch
enum class MetricType {
    Sector,         // angular_causticness, tip_causticness, etc.
    CVSector,       // cv_causticness
    LocalCoherence, // local_coherence
    None            // variance, spread_ratio, etc. (no configurable params)
};

// Get the metric type for a given metric name
inline MetricType getMetricType(std::string const& name) {
    if (name == "angular_causticness" || name == "tip_causticness" ||
        name == "organization_causticness" || name == "r1_concentration" ||
        name == "r2_concentration" || name == "joint_concentration") {
        return MetricType::Sector;
    } else if (name == "cv_causticness") {
        return MetricType::CVSector;
    } else if (name == "local_coherence") {
        return MetricType::LocalCoherence;
    }
    return MetricType::None;
}

// Create default MetricConfig for a given metric name
inline MetricConfig createDefaultMetricConfig(std::string const& name) {
    MetricConfig config;
    config.name = name;
    config.params = SectorMetricParams{}; // Default

    switch (getMetricType(name)) {
    case MetricType::Sector:
        config.params = SectorMetricParams{};
        break;
    case MetricType::CVSector:
        config.params = CVSectorMetricParams{};
        break;
    case MetricType::LocalCoherence:
        config.params = LocalCoherenceMetricParams{};
        break;
    case MetricType::None:
        // Use SectorMetricParams as a placeholder for unparameterized metrics
        config.params = SectorMetricParams{};
        break;
    }

    return config;
}

// ============================================================================
// TARGET CONFIGURATION (for multi-target prediction system)
// ============================================================================

// Configuration for a single prediction target
// Supports both frame predictions (boom, chaos) and score predictions (boom_quality)
struct TargetConfig {
    std::string name;           // e.g., "boom", "chaos", "boom_quality"
    std::string type = "frame"; // "frame" or "score"
    std::string metric;         // Metric to use for detection
    std::string method;         // Detection/scoring method

    // Frame detection parameters
    double offset_seconds = 0.0;
    double peak_percent_threshold = 0.6;
    double min_peak_prominence = 0.05;
    int smoothing_window = 5;
    double crossing_threshold = 0.3;
    int crossing_confirmation = 3;

    // Score prediction weights (for composite scoring)
    std::vector<std::pair<std::string, double>> weights;
};

enum class OutputFormat { PNG, Video };

// Output directory mode (internal use only - not configurable via TOML)
// This is set programmatically by batch mode; single runs always use Timestamped
enum class OutputMode {
    Timestamped, // Create run_YYYYMMDD_HHMMSS subdirectory (default for single runs)
    Direct       // Write directly to output.directory (used by batch mode)
};

struct OutputParams {
    OutputFormat format = OutputFormat::PNG;
    std::string directory = "output";
    std::string filename_prefix = "frame";
    std::string video_codec = "libx264";
    int video_crf = 23;
    int video_fps = 60;                        // Only affects video encoding, not simulation
    OutputMode mode = OutputMode::Timestamped; // Internal: set by batch mode, not TOML

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

    // Per-metric configuration (replaces old MetricParams and BoomDetectionParams)
    std::unordered_map<std::string, MetricConfig> metric_configs;

    // Which metric to use for boom detection (legacy - prefer using targets)
    // Empty by default - should be set via [targets.boom] in config
    std::string boom_metric;

    // Multi-target prediction configuration
    // REQUIRED: Define [targets.boom] in config file to enable boom detection
    // No auto-generation from boom_metric - targets must be explicit
    std::vector<TargetConfig> targets;

    OutputParams output;
    AnalysisParams analysis;

    // Preset names (set by batch generator when random preset selected)
    // These are for metadata output only - not saved to config.toml
    std::string selected_color_preset_name;
    std::string selected_post_process_preset_name;
    std::string selected_theme_name;  // Set when using theme presets

    // Get config for a specific metric (returns nullptr if not configured)
    MetricConfig const* getMetricConfig(std::string const& name) const {
        auto it = metric_configs.find(name);
        return it != metric_configs.end() ? &it->second : nullptr;
    }

    // Get or create config for a metric (creates with defaults if not exists)
    MetricConfig& getOrCreateMetricConfig(std::string const& name) {
        auto it = metric_configs.find(name);
        if (it == metric_configs.end()) {
            metric_configs[name] = createDefaultMetricConfig(name);
            return metric_configs[name];
        }
        return it->second;
    }

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
