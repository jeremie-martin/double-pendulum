#include "color_scheme.h"
#include "config.h"
#include "gl_renderer.h"
#include "metrics/boom_detection.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metric_registry.h"
#include "metrics/metrics_collector.h"
#include "metrics/metrics_init.h"
#include "optimize/prediction_target.h"
#include "optimize/target_evaluator.h"
#include "pendulum.h"
#include "preset_library.h"
#include "simulation.h"
#include "simulation_data.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <implot.h>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// Preview parameters (lower resolution for real-time)
struct PreviewParams {
    int width = 540;
    int height = 540;
    int pendulum_count = 10000;
    PhysicsQuality physics_quality = PhysicsQuality::High;
    double max_dt = 0.007; // Computed from quality

    // Compute substeps for a given frame duration
    int substeps(double frame_dt) const {
        return std::max(1, static_cast<int>(std::ceil(frame_dt / max_dt)));
    }
};

// Plot scaling mode for multi-axis support
enum class PlotMode {
    SingleAxis, // All metrics on one Y-axis (auto-fit)
    MultiAxis,  // Group by scale: Y1=large, Y2=normalized, Y3=medium
    Normalized  // All metrics scaled to 0-1
};

// Graph metric flags for multi-select (with derivative toggles)
// Map-based implementation using centralized registry for default values
struct MetricFlags {
    // Stores (enabled, deriv_enabled) for each metric by name
    std::unordered_map<std::string, std::pair<bool, bool>> flags;

    MetricFlags() {
        // Initialize from registry with default_enabled values
        for (auto const& m : metrics::METRIC_REGISTRY) {
            flags[m.name] = {m.default_enabled, false};
        }
    }

    bool enabled(std::string const& name) const {
        auto it = flags.find(name);
        return it != flags.end() && it->second.first;
    }

    bool derivEnabled(std::string const& name) const {
        auto it = flags.find(name);
        return it != flags.end() && it->second.second;
    }

    bool* enabledPtr(std::string const& name) { return &flags[name].first; }

    bool* derivPtr(std::string const& name) { return &flags[name].second; }
};

// Export state (thread-safe)
struct ExportState {
    std::atomic<bool> active{false};
    std::atomic<bool> cancel_requested{false};
    std::atomic<int> current_frame{0};
    std::atomic<int> total_frames{0};
    std::mutex result_mutex;
    std::string result_message;
    std::string output_path;
    std::thread export_thread;

    void reset() {
        active = false;
        cancel_requested = false;
        current_frame = 0;
        total_frames = 0;
        std::lock_guard<std::mutex> lock(result_mutex);
        result_message.clear();
        output_path.clear();
    }
};

// Preset UI state
struct PresetUIState {
    // Color preset
    std::string loaded_color_preset; // Name of currently loaded preset (empty if none)
    ColorParams loaded_color_values; // Values when preset was loaded (for detecting changes)
    char new_color_preset_name[64] = "";
    bool show_color_save_popup = false;
    bool show_color_delete_confirm = false;

    // Post-process preset
    std::string loaded_pp_preset;
    PostProcessParams loaded_pp_values;
    char new_pp_preset_name[64] = "";
    bool show_pp_save_popup = false;
    bool show_pp_delete_confirm = false;

    // Check if color values have been modified from loaded preset
    bool isColorModified(ColorParams const& current) const {
        if (loaded_color_preset.empty())
            return false;
        return current.scheme != loaded_color_values.scheme ||
               std::abs(current.start - loaded_color_values.start) > 0.001 ||
               std::abs(current.end - loaded_color_values.end) > 0.001;
    }

    // Check if post-process values have been modified
    bool isPPModified(PostProcessParams const& current) const {
        if (loaded_pp_preset.empty())
            return false;
        return current.tone_map != loaded_pp_values.tone_map ||
               std::abs(current.exposure - loaded_pp_values.exposure) > 0.001 ||
               std::abs(current.contrast - loaded_pp_values.contrast) > 0.001 ||
               std::abs(current.gamma - loaded_pp_values.gamma) > 0.001 ||
               std::abs(current.reinhard_white_point - loaded_pp_values.reinhard_white_point) >
                   0.001 ||
               current.normalization != loaded_pp_values.normalization;
    }
};

// Per-metric boom detection state (for multi-boom visualization)
struct MetricBoomState {
    bool enabled = false;     // Show this metric's boom line on graph
    bool show_params = false; // Show this metric's params in UI panel
    int boom_frame = -1;      // Detected boom frame for this metric
    double boom_value = 0.0;  // Causticness/metric value at boom
};

// List of all metrics that support boom detection (uses centralized registry)
inline std::vector<std::string> getAllBoomMetrics() {
    auto boom_defs = metrics::getBoomMetrics();
    std::vector<std::string> result;
    result.reserve(boom_defs.size());
    for (auto const* m : boom_defs) {
        result.push_back(m->name);
    }
    return result;
}

// Application state
struct AppState {
    Config config;
    PreviewParams preview;
    PresetLibrary presets;
    PresetUIState preset_ui;

    // Simulation state
    std::vector<Pendulum> pendulums;
    std::vector<PendulumState> states;
    std::vector<Color> colors;
    metrics::MetricsCollector metrics_collector;
    metrics::EventDetector event_detector;
    metrics::CausticnessAnalyzer causticness_analyzer;
    MetricFlags detailed_flags; // For Detailed Analysis window

    // Frame history for timeline scrubbing
    std::vector<std::vector<PendulumState>> frame_history;
    int max_history_frames = 1000; // Limit memory usage

    // Control
    bool running = false;
    bool paused = false;
    bool needs_redraw = false; // For re-rendering when paused
    int current_frame = 0;
    int display_frame = 0;  // Frame being displayed (for timeline scrubbing)
    bool scrubbing = false; // True when user is dragging timeline

    // Detection results
    std::optional<int> boom_frame;
    double boom_causticness = 0.0;
    std::optional<int> chaos_frame;
    double chaos_variance = 0.0;

    // Timing
    double fps = 0.0;
    double sim_time_ms = 0.0;
    double render_time_ms = 0.0;

    // Cached frame duration (updated on config change)
    // Avoids recomputing in every stepSimulation call
    double frame_duration = 0.0;

    // Update frame_duration from config
    void updateFrameDuration() {
        frame_duration = config.simulation.frameDuration();
        causticness_analyzer.setFrameDuration(frame_duration);
    }

    // Export
    ExportState export_state;

    // Loaded simulation data (for replay mode)
    std::unique_ptr<simulation_data::Reader> loaded_data;
    std::string loaded_data_path;
    bool replay_mode = false; // True when playing back loaded data

    // Replay timing
    std::chrono::high_resolution_clock::time_point replay_start_time;
    int replay_start_frame = 0;
    bool replay_playing = false; // True when actively replaying in real-time

    // Metric parameters window
    bool show_metric_params_window = false;
    bool needs_metric_recompute = false;
    std::string editing_metric; // Which metric's params are being edited (separate from primary)

    // Multi-metric boom detection state
    std::map<std::string, MetricBoomState> boom_metrics;

    // Initialize boom_metrics map with all metrics disabled
    void initBoomMetrics() {
        for (auto const& name : getAllBoomMetrics()) {
            boom_metrics[name] = MetricBoomState{};
        }
        // Enable the primary metric by default
        if (boom_metrics.count(config.boom_metric)) {
            boom_metrics[config.boom_metric].enabled = true;
        }
    }
};

// Helper: Update boom detection using configured params and run analyzers
// Consolidates duplicate boom detection logic from multiple places
void updateBoomDetection(AppState& state, bool run_analyzer = true) {
    // Look for a "boom" target in config.targets
    optimize::FrameDetectionParams boom_params;
    bool has_boom_target = false;
    for (auto const& tc : state.config.targets) {
        if (tc.name == "boom" && tc.type == "frame") {
            auto target = optimize::targetConfigToPredictionTarget(
                tc.name, tc.type, tc.metric, tc.method, tc.offset_seconds,
                tc.peak_percent_threshold, tc.min_peak_prominence, tc.smoothing_window,
                tc.crossing_threshold, tc.crossing_confirmation, tc.weights);
            boom_params = target.frameParams();
            has_boom_target = true;
            break;
        }
    }

    // Use configured boom target params; if none configured, skip boom detection
    metrics::BoomDetection boom;
    if (has_boom_target) {
        boom = metrics::findBoomFrame(state.metrics_collector, state.frame_duration, boom_params);
    } else {
        // No boom target configured - boom detection disabled
        boom.frame = -1;
    }

    if (boom.frame >= 0) {
        state.boom_frame = boom.frame;
        state.boom_causticness = boom.metric_value;

        double variance_at_boom = 0.0;
        if (auto const* var = state.metrics_collector.getMetric(metrics::MetricNames::Variance)) {
            if (boom.frame < static_cast<int>(var->size())) {
                variance_at_boom = var->at(boom.frame);
            }
        }
        metrics::forceBoomEvent(state.event_detector, boom, variance_at_boom);
    } else {
        state.boom_frame.reset();
    }

    if (run_analyzer && state.boom_frame) {
        state.causticness_analyzer.analyze(state.metrics_collector, state.event_detector);
    }
}

// Helper: Update chaos event detection
void updateChaosDetection(AppState& state) {
    if (auto chaos = state.event_detector.getEvent(metrics::EventNames::Chaos)) {
        state.chaos_frame = chaos->frame;
        state.chaos_variance = chaos->value;
    } else {
        state.chaos_frame.reset();
    }
}

// Helper: Update boom detection for all enabled metrics
// This runs boom detection independently for each metric's configured params
void updateAllBoomDetections(AppState& state) {
    for (auto& [metric_name, boom_state] : state.boom_metrics) {
        if (!boom_state.enabled) {
            boom_state.boom_frame = -1;
            boom_state.boom_value = 0.0;
            continue;
        }

        // Use default detection params with this metric
        optimize::FrameDetectionParams params;
        params.metric_name = metric_name;
        params.method = optimize::FrameDetectionMethod::MaxValue;
        params.offset_seconds = 0.3;

        auto boom = metrics::findBoomFrame(state.metrics_collector, state.frame_duration, params);
        boom_state.boom_frame = boom.frame;
        boom_state.boom_value = boom.metric_value;
    }

    // Also update primary boom (for other GUI features that use state.boom_frame)
    updateBoomDetection(state);
}

// Helper: Shorten metric names for compact button display
// Uses centralized metric registry for short names
std::string shortenMetricName(std::string const& name) {
    return std::string(metrics::getShortName(name));
}

void initSimulation(AppState& state, GLRenderer& renderer) {
    int n = state.preview.pendulum_count;

    state.pendulums.resize(n);
    state.states.resize(n);
    state.colors.resize(n);

    double center_angle = state.config.physics.initial_angle1;
    double variation = state.config.simulation.angle_variation;

    ColorSchemeGenerator color_gen(state.config.color);

    for (int i = 0; i < n; ++i) {
        double t = (n > 1) ? static_cast<double>(i) / (n - 1) : 0.0;
        double th1 = center_angle - variation / 2 + t * variation;

        state.pendulums[i] = Pendulum(
            state.config.physics.gravity, state.config.physics.length1,
            state.config.physics.length2, state.config.physics.mass1, state.config.physics.mass2,
            th1, state.config.physics.initial_angle2, state.config.physics.initial_velocity1,
            state.config.physics.initial_velocity2);

        state.colors[i] = color_gen.getColorForIndex(i, n);
    }

    // Initialize metrics system using common helper
    state.updateFrameDuration(); // Cache frame_duration and set on analyzer
    metrics::resetMetricsSystem(state.metrics_collector, state.event_detector,
                                state.causticness_analyzer);
    metrics::initializeMetricsSystem(state.metrics_collector, state.event_detector,
                                     state.causticness_analyzer, state.frame_duration,
                                     /*with_gpu=*/true);

    // Apply per-metric configs from config
    state.metrics_collector.setAllMetricConfigs(state.config.metric_configs);

    state.boom_frame.reset();
    state.chaos_frame.reset();
    state.current_frame = 0;
    state.display_frame = 0;
    state.scrubbing = false;
    state.running = true;
    state.paused = false;

    // Clear frame history
    state.frame_history.clear();
    state.frame_history.reserve(state.max_history_frames);

    renderer.resize(state.preview.width, state.preview.height);

    // Exit replay mode when starting fresh simulation
    state.replay_mode = false;
    state.replay_playing = false;
    state.loaded_data.reset();
    state.loaded_data_path.clear();
}

// Forward declaration (defined later)
void renderFrameFromHistory(AppState& state, GLRenderer& renderer, int frame_index);

// Load simulation data from file and prepare for replay
bool loadSimulationData(AppState& state, GLRenderer& renderer, std::string const& path) {
    namespace fs = std::filesystem;

    // Create reader
    auto reader = std::make_unique<simulation_data::Reader>();
    if (!reader->open(path)) {
        std::cerr << "Failed to load simulation data from: " << path << "\n";
        return false;
    }

    std::cout << "Loaded simulation: " << reader->pendulumCount() << " pendulums, "
              << reader->frameCount() << " frames\n";

    // Try to load config from same directory
    fs::path data_path(path);
    fs::path config_path = data_path.parent_path() / "config.toml";
    if (fs::exists(config_path)) {
        state.config = Config::load(config_path.string());
        std::cout << "Loaded config from: " << config_path << "\n";
    } else {
        std::cout << "Warning: No config.toml found, using current settings\n";
    }

    // Update config to match loaded data
    state.config.simulation.total_frames = static_cast<int>(reader->frameCount());
    state.config.simulation.duration_seconds = reader->header().duration_seconds;
    state.config.physics.initial_angle1 = reader->header().initial_angle1;
    state.config.physics.initial_angle2 = reader->header().initial_angle2;
    state.config.simulation.angle_variation = reader->header().angle_variation;

    // Setup colors based on loaded pendulum count
    int n = static_cast<int>(reader->pendulumCount());
    state.preview.pendulum_count = n;
    state.colors.resize(n);
    ColorSchemeGenerator color_gen(state.config.color);
    for (int i = 0; i < n; ++i) {
        state.colors[i] = color_gen.getColorForIndex(i, n);
    }

    // Pre-load all frames into frame_history for scrubbing
    state.frame_history.clear();
    state.frame_history.reserve(reader->frameCount());
    for (uint32_t f = 0; f < reader->frameCount(); ++f) {
        state.frame_history.push_back(reader->getFrame(f));
    }

    // Compute all physics metrics for the loaded data
    state.updateFrameDuration();
    metrics::resetMetricsSystem(state.metrics_collector, state.event_detector,
                                state.causticness_analyzer);
    metrics::initializeMetricsSystem(state.metrics_collector, state.event_detector,
                                     state.causticness_analyzer, state.frame_duration,
                                     /*with_gpu=*/true);

    for (uint32_t f = 0; f < reader->frameCount(); ++f) {
        auto states = reader->getFrame(f);
        state.metrics_collector.beginFrame(static_cast<int>(f));
        state.metrics_collector.updateFromStates(states);
        state.metrics_collector.endFrame();
        state.event_detector.update(state.metrics_collector, state.frame_duration);
    }

    // Detect boom and chaos, run analyzers
    updateBoomDetection(state);
    updateChaosDetection(state);

    // Setup state for replay
    state.loaded_data = std::move(reader);
    state.loaded_data_path = path;
    state.replay_mode = true;
    state.replay_playing = false;
    state.running = true;
    state.paused = true; // Start paused so user can see frame 0
    state.current_frame = static_cast<int>(state.frame_history.size()) - 1;
    state.display_frame = 0;
    state.scrubbing = true; // Show scrubbing UI initially

    // Resize renderer to match config
    renderer.resize(state.config.render.width, state.config.render.height);
    state.preview.width = state.config.render.width;
    state.preview.height = state.config.render.height;

    // Render the first frame
    if (!state.frame_history.empty()) {
        renderFrameFromHistory(state, renderer, 0);
    }

    std::cout << "Ready for replay. Press Space to play.\n";
    return true;
}

// Start real-time replay from current display frame
void startReplay(AppState& state) {
    if (!state.replay_mode || state.frame_history.empty())
        return;

    state.replay_playing = true;
    state.paused = false;
    state.replay_start_frame = state.display_frame;
    state.replay_start_time = std::chrono::high_resolution_clock::now();
}

// Stop replay
void stopReplay(AppState& state) {
    state.replay_playing = false;
    state.paused = true;
}

// Update replay - advances display_frame based on real time
void updateReplay(AppState& state, GLRenderer& renderer) {
    if (!state.replay_playing || state.frame_history.empty())
        return;

    auto now = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(now - state.replay_start_time).count();

    // Calculate which frame we should be on
    int frames_elapsed = static_cast<int>(elapsed / state.frame_duration);
    int target_frame = state.replay_start_frame + frames_elapsed;

    // Clamp to valid range
    int max_frame = static_cast<int>(state.frame_history.size()) - 1;
    if (target_frame > max_frame) {
        target_frame = max_frame;
        stopReplay(state); // Stop at end
    }

    // Update display if frame changed
    if (target_frame != state.display_frame) {
        state.display_frame = target_frame;
        renderFrameFromHistory(state, renderer, target_frame);
    }
}

// Render a given set of states
void renderStates(AppState& state, GLRenderer& renderer,
                  std::vector<PendulumState> const& states_to_render) {
    if (!state.running || states_to_render.empty())
        return;

    auto render_start = std::chrono::high_resolution_clock::now();

    renderer.clear();

    int n = states_to_render.size();
    float scale = state.preview.width / 5.0f;
    float cx = state.preview.width / 2.0f;
    float cy = state.preview.height / 2.0f;

    for (int i = 0; i < n; ++i) {
        auto const& s = states_to_render[i];
        auto const& c = state.colors[i];

        float x0 = cx;
        float y0 = cy;
        float x1 = cx + s.x1 * scale;
        float y1 = cy + s.y1 * scale; // Positive Y = below pivot (screen Y also increases downward)
        float x2 = cx + s.x2 * scale;
        float y2 = cy + s.y2 * scale;

        renderer.drawLine(x0, y0, x1, y1, c.r, c.g, c.b);
        renderer.drawLine(x1, y1, x2, y2, c.r, c.g, c.b);
    }

    renderer.updateDisplayTexture(
        static_cast<float>(state.config.post_process.exposure),
        static_cast<float>(state.config.post_process.contrast),
        static_cast<float>(state.config.post_process.gamma), state.config.post_process.tone_map,
        static_cast<float>(state.config.post_process.reinhard_white_point),
        state.config.post_process.normalization, static_cast<int>(states_to_render.size()));

    // Compute brightness/contrast metrics from rendered frame
    renderer.computeMetrics();

    auto render_end = std::chrono::high_resolution_clock::now();
    state.render_time_ms =
        std::chrono::duration<double, std::milli>(render_end - render_start).count();
}

// Render current state (without physics step)
void renderFrame(AppState& state, GLRenderer& renderer) {
    renderStates(state, renderer, state.states);
}

// Render a specific frame from history
void renderFrameFromHistory(AppState& state, GLRenderer& renderer, int frame_index) {
    if (frame_index < 0 || frame_index >= static_cast<int>(state.frame_history.size())) {
        return;
    }
    renderStates(state, renderer, state.frame_history[frame_index]);
}

void stepSimulation(AppState& state, GLRenderer& renderer) {
    if (!state.running || state.paused)
        return;

    auto start = std::chrono::high_resolution_clock::now();

    int n = state.pendulums.size();
    double frame_dt =
        state.config.simulation.duration_seconds / state.config.simulation.total_frames;
    int substeps = state.preview.substeps(frame_dt);
    double dt = frame_dt / substeps;

    // Physics step
    for (int s = 0; s < substeps; ++s) {
        for (int i = 0; i < n; ++i) {
            state.states[i] = state.pendulums[i].step(dt);
        }
    }

    // Begin frame for metrics collection
    state.metrics_collector.beginFrame(state.current_frame);

    // Track all physics metrics (variance, spread, causticness metrics)
    // using full state data for position-based metrics
    state.metrics_collector.updateFromStates(state.states);

    // Compute total energy for extended analysis
    double total_energy = 0.0;
    for (auto const& p : state.pendulums) {
        total_energy += p.totalEnergy();
    }
    state.metrics_collector.setMetric(metrics::MetricNames::TotalEnergy, total_energy);

    // Use cached frame_duration (updated on config change via updateFrameDuration())

    auto sim_end = std::chrono::high_resolution_clock::now();
    state.sim_time_ms = std::chrono::duration<double, std::milli>(sim_end - start).count();

    // Save to frame history (if under limit)
    if (static_cast<int>(state.frame_history.size()) < state.max_history_frames) {
        state.frame_history.push_back(state.states);
    }

    // Render (needed before endFrame to get GPU metrics)
    renderFrame(state, renderer);

    // Update metrics collector with GPU stats after rendering but BEFORE endFrame
    metrics::GPUMetricsBundle gpu_metrics;
    gpu_metrics.max_value = renderer.lastMax();
    gpu_metrics.brightness = renderer.lastBrightness();
    gpu_metrics.coverage = renderer.lastCoverage();
    state.metrics_collector.setGPUMetrics(gpu_metrics);

    // End frame metrics collection (after ALL metrics including GPU are set)
    state.metrics_collector.endFrame();

    // Update event detection (needs complete frame data)
    state.event_detector.update(state.metrics_collector, state.frame_duration);

    // Extract chaos event (only set once when first detected)
    if (auto chaos_event = state.event_detector.getEvent(metrics::EventNames::Chaos)) {
        if (chaos_event->detected() && !state.chaos_frame) {
            state.chaos_frame = chaos_event->frame;
            state.chaos_variance = chaos_event->value;
        }
    }

    // Boom detection using configured params (run analyzer every 30 frames)
    bool run_analyzer = state.boom_frame && (state.current_frame % 30 == 0);
    updateBoomDetection(state, run_analyzer);

    state.current_frame++;
    state.display_frame = state.current_frame;
}

// Static plot mode for persistence across frames
static PlotMode s_plot_mode = PlotMode::MultiAxis;

// Helper to get metric value at a specific frame (for timeline scrubbing)
double getMetricAtFrame(metrics::MetricsCollector const& collector, std::string const& name,
                        int frame) {
    auto const* series = collector.getMetric(name);
    if (!series || series->empty())
        return 0.0;
    if (frame < 0)
        return 0.0;
    auto const& values = series->values();
    size_t idx = std::min(static_cast<size_t>(frame), values.size() - 1);
    return values[idx];
}

void drawMetricGraph(AppState& state, ImVec2 size) {
    // Get first available metric to determine frame count
    auto* first_series = state.metrics_collector.getMetric(metrics::MetricNames::Variance);
    if (!first_series || first_series->empty()) {
        ImGui::Text("No data yet");
        return;
    }

    size_t data_size = first_series->size();

    // Create frame index array for x-axis
    std::vector<double> frames(data_size);
    for (size_t i = 0; i < data_size; ++i) {
        frames[i] = static_cast<double>(i);
    }

    // Current frame marker (for dragging)
    double current_frame_d = static_cast<double>(state.display_frame);

    // Helper to normalize a series to 0-1 range
    auto normalizeData = [](std::vector<double> const& data) -> std::vector<double> {
        if (data.empty())
            return {};
        double min_val = *std::min_element(data.begin(), data.end());
        double max_val = *std::max_element(data.begin(), data.end());
        double range = max_val - min_val;
        if (range < 1e-10)
            range = 1.0; // Avoid division by zero
        std::vector<double> result(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            result[i] = (data[i] - min_val) / range;
        }
        return result;
    };

    if (ImPlot::BeginPlot("##Metrics", size, ImPlotFlags_NoTitle)) {
        // Setup axes based on plot mode
        if (s_plot_mode == PlotMode::MultiAxis) {
            // Multi-axis: Y1=Large scale, Y2=Normalized (0-1), Y3=Medium scale
            ImPlot::SetupAxes("Frame", "Large", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y2, "[0-1]",
                              ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y3, "Med",
                              ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
        } else if (s_plot_mode == PlotMode::Normalized) {
            // Normalized mode: all metrics on 0-1 scale
            ImPlot::SetupAxes("Frame", "[0-1]", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
        } else {
            ImPlot::SetupAxes("Frame", nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        }

        // Plot all metrics from centralized registry
        for (auto const& m : metrics::METRIC_REGISTRY) {
            // Skip if not enabled
            if (!state.detailed_flags.enabled(m.name))
                continue;

            // Get metric series
            auto const* series = state.metrics_collector.getMetric(m.name);
            if (!series || series->empty())
                continue;

            // Set axis based on registry
            if (s_plot_mode == PlotMode::MultiAxis) {
                switch (m.axis) {
                case metrics::PlotAxis::Y1_Large:
                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                    break;
                case metrics::PlotAxis::Y2_Normalized:
                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                    break;
                case metrics::PlotAxis::Y3_Medium:
                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
                    break;
                }
            }

            // Set color from registry
            ImPlot::SetNextLineStyle(ImVec4(m.color.r, m.color.g, m.color.b, m.color.a), 2.0f);

            // Plot values
            auto const& values = series->values();
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(values);
                ImPlot::PlotLine(m.short_name, frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine(m.short_name, frames.data(), values.data(), values.size());
            }

            // Plot derivative if enabled
            if (state.detailed_flags.derivEnabled(m.name) && series->size() > 1) {
                auto derivs = series->derivativeHistory();
                if (!derivs.empty()) {
                    // Derivative uses same color with lower alpha
                    auto deriv_color = m.color.deriv();
                    ImPlot::SetNextLineStyle(
                        ImVec4(deriv_color.r, deriv_color.g, deriv_color.b, deriv_color.a), 1.0f);
                    std::string deriv_name = std::string(m.short_name) + "'";
                    if (s_plot_mode == PlotMode::Normalized) {
                        auto normalized = normalizeData(derivs);
                        ImPlot::PlotLine(deriv_name.c_str(), frames.data() + 1, normalized.data(),
                                         derivs.size());
                    } else {
                        ImPlot::PlotLine(deriv_name.c_str(), frames.data() + 1, derivs.data(),
                                         derivs.size());
                    }
                }
            }
        }

        // Draw boom markers for all enabled metrics
        // Primary metric = bright thick red-orange line
        // Secondary metrics = visible cyan lines
        for (auto const& [metric_name, boom_state] : state.boom_metrics) {
            if (!boom_state.enabled || boom_state.boom_frame < 0)
                continue;

            bool is_primary = (metric_name == state.config.boom_metric);
            double boom_x = static_cast<double>(boom_state.boom_frame);

            if (is_primary) {
                // Primary: Bright thick red-orange line
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 0.1f, 1.0f), 4.0f);
            } else {
                // Secondary: Visible cyan line
                ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.9f, 1.0f, 0.85f), 2.5f);
            }

            std::string label = "##boom_" + metric_name;
            ImPlot::PlotInfLines(label.c_str(), &boom_x, 1);

            // Add annotation for primary boom
            if (is_primary) {
                auto limits = ImPlot::GetPlotLimits();
                double y_top = limits.Y.Max * 0.92;
                ImPlot::Annotation(boom_x, y_top, ImVec4(1.0f, 0.4f, 0.1f, 1.0f), ImVec2(3, 0),
                                   true, "BOOM");
            }
        }

        // Fallback: draw primary boom if no boom_metrics enabled (from state.boom_frame)
        if (state.boom_frame.has_value()) {
            bool any_enabled = false;
            for (auto const& [_, bs] : state.boom_metrics) {
                if (bs.enabled) {
                    any_enabled = true;
                    break;
                }
            }
            if (!any_enabled) {
                double boom_x = static_cast<double>(*state.boom_frame);
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 0.1f, 1.0f), 4.0f);
                ImPlot::PlotInfLines("##boom_primary", &boom_x, 1);
            }
        }

        // Draw chaos marker (blue)
        if (state.chaos_frame.has_value()) {
            double chaos_x = static_cast<double>(*state.chaos_frame);
            ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.5f, 1.0f, 1.0f), 2.0f);
            ImPlot::PlotInfLines("##chaos", &chaos_x, 1);
        }

        // Draggable current frame marker
        ImPlot::SetNextLineStyle(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), 2.0f);
        if (ImPlot::DragLineX(0, &current_frame_d, ImVec4(0.0f, 0.8f, 1.0f, 1.0f))) {
            state.display_frame =
                std::clamp(static_cast<int>(current_frame_d), 0, static_cast<int>(data_size) - 1);
            state.scrubbing = true;
            state.needs_redraw = true;
        }

        ImPlot::EndPlot();
    }
}

void startExport(AppState& state) {
    if (state.export_state.active)
        return;

    // Join previous thread if exists
    if (state.export_state.export_thread.joinable()) {
        state.export_state.export_thread.join();
    }

    state.export_state.reset();
    state.export_state.active = true;
    state.export_state.total_frames = state.config.simulation.total_frames;

    // Create a copy of config for the export thread
    Config export_config = state.config;

    // Capture pointer to export_state explicitly - makes thread access clear
    // and prevents accidental access to other AppState members
    ExportState* export_state_ptr = &state.export_state;

    state.export_state.export_thread = std::thread([export_state_ptr, export_config]() {
        try {
            Simulation sim(export_config);

            sim.run([export_state_ptr](int current, int total) {
                export_state_ptr->current_frame = current;
                export_state_ptr->total_frames = total;
            });

            // Update result
            {
                std::lock_guard<std::mutex> lock(export_state_ptr->result_mutex);
                export_state_ptr->result_message = "Export completed successfully!";
                export_state_ptr->output_path = export_config.output.directory;
            }
        } catch (std::exception const& e) {
            std::lock_guard<std::mutex> lock(export_state_ptr->result_mutex);
            export_state_ptr->result_message = std::string("Export failed: ") + e.what();
        }

        export_state_ptr->active = false;
    });
}

void drawExportPanel(AppState& state) {
    ImGui::Separator();
    ImGui::Text("Export");

    if (state.export_state.active) {
        // Show progress
        int current = state.export_state.current_frame;
        int total = state.export_state.total_frames;
        float progress = total > 0 ? static_cast<float>(current) / total : 0.0f;

        ImGui::ProgressBar(progress, ImVec2(-1, 0));
        ImGui::Text("Frame %d / %d", current, total);

        if (ImGui::Button("Cancel")) {
            state.export_state.cancel_requested = true;
        }
    } else {
        // Show export button and settings
        if (ImGui::CollapsingHeader("Export Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderInt("Width", &state.config.render.width, 540, 4320);
            ImGui::SliderInt("Height", &state.config.render.height, 540, 4320);
            ImGui::SliderInt("Pendulum Count", &state.config.simulation.pendulum_count, 1000,
                             500000);
            ImGui::SliderInt("Total Frames", &state.config.simulation.total_frames, 60, 3600);
            ImGui::SliderInt("Video FPS", &state.config.output.video_fps, 24, 120);

            // Calculate and display video duration
            double video_duration = static_cast<double>(state.config.simulation.total_frames) /
                                    state.config.output.video_fps;
            ImGui::Text("Video duration: %.2f seconds", video_duration);

            const char* formats[] = {"PNG Sequence", "Video (MP4)"};
            int format_idx = state.config.output.format == OutputFormat::PNG ? 0 : 1;
            if (ImGui::Combo("Format", &format_idx, formats, 2)) {
                state.config.output.format =
                    format_idx == 0 ? OutputFormat::PNG : OutputFormat::Video;
            }
        }

        if (ImGui::Button("Export Full Quality", ImVec2(-1, 40))) {
            startExport(state);
        }

        // Show result message if any
        std::lock_guard<std::mutex> lock(state.export_state.result_mutex);
        if (!state.export_state.result_message.empty()) {
            ImGui::TextWrapped("%s", state.export_state.result_message.c_str());
            if (!state.export_state.output_path.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Output: %s",
                                   state.export_state.output_path.c_str());
            }
        }
    }
}

// Helper for consistent tooltips
void tooltip(const char* text) {
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

// Random number generator for physics randomization
static std::random_device rd;
static std::mt19937 rng(rd());

// Randomize physics parameters within sensible ranges
void randomizePhysics(AppState& state) {
    std::uniform_real_distribution<double> angle_dist(100.0, 260.0); // degrees, around 180
    std::uniform_real_distribution<double> length_dist(0.5, 1.5);
    std::uniform_real_distribution<double> mass_dist(0.5, 2.0);
    std::uniform_real_distribution<double> vel_dist(-2.0, 2.0);

    state.config.physics.initial_angle1 = deg2rad(angle_dist(rng));
    state.config.physics.initial_angle2 = deg2rad(angle_dist(rng));
    state.config.physics.length1 = length_dist(rng);
    state.config.physics.length2 = length_dist(rng);
    state.config.physics.mass1 = mass_dist(rng);
    state.config.physics.mass2 = mass_dist(rng);
    state.config.physics.initial_velocity1 = vel_dist(rng);
    state.config.physics.initial_velocity2 = vel_dist(rng);
}

// Draw a color ramp preview (like Blender's ColorRamp)
void drawColorRamp(ColorParams const& params, float width, float height) {
    ColorSchemeGenerator gen(params);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Draw gradient using multiple rectangles
    int segments = static_cast<int>(width);
    float segment_width = width / segments;

    for (int i = 0; i < segments; ++i) {
        float t = static_cast<float>(i) / (segments - 1);
        Color c = gen.getColor(t);

        ImU32 col = IM_COL32(static_cast<int>(c.r * 255), static_cast<int>(c.g * 255),
                             static_cast<int>(c.b * 255), 255);

        draw_list->AddRectFilled(ImVec2(pos.x + i * segment_width, pos.y),
                                 ImVec2(pos.x + (i + 1) * segment_width + 1, pos.y + height), col);
    }

    // Draw border
    draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(100, 100, 100, 255));

    // Draw start/end markers (triangular handles like Blender)
    float marker_size = 8.0f;
    float start_x = pos.x + params.start * width;
    float end_x = pos.x + params.end * width;

    // Start marker (bottom triangle)
    draw_list->AddTriangleFilled(ImVec2(start_x, pos.y + height),
                                 ImVec2(start_x - marker_size / 2, pos.y + height + marker_size),
                                 ImVec2(start_x + marker_size / 2, pos.y + height + marker_size),
                                 IM_COL32(200, 200, 200, 255));
    draw_list->AddTriangle(ImVec2(start_x, pos.y + height),
                           ImVec2(start_x - marker_size / 2, pos.y + height + marker_size),
                           ImVec2(start_x + marker_size / 2, pos.y + height + marker_size),
                           IM_COL32(50, 50, 50, 255));

    // End marker
    draw_list->AddTriangleFilled(ImVec2(end_x, pos.y + height),
                                 ImVec2(end_x - marker_size / 2, pos.y + height + marker_size),
                                 ImVec2(end_x + marker_size / 2, pos.y + height + marker_size),
                                 IM_COL32(200, 200, 200, 255));
    draw_list->AddTriangle(ImVec2(end_x, pos.y + height),
                           ImVec2(end_x - marker_size / 2, pos.y + height + marker_size),
                           ImVec2(end_x + marker_size / 2, pos.y + height + marker_size),
                           IM_COL32(50, 50, 50, 255));

    // Advance cursor
    ImGui::Dummy(ImVec2(width, height + marker_size + 4));
}

void drawPreviewSection(AppState& state) {
    if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderInt("Pendulums", &state.preview.pendulum_count, 1000, 100000);
        tooltip("Number of pendulums in preview (lower = faster)");

        ImGui::SliderInt("Preview Size", &state.preview.width, 270, 1080);
        tooltip("Preview resolution (lower = faster)");
        state.preview.height = state.preview.width;

        // Physics quality dropdown (replaces raw substeps)
        const char* quality_names[] = {"Low", "Medium", "High", "Ultra"};
        int quality_idx = static_cast<int>(state.preview.physics_quality);
        if (ImGui::Combo("Physics Quality", &quality_idx, quality_names, 4)) {
            state.preview.physics_quality = static_cast<PhysicsQuality>(quality_idx);
            state.preview.max_dt = qualityToMaxDt(state.preview.physics_quality);
        }
        tooltip("Low=20ms, Medium=12ms, High=7ms, Ultra=3ms max timestep");

        // Show computed values
        double frame_dt =
            state.config.simulation.duration_seconds / state.config.simulation.total_frames;
        int computed_substeps = state.preview.substeps(frame_dt);
        ImGui::Text("Computed: substeps=%d, dt=%.2fms", computed_substeps,
                    (frame_dt / computed_substeps) * 1000.0);
    }
}

void drawPhysicsSection(AppState& state) {
    if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto gravity = static_cast<float>(state.config.physics.gravity);
        if (ImGui::SliderFloat("Gravity", &gravity, 0.1f, 20.0f)) {
            state.config.physics.gravity = gravity;
        }
        tooltip("Gravitational acceleration (m/s^2)");

        auto length1 = static_cast<float>(state.config.physics.length1);
        if (ImGui::SliderFloat("Length 1", &length1, 0.1f, 3.0f)) {
            state.config.physics.length1 = length1;
        }
        tooltip("Length of first pendulum arm (m)");

        auto length2 = static_cast<float>(state.config.physics.length2);
        if (ImGui::SliderFloat("Length 2", &length2, 0.1f, 3.0f)) {
            state.config.physics.length2 = length2;
        }
        tooltip("Length of second pendulum arm (m)");

        auto mass1 = static_cast<float>(state.config.physics.mass1);
        if (ImGui::SliderFloat("Mass 1", &mass1, 0.1f, 5.0f)) {
            state.config.physics.mass1 = mass1;
        }
        tooltip("Mass of first bob (kg)");

        auto mass2 = static_cast<float>(state.config.physics.mass2);
        if (ImGui::SliderFloat("Mass 2", &mass2, 0.1f, 5.0f)) {
            state.config.physics.mass2 = mass2;
        }
        tooltip("Mass of second bob (kg)");

        auto angle1_deg = static_cast<float>(rad2deg(state.config.physics.initial_angle1));
        if (ImGui::SliderFloat("Initial Angle 1", &angle1_deg, -180.0f, 180.0f)) {
            state.config.physics.initial_angle1 = deg2rad(angle1_deg);
        }
        tooltip("Starting angle of first arm (degrees from vertical)");

        auto angle2_deg = static_cast<float>(rad2deg(state.config.physics.initial_angle2));
        if (ImGui::SliderFloat("Initial Angle 2", &angle2_deg, -180.0f, 180.0f)) {
            state.config.physics.initial_angle2 = deg2rad(angle2_deg);
        }
        tooltip("Starting angle of second arm (degrees from vertical)");

        auto vel1 = static_cast<float>(state.config.physics.initial_velocity1);
        if (ImGui::SliderFloat("Initial Vel 1", &vel1, -10.0f, 10.0f)) {
            state.config.physics.initial_velocity1 = vel1;
        }
        tooltip("Starting angular velocity of first arm (rad/s)");

        auto vel2 = static_cast<float>(state.config.physics.initial_velocity2);
        if (ImGui::SliderFloat("Initial Vel 2", &vel2, -10.0f, 10.0f)) {
            state.config.physics.initial_velocity2 = vel2;
        }
        tooltip("Starting angular velocity of second arm (rad/s)");
    }
}

void drawSimulationSection(AppState& state) {
    if (ImGui::CollapsingHeader("Simulation")) {
        auto variation_deg = static_cast<float>(rad2deg(state.config.simulation.angle_variation));
        if (ImGui::SliderFloat("Angle Variation", &variation_deg, 0.001f, 5.0f, "%.3f deg")) {
            state.config.simulation.angle_variation = deg2rad(variation_deg);
        }
        tooltip("Total spread of initial angles across all pendulums");

        auto duration = static_cast<float>(state.config.simulation.duration_seconds);
        if (ImGui::SliderFloat("Duration (s)", &duration, 1.0f, 60.0f)) {
            state.config.simulation.duration_seconds = duration;
            state.updateFrameDuration(); // Update cached frame_duration
        }
        tooltip("Total simulation time in physical seconds");

        if (ImGui::SliderInt("Total Frames", &state.config.simulation.total_frames, 60, 3600)) {
            state.updateFrameDuration(); // Update cached frame_duration
        }
        tooltip("Number of frames to render");

        // Physics quality settings
        const char* quality_names[] = {"Low", "Medium", "High", "Ultra", "Custom"};
        int quality_idx = static_cast<int>(state.config.simulation.physics_quality);
        if (ImGui::Combo("Physics Quality", &quality_idx, quality_names, 5)) {
            state.config.simulation.physics_quality = static_cast<PhysicsQuality>(quality_idx);
            if (quality_idx < 4) { // Not Custom
                state.config.simulation.max_dt =
                    qualityToMaxDt(state.config.simulation.physics_quality);
            }
        }
        tooltip("Low=20ms, Medium=12ms, High=7ms, Ultra=3ms max timestep");

        // Show max_dt slider (editable, sets quality to Custom)
        auto max_dt_ms = static_cast<float>(state.config.simulation.max_dt * 1000.0);
        if (ImGui::SliderFloat("Max dt (ms)", &max_dt_ms, 1.0f, 30.0f, "%.1f")) {
            state.config.simulation.max_dt = max_dt_ms / 1000.0;
            state.config.simulation.physics_quality = PhysicsQuality::Custom;
        }
        tooltip("Maximum physics timestep (lower = more accurate)");

        // Display computed values
        ImGui::Text("Substeps: %d, dt = %.4f ms", state.config.simulation.substeps(),
                    state.config.simulation.dt() * 1000.0);
    }
}

void drawColorSection(AppState& state) {
    if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool color_changed = false;

        // Color ramp visualization
        float ramp_width = ImGui::GetContentRegionAvail().x;
        drawColorRamp(state.config.color, ramp_width, 24.0f);

        // Scheme selector (this determines which presets are shown)
        const char* schemes[] = {
            "Spectrum",   "Rainbow",    "Heat",           "Cool",        "Monochrome",
            "Plasma",     "Viridis",    "Inferno",        "Sunset",      "Ember",
            "DeepOcean",  "NeonViolet", "Aurora",         "Pearl",       "TurboPop",
            "Nebula",     "Blackbody",  "Magma",          "Cyberpunk",   "Biolume",
            "Gold",       "RoseGold",   "Twilight",       "ForestFire",  "AbyssalGlow",
            "MoltenCore", "Iridescent", "StellarNursery", "WhiskeyAmber"};

        int scheme_idx = static_cast<int>(state.config.color.scheme);
        if (ImGui::Combo("Scheme", &scheme_idx, schemes, 29)) {
            state.config.color.scheme = static_cast<ColorScheme>(scheme_idx);
            color_changed = true;
            // Clear loaded preset when scheme changes (it's no longer valid)
            state.preset_ui.loaded_color_preset.clear();
        }
        tooltip("Color mapping style");

        // Preset selector - filtered by current scheme
        auto preset_names = state.presets.getColorNamesForScheme(state.config.color.scheme);

        // Build display label
        std::string preset_label;
        if (state.preset_ui.loaded_color_preset.empty()) {
            preset_label = "(Custom)";
        } else if (state.preset_ui.isColorModified(state.config.color)) {
            preset_label = state.preset_ui.loaded_color_preset + " (modified)";
        } else {
            preset_label = state.preset_ui.loaded_color_preset;
        }

        if (ImGui::BeginCombo("Preset", preset_label.c_str())) {
            // Option to clear preset
            if (ImGui::Selectable("(Custom)", state.preset_ui.loaded_color_preset.empty())) {
                state.preset_ui.loaded_color_preset.clear();
            }

            // Show presets for current scheme
            for (auto const& name : preset_names) {
                bool is_selected = (state.preset_ui.loaded_color_preset == name);
                if (ImGui::Selectable(name.c_str(), is_selected)) {
                    if (auto preset = state.presets.getColor(name)) {
                        state.config.color = *preset;
                        state.preset_ui.loaded_color_preset = name;
                        state.preset_ui.loaded_color_values = *preset;
                        color_changed = true;
                    }
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (preset_names.empty()) {
            tooltip("No presets for this scheme yet");
        } else {
            tooltip("Load a preset for this scheme");
        }

        // Start/End sliders
        auto color_start = static_cast<float>(state.config.color.start);
        if (ImGui::SliderFloat("Start", &color_start, 0.0f, 1.0f, "%.2f")) {
            state.config.color.start = color_start;
            color_changed = true;
        }
        tooltip("Start position in color range [0-1]");

        auto color_end = static_cast<float>(state.config.color.end);
        if (ImGui::SliderFloat("End", &color_end, 0.0f, 1.0f, "%.2f")) {
            state.config.color.end = color_end;
            color_changed = true;
        }
        tooltip("End position in color range [0-1]");

        // Preset management buttons
        bool has_loaded_preset = !state.preset_ui.loaded_color_preset.empty();
        bool is_modified = state.preset_ui.isColorModified(state.config.color);

        // Save button (overwrite current preset) - only if loaded and modified
        if (has_loaded_preset && is_modified) {
            if (ImGui::Button("Save")) {
                state.presets.setColor(state.preset_ui.loaded_color_preset, state.config.color);
                state.presets.save();
                state.preset_ui.loaded_color_values = state.config.color;
            }
            tooltip("Overwrite the current preset");
            ImGui::SameLine();
        }

        // Save As button
        if (ImGui::Button("Save As...")) {
            state.preset_ui.show_color_save_popup = true;
            state.preset_ui.new_color_preset_name[0] = '\0';
        }
        tooltip("Save as a new preset");

        // Delete button - only if a preset is loaded
        if (has_loaded_preset) {
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                state.preset_ui.show_color_delete_confirm = true;
            }
            tooltip("Delete the current preset");
        }

        // Save As popup
        if (state.preset_ui.show_color_save_popup) {
            ImGui::OpenPopup("Save Color Preset");
        }
        if (ImGui::BeginPopupModal("Save Color Preset", &state.preset_ui.show_color_save_popup,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter preset name:");
            ImGui::InputText("##color_preset_name", state.preset_ui.new_color_preset_name,
                             sizeof(state.preset_ui.new_color_preset_name));

            if (ImGui::Button("Save", ImVec2(120, 0))) {
                if (state.preset_ui.new_color_preset_name[0] != '\0') {
                    state.presets.setColor(state.preset_ui.new_color_preset_name,
                                           state.config.color);
                    state.presets.save();
                    state.preset_ui.loaded_color_preset = state.preset_ui.new_color_preset_name;
                    state.preset_ui.loaded_color_values = state.config.color;
                    state.preset_ui.show_color_save_popup = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                state.preset_ui.show_color_save_popup = false;
            }
            ImGui::EndPopup();
        }

        // Delete confirmation popup
        if (state.preset_ui.show_color_delete_confirm) {
            ImGui::OpenPopup("Delete Color Preset?");
        }
        if (ImGui::BeginPopupModal("Delete Color Preset?",
                                   &state.preset_ui.show_color_delete_confirm,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete preset \"%s\"?", state.preset_ui.loaded_color_preset.c_str());
            ImGui::Text("This cannot be undone.");

            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                state.presets.deleteColor(state.preset_ui.loaded_color_preset);
                state.presets.save();
                state.preset_ui.loaded_color_preset.clear();
                state.preset_ui.show_color_delete_confirm = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                state.preset_ui.show_color_delete_confirm = false;
            }
            ImGui::EndPopup();
        }

        // Apply color changes to pendulums
        if (color_changed && state.running) {
            ColorSchemeGenerator color_gen(state.config.color);
            int n = static_cast<int>(state.colors.size());
            for (int i = 0; i < n; ++i) {
                state.colors[i] = color_gen.getColorForIndex(i, n);
            }
            state.needs_redraw = true;
        }
    }
}

void drawPostProcessSection(AppState& state) {
    if (ImGui::CollapsingHeader("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool pp_changed = false;

        // Preset selector with modified indicator
        auto preset_names = state.presets.getPostProcessNames();

        std::string preset_label;
        if (state.preset_ui.loaded_pp_preset.empty()) {
            preset_label = "(Custom)";
        } else if (state.preset_ui.isPPModified(state.config.post_process)) {
            preset_label = state.preset_ui.loaded_pp_preset + " (modified)";
        } else {
            preset_label = state.preset_ui.loaded_pp_preset;
        }

        if (ImGui::BeginCombo("Preset##pp", preset_label.c_str())) {
            // Option to clear preset
            if (ImGui::Selectable("(Custom)", state.preset_ui.loaded_pp_preset.empty())) {
                state.preset_ui.loaded_pp_preset.clear();
            }

            for (auto const& name : preset_names) {
                bool is_selected = (state.preset_ui.loaded_pp_preset == name);
                if (ImGui::Selectable(name.c_str(), is_selected)) {
                    if (auto preset = state.presets.getPostProcess(name)) {
                        state.config.post_process = *preset;
                        state.preset_ui.loaded_pp_preset = name;
                        state.preset_ui.loaded_pp_values = *preset;
                        pp_changed = true;
                    }
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        tooltip("Load a post-processing preset");

        // Normalization mode
        const char* norm_names[] = {"Per-Frame (auto)", "By Count (consistent)"};
        int current_norm = static_cast<int>(state.config.post_process.normalization);
        if (ImGui::Combo("Normalization", &current_norm, norm_names, 2)) {
            state.config.post_process.normalization = static_cast<NormalizationMode>(current_norm);
            pp_changed = true;
        }
        tooltip("Per-Frame: auto-adjusts brightness\n"
                "By Count: consistent brightness regardless of pendulum count");

        const char* tone_map_names[] = {"None (Linear)", "Reinhard", "Reinhard Extended",
                                        "ACES Filmic", "Logarithmic"};
        int current_tone_map = static_cast<int>(state.config.post_process.tone_map);
        if (ImGui::Combo("Tone Mapping", &current_tone_map, tone_map_names, 5)) {
            state.config.post_process.tone_map = static_cast<ToneMapOperator>(current_tone_map);
            pp_changed = true;
        }
        tooltip("HDR to SDR tone mapping curve");

        if (state.config.post_process.tone_map == ToneMapOperator::ReinhardExtended ||
            state.config.post_process.tone_map == ToneMapOperator::Logarithmic) {
            auto white_point = static_cast<float>(state.config.post_process.reinhard_white_point);
            if (ImGui::SliderFloat("White Point", &white_point, 0.5f, 10.0f)) {
                state.config.post_process.reinhard_white_point = white_point;
                pp_changed = true;
            }
            tooltip("Input value that maps to pure white");
        }

        auto exposure = static_cast<float>(state.config.post_process.exposure);
        if (ImGui::SliderFloat("Exposure", &exposure, -3.0f, 10.0f, "%.2f stops")) {
            state.config.post_process.exposure = exposure;
            pp_changed = true;
        }
        tooltip("Brightness in stops (0 = no change, +1 = 2x brighter)");

        auto contrast = static_cast<float>(state.config.post_process.contrast);
        if (ImGui::SliderFloat("Contrast", &contrast, 0.5f, 2.0f)) {
            state.config.post_process.contrast = contrast;
            pp_changed = true;
        }
        tooltip("Contrast adjustment (1.0 = no change)");

        auto gamma = static_cast<float>(state.config.post_process.gamma);
        if (ImGui::SliderFloat("Gamma", &gamma, 1.0f, 3.0f)) {
            state.config.post_process.gamma = gamma;
            pp_changed = true;
        }
        tooltip("Display gamma (2.2 = sRGB standard)");

        // Preset management buttons
        bool has_loaded_preset = !state.preset_ui.loaded_pp_preset.empty();
        bool is_modified = state.preset_ui.isPPModified(state.config.post_process);

        // Save button (overwrite current preset) - only if loaded and modified
        if (has_loaded_preset && is_modified) {
            if (ImGui::Button("Save##pp")) {
                state.presets.setPostProcess(state.preset_ui.loaded_pp_preset,
                                             state.config.post_process);
                state.presets.save();
                state.preset_ui.loaded_pp_values = state.config.post_process;
            }
            tooltip("Overwrite the current preset");
            ImGui::SameLine();
        }

        // Save As button
        if (ImGui::Button("Save As...##pp")) {
            state.preset_ui.show_pp_save_popup = true;
            state.preset_ui.new_pp_preset_name[0] = '\0';
        }
        tooltip("Save as a new preset");

        // Delete button - only if a preset is loaded
        if (has_loaded_preset) {
            ImGui::SameLine();
            if (ImGui::Button("Delete##pp")) {
                state.preset_ui.show_pp_delete_confirm = true;
            }
            tooltip("Delete the current preset");
        }

        // Save As popup
        if (state.preset_ui.show_pp_save_popup) {
            ImGui::OpenPopup("Save Post-Process Preset");
        }
        if (ImGui::BeginPopupModal("Save Post-Process Preset", &state.preset_ui.show_pp_save_popup,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter preset name:");
            ImGui::InputText("##pp_preset_name", state.preset_ui.new_pp_preset_name,
                             sizeof(state.preset_ui.new_pp_preset_name));

            if (ImGui::Button("Save", ImVec2(120, 0))) {
                if (state.preset_ui.new_pp_preset_name[0] != '\0') {
                    state.presets.setPostProcess(state.preset_ui.new_pp_preset_name,
                                                 state.config.post_process);
                    state.presets.save();
                    state.preset_ui.loaded_pp_preset = state.preset_ui.new_pp_preset_name;
                    state.preset_ui.loaded_pp_values = state.config.post_process;
                    state.preset_ui.show_pp_save_popup = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                state.preset_ui.show_pp_save_popup = false;
            }
            ImGui::EndPopup();
        }

        // Delete confirmation popup
        if (state.preset_ui.show_pp_delete_confirm) {
            ImGui::OpenPopup("Delete Post-Process Preset?");
        }
        if (ImGui::BeginPopupModal("Delete Post-Process Preset?",
                                   &state.preset_ui.show_pp_delete_confirm,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete preset \"%s\"?", state.preset_ui.loaded_pp_preset.c_str());
            ImGui::Text("This cannot be undone.");

            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                state.presets.deletePostProcess(state.preset_ui.loaded_pp_preset);
                state.presets.save();
                state.preset_ui.loaded_pp_preset.clear();
                state.preset_ui.show_pp_delete_confirm = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                state.preset_ui.show_pp_delete_confirm = false;
            }
            ImGui::EndPopup();
        }

        if (pp_changed && state.running) {
            state.needs_redraw = true;
        }
    }
}

// Recompute all metrics from frame history using current parameters
void recomputeMetrics(AppState& state) {
    if (state.frame_history.empty())
        return;

    // Update per-metric configs on collector
    state.metrics_collector.setAllMetricConfigs(state.config.metric_configs);

    // Reset metrics collector
    state.metrics_collector.reset();

    // Recompute metrics for all frames in history
    for (size_t frame = 0; frame < state.frame_history.size(); ++frame) {
        state.metrics_collector.beginFrame(static_cast<int>(frame));
        state.metrics_collector.updateFromStates(state.frame_history[frame]);
        state.metrics_collector.endFrame();
    }

    // Re-detect boom with params from config targets
    optimize::FrameDetectionParams boom_params;
    for (auto const& tc : state.config.targets) {
        if (tc.name == "boom" && tc.type == "frame") {
            auto target = optimize::targetConfigToPredictionTarget(
                tc.name, tc.type, tc.metric, tc.method, tc.offset_seconds,
                tc.peak_percent_threshold, tc.min_peak_prominence, tc.smoothing_window,
                tc.crossing_threshold, tc.crossing_confirmation, tc.weights);
            boom_params = target.frameParams();
            break;
        }
    }

    metrics::BoomDetection boom;
    if (!boom_params.metric_name.empty()) {
        boom = metrics::findBoomFrame(state.metrics_collector, state.frame_duration, boom_params);
    } else {
        boom.frame = -1; // No boom target configured
    }

    if (boom.frame >= 0) {
        state.boom_frame = boom.frame;
        state.boom_causticness = boom.metric_value;

        // Update event detector for analyzers
        double variance = state.metrics_collector.getVariance();
        metrics::forceBoomEvent(state.event_detector, boom, variance);
    } else {
        state.boom_frame.reset();
        state.boom_causticness = 0.0;
    }

    state.needs_metric_recompute = false;
}

// Helper: Get or create metric config with proper defaults
MetricConfig& getOrCreateMetricConfig(Config& config, std::string const& metric_name) {
    auto& configs = config.metric_configs;
    if (configs.find(metric_name) == configs.end()) {
        configs[metric_name] = createDefaultMetricConfig(metric_name);
    }
    return configs[metric_name];
}

// Draw metric parameters window - redesigned with clear separation of concerns
void drawMetricParametersWindow(AppState& state) {
    if (!state.show_metric_params_window)
        return;

    ImGui::SetNextWindowSize(ImVec2(420, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Metric Parameters", &state.show_metric_params_window)) {
        ImGui::End();
        return;
    }

    auto const& all_metrics = getAllBoomMetrics();

    // Initialize editing_metric if empty
    if (state.editing_metric.empty()) {
        // Try to get metric from boom target, otherwise use first available metric
        std::string default_metric = all_metrics.empty() ? "" : all_metrics[0];
        for (auto const& tc : state.config.targets) {
            if (tc.name == "boom" && !tc.metric.empty()) {
                default_metric = tc.metric;
                break;
            }
        }
        state.editing_metric =
            state.config.boom_metric.empty() ? default_metric : state.config.boom_metric;
    }

    // ========== SECTION 1: Metric selector dropdown ==========
    if (ImGui::CollapsingHeader("Edit Metric Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Build combo items
        std::string combo_preview = state.editing_metric;
        if (ImGui::BeginCombo("Edit Metric", combo_preview.c_str())) {
            for (size_t i = 0; i < all_metrics.size(); ++i) {
                bool is_selected = (all_metrics[i] == state.editing_metric);
                std::string label = all_metrics[i];
                if (all_metrics[i] == state.config.boom_metric) {
                    label += " [PRIMARY]";
                }
                if (state.boom_metrics[all_metrics[i]].enabled) {
                    label += " *";
                }
                if (ImGui::Selectable(label.c_str(), is_selected)) {
                    state.editing_metric = all_metrics[i];
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select which metric's parameters to edit");
        }

        ImGui::Spacing();

        // Get config for the metric being edited
        auto& editing_config = getOrCreateMetricConfig(state.config, state.editing_metric);

        bool params_changed = false;

        // Metric-specific parameters
        ImGui::Text("Metric Parameters:");
        ImGui::Indent();

        std::visit(
            [&params_changed](auto& p) {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, SectorMetricParams> ||
                              std::is_same_v<T, CVSectorMetricParams>) {
                    if (ImGui::SliderInt("Min Sectors", &p.min_sectors, 4, 32))
                        params_changed = true;
                    if (ImGui::SliderInt("Max Sectors", &p.max_sectors, 8, 144))
                        params_changed = true;
                    if (ImGui::SliderInt("Target/Sector", &p.target_per_sector, 10, 500))
                        params_changed = true;
                }
                if constexpr (std::is_same_v<T, CVSectorMetricParams>) {
                    auto cv = static_cast<float>(p.cv_normalization);
                    if (ImGui::SliderFloat("CV Norm", &cv, 0.5f, 3.0f, "%.2f")) {
                        p.cv_normalization = cv;
                        params_changed = true;
                    }
                }
                if constexpr (std::is_same_v<T, LocalCoherenceMetricParams>) {
                    auto r = static_cast<float>(p.max_radius);
                    if (ImGui::SliderFloat("Max Radius", &r, 0.5f, 4.0f)) {
                        p.max_radius = r;
                        params_changed = true;
                    }
                    auto s = static_cast<float>(p.min_spread_threshold);
                    if (ImGui::SliderFloat("Min Spread", &s, 0.01f, 0.2f, "%.3f")) {
                        p.min_spread_threshold = s;
                        params_changed = true;
                    }
                    auto lb = static_cast<float>(p.log_inverse_baseline);
                    if (ImGui::SliderFloat("Log Baseline", &lb, 0.1f, 2.0f)) {
                        p.log_inverse_baseline = lb;
                        params_changed = true;
                    }
                    auto ld = static_cast<float>(p.log_inverse_divisor);
                    if (ImGui::SliderFloat("Log Divisor", &ld, 0.5f, 3.0f)) {
                        p.log_inverse_divisor = ld;
                        params_changed = true;
                    }
                }
            },
            editing_config.params);

        ImGui::Unindent();

        // Note: params_changed is set by metric-specific parameter edits above
        // The changes are applied directly to editing_config.params via std::visit
        (void)params_changed; // Suppress unused variable warning
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ========== SECTION: Prediction Targets Configuration ==========
    if (ImGui::CollapsingHeader("Prediction Targets")) {
        ImGui::TextDisabled("Configure multi-target predictions (boom, chaos, quality)");
        ImGui::Spacing();

        // Show currently configured targets
        if (state.config.targets.empty()) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No explicit targets configured.");
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "Using defaults: boom, chaos, boom_quality");
            ImGui::Spacing();

            // Button to add default targets to config
            if (ImGui::Button("Add Default Targets")) {
                // Add boom target
                TargetConfig boom;
                boom.name = "boom";
                boom.type = "frame";
                boom.metric = state.config.boom_metric;
                boom.method = "max_value";
                state.config.targets.push_back(boom);

                // Add chaos target
                TargetConfig chaos;
                chaos.name = "chaos";
                chaos.type = "frame";
                chaos.metric = "variance";
                chaos.method = "threshold_crossing";
                chaos.crossing_threshold = 0.8;
                chaos.crossing_confirmation = 10;
                state.config.targets.push_back(chaos);

                // Add boom_quality target
                TargetConfig quality;
                quality.name = "boom_quality";
                quality.type = "score";
                quality.metric = state.config.boom_metric;
                quality.method = "composite";
                state.config.targets.push_back(quality);
            }
        } else {
            // Show each configured target
            int target_to_remove = -1;
            for (size_t i = 0; i < state.config.targets.size(); ++i) {
                auto& tc = state.config.targets[i];
                ImGui::PushID(static_cast<int>(i));

                // Target header with type indicator
                ImVec4 type_color = (tc.type == "score")
                                        ? ImVec4(0.4f, 0.8f, 0.4f, 1.0f)  // Green for score
                                        : ImVec4(0.8f, 0.6f, 0.2f, 1.0f); // Orange for frame
                ImGui::TextColored(type_color, "[%s]", tc.type.c_str());
                ImGui::SameLine();
                ImGui::Text("%s", tc.name.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 60);
                if (ImGui::SmallButton("Remove")) {
                    target_to_remove = static_cast<int>(i);
                }

                ImGui::Indent();

                // Metric selection
                if (ImGui::BeginCombo("Metric", tc.metric.c_str())) {
                    for (auto const& m : all_metrics) {
                        if (ImGui::Selectable(m.c_str(), m == tc.metric)) {
                            tc.metric = m;
                        }
                    }
                    ImGui::EndCombo();
                }

                // Method selection (different for frame vs score)
                if (tc.type == "frame") {
                    const char* frame_methods[] = {"max_value", "first_peak_percent",
                                                   "derivative_peak", "threshold_crossing",
                                                   "second_derivative_peak"};
                    int method_idx = 0;
                    for (int j = 0; j < 5; ++j) {
                        if (tc.method == frame_methods[j]) {
                            method_idx = j;
                            break;
                        }
                    }
                    if (ImGui::Combo("Method", &method_idx, frame_methods, 5)) {
                        tc.method = frame_methods[method_idx];
                    }

                    // Method-specific parameters
                    float offset = static_cast<float>(tc.offset_seconds);
                    if (ImGui::SliderFloat("Offset", &offset, -0.5f, 1.0f, "%.2fs")) {
                        tc.offset_seconds = offset;
                    }

                    if (tc.method == "threshold_crossing") {
                        float thresh = static_cast<float>(tc.crossing_threshold);
                        if (ImGui::SliderFloat("Threshold", &thresh, 0.1f, 0.9f)) {
                            tc.crossing_threshold = thresh;
                        }
                        if (ImGui::SliderInt("Confirm", &tc.crossing_confirmation, 1, 20)) {}
                    }
                } else {
                    // Score methods
                    const char* score_methods[] = {"peak_clarity", "post_boom_sustain",
                                                   "composite"};
                    int method_idx = 0;
                    for (int j = 0; j < 3; ++j) {
                        if (tc.method == score_methods[j]) {
                            method_idx = j;
                            break;
                        }
                    }
                    if (ImGui::Combo("Method", &method_idx, score_methods, 3)) {
                        tc.method = score_methods[method_idx];
                    }
                }

                ImGui::Unindent();
                ImGui::Spacing();
                ImGui::PopID();
            }

            // Remove target if requested
            if (target_to_remove >= 0) {
                state.config.targets.erase(state.config.targets.begin() + target_to_remove);
            }

            ImGui::Spacing();

            // Add new target buttons
            if (ImGui::Button("+ Frame Target")) {
                TargetConfig tc;
                tc.name = "custom_frame";
                tc.type = "frame";
                tc.metric = state.config.boom_metric;
                tc.method = "max_value";
                state.config.targets.push_back(tc);
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Score Target")) {
                TargetConfig tc;
                tc.name = "custom_score";
                tc.type = "score";
                tc.metric = state.config.boom_metric;
                tc.method = "composite";
                state.config.targets.push_back(tc);
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear All")) {
                state.config.targets.clear();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ========== SECTION 2: Boom line configuration ==========
    if (ImGui::CollapsingHeader("Boom Line Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Primary metric selector (radio buttons in a compact layout)
        ImGui::Text("Primary Metric:");
        ImGui::Indent();
        int cols = 4;
        for (size_t i = 0; i < all_metrics.size(); ++i) {
            auto const& name = all_metrics[i];
            bool is_primary = (name == state.config.boom_metric);
            std::string short_name = shortenMetricName(name);

            // Use unique ID suffix to avoid conflicts with checkboxes below
            std::string radio_label = short_name + "##primary_" + name;
            if (ImGui::RadioButton(radio_label.c_str(), is_primary)) {
                state.config.boom_metric = name;
                state.boom_metrics[name].enabled = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", name.c_str());
            }
            if ((i + 1) % cols != 0 && i < all_metrics.size() - 1) {
                ImGui::SameLine();
            }
        }
        ImGui::Unindent();

        ImGui::Spacing();

        // Show checkboxes for which boom lines to display
        ImGui::Text("Show Boom Lines:");
        ImGui::Indent();
        for (size_t i = 0; i < all_metrics.size(); ++i) {
            auto const& name = all_metrics[i];
            auto& bs = state.boom_metrics[name];
            std::string short_name = shortenMetricName(name);
            bool is_primary = (name == state.config.boom_metric);

            std::string label = short_name;
            if (is_primary)
                label += "*";
            if (bs.boom_frame >= 0) {
                label += " [" + std::to_string(bs.boom_frame) + "]";
            }
            // Use unique ID suffix to avoid conflicts with radio buttons above
            label += "##show_" + name;

            ImGui::Checkbox(label.c_str(), &bs.enabled);
            if (ImGui::IsItemHovered()) {
                std::string tip = name;
                if (bs.boom_frame >= 0) {
                    tip += "\nBoom @ frame " + std::to_string(bs.boom_frame);
                }
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if ((i + 1) % cols != 0 && i < all_metrics.size() - 1) {
                ImGui::SameLine();
            }
        }
        ImGui::Unindent();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ========== SECTION 3: Action buttons ==========
    if (ImGui::Button("Detect All Booms", ImVec2(140, 0))) {
        if (!state.frame_history.empty()) {
            updateAllBoomDetections(state);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Run boom detection for all enabled metrics");
    }

    ImGui::SameLine();
    if (ImGui::Button("Recompute Metrics", ImVec2(140, 0))) {
        state.needs_metric_recompute = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Recompute all metrics from history\n(needed after changing metric parameters)");
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(60, 0))) {
        state.config.metric_configs.clear();
        // Keep the boom_metric from targets if configured, otherwise clear
        std::string default_metric;
        for (auto const& tc : state.config.targets) {
            if (tc.name == "boom" && !tc.metric.empty()) {
                default_metric = tc.metric;
                break;
            }
        }
        if (default_metric.empty() && !all_metrics.empty()) {
            default_metric = all_metrics[0];
        }
        state.config.boom_metric = default_metric;
        state.editing_metric = default_metric;
        state.initBoomMetrics();
    }

    if (state.needs_metric_recompute) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Recomputing...");
    }

    ImGui::End();
}

void drawControlPanel(AppState& state, GLRenderer& renderer) {
    ImGui::Begin("Controls");

    // File path input for loading simulation data
    static char load_path[512] = "";
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##loadpath", "Path to simulation_data.bin", load_path,
                             sizeof(load_path));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        if (strlen(load_path) > 0) {
            loadSimulationData(state, renderer, load_path);
        }
    }
    tooltip("Load saved simulation data for replay");

    ImGui::Separator();

    // Simulation control buttons with keyboard hints
    if (!state.running) {
        if (ImGui::Button("Start [Space]")) {
            initSimulation(state, renderer);
        }
        ImGui::SameLine();
        if (ImGui::Button("Randomize [R]")) {
            randomizePhysics(state);
        }
        tooltip("Randomize physics parameters");
    } else if (state.replay_mode) {
        // Replay mode controls
        if (state.replay_playing) {
            if (ImGui::Button("Pause [Space]")) {
                stopReplay(state);
            }
        } else {
            if (ImGui::Button("Play [Space]")) {
                startReplay(state);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart")) {
            state.display_frame = 0;
            state.scrubbing = true;
            stopReplay(state);
            renderFrameFromHistory(state, renderer, 0);
        }
        ImGui::SameLine();
        if (ImGui::Button("New Sim [R]")) {
            state.replay_mode = false;
            state.loaded_data.reset();
            initSimulation(state, renderer);
        }
    } else {
        // Normal simulation mode controls
        if (ImGui::Button(state.paused ? "Resume [Space]" : "Pause [Space]")) {
            state.paused = !state.paused;
        }
        ImGui::SameLine();
        if (state.paused) {
            if (ImGui::Button("Step [.]")) {
                state.paused = false;
                stepSimulation(state, renderer);
                state.paused = true;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Restart [R]")) {
            initSimulation(state, renderer);
        }
    }

    // Quick action buttons
    if (state.running) {
        if (state.boom_frame.has_value()) {
            ImGui::SameLine();
            if (ImGui::Button("Boom [B]")) {
                int target = *state.boom_frame;
                if (target < static_cast<int>(state.frame_history.size())) {
                    state.paused = true;
                    state.scrubbing = true;
                    state.display_frame = target;
                    renderFrameFromHistory(state, renderer, target);
                }
            }
            tooltip("Jump to boom frame");
        }
    }

    // Status display
    ImGui::Separator();
    if (state.replay_mode) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "REPLAY MODE");
        ImGui::Text("Frame: %d / %d", state.display_frame,
                    static_cast<int>(state.frame_history.size()) - 1);
        double time_seconds = state.display_frame * state.frame_duration;
        ImGui::Text("Time: %.2fs / %.2fs", time_seconds, state.config.simulation.duration_seconds);
        if (!state.loaded_data_path.empty()) {
            // Show just the filename
            std::filesystem::path p(state.loaded_data_path);
            ImGui::TextDisabled("File: %s", p.filename().string().c_str());
        }
    } else {
        ImGui::Text("Frame: %d", state.current_frame);
        ImGui::Text("FPS: %.1f", state.fps);
        ImGui::Text("Sim: %.2f ms", state.sim_time_ms);
        ImGui::Text("Render: %.2f ms", state.render_time_ms);
    }

    // Event status
    if (state.boom_frame.has_value()) {
        double boom_seconds = *state.boom_frame * state.frame_duration;
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Boom: frame %d (%.2fs)",
                           *state.boom_frame, boom_seconds);
    }
    if (state.chaos_frame.has_value()) {
        double chaos_seconds = *state.chaos_frame * state.frame_duration;
        ImGui::TextColored(ImVec4(0.3f, 0.5f, 1.0f, 1.0f), "Chaos: frame %d (%.2fs)",
                           *state.chaos_frame, chaos_seconds);
    }

    ImGui::Separator();

    // Draw each section
    drawPreviewSection(state);
    drawPhysicsSection(state);
    drawSimulationSection(state);
    drawColorSection(state);
    drawPostProcessSection(state);

    drawExportPanel(state);

    ImGui::End();
}

void drawTimeline(AppState& state, GLRenderer& renderer) {
    if (!state.running) {
        ImGui::Text("Start simulation to enable timeline");
        return;
    }

    int history_size = static_cast<int>(state.frame_history.size());
    if (history_size == 0) {
        ImGui::Text("Recording frames...");
        return;
    }

    ImGui::Text("Timeline");
    if (state.replay_mode) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "(Replay)");
    }
    ImGui::Separator();

    // Playback controls
    bool is_playing = state.replay_mode ? state.replay_playing : !state.paused;
    if (ImGui::Button(is_playing ? "Pause" : "Play")) {
        if (state.replay_mode) {
            if (state.replay_playing) {
                stopReplay(state);
            } else {
                startReplay(state);
            }
        } else {
            state.paused = !state.paused;
            if (!state.paused) {
                // Resume from current display frame
                state.scrubbing = false;
            }
        }
    }
    ImGui::SameLine();

    // Step backward
    if (ImGui::Button("<<") && state.display_frame > 0) {
        if (state.replay_mode)
            stopReplay(state);
        state.paused = true;
        state.display_frame--;
        state.scrubbing = true;
        renderFrameFromHistory(state, renderer, state.display_frame);
    }
    ImGui::SameLine();

    // Step forward (only within history)
    if (ImGui::Button(">>") && state.display_frame < history_size - 1) {
        if (state.replay_mode)
            stopReplay(state);
        state.paused = true;
        state.display_frame++;
        state.scrubbing = true;
        renderFrameFromHistory(state, renderer, state.display_frame);
    }
    ImGui::SameLine();

    // Jump to end (or live in normal mode)
    const char* end_label = state.replay_mode ? "End" : "Live";
    if (ImGui::Button(end_label)) {
        if (state.replay_mode)
            stopReplay(state);
        state.scrubbing = false;
        state.display_frame = history_size - 1;
        if (!state.frame_history.empty()) {
            renderFrameFromHistory(state, renderer, state.display_frame);
        }
    }

    // Timeline slider
    int max_frame = std::max(1, history_size - 1);
    int slider_frame = std::min(state.display_frame, max_frame);

    if (ImGui::SliderInt("Frame", &slider_frame, 0, max_frame)) {
        if (state.replay_mode)
            stopReplay(state);
        state.paused = true;
        state.scrubbing = true;
        state.display_frame = slider_frame;
        renderFrameFromHistory(state, renderer, state.display_frame);
    }

    // Frame info with time display
    double display_time = state.display_frame * state.frame_duration;
    double total_time = max_frame * state.frame_duration;
    ImGui::Text("Frame: %d / %d  (%.2fs / %.2fs)", state.display_frame, history_size - 1,
                display_time, total_time);
    if (!state.replay_mode && history_size >= state.max_history_frames) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "History limit reached (%d frames)",
                           state.max_history_frames);
    }

    // Show boom/white markers on timeline
    if (state.boom_frame.has_value() && *state.boom_frame < history_size) {
        float boom_pos = static_cast<float>(*state.boom_frame) / max_frame;
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Boom at frame %d (%.1f%%)",
                           *state.boom_frame, boom_pos * 100);
    }
    if (state.chaos_frame.has_value() && *state.chaos_frame < history_size) {
        float chaos_pos = static_cast<float>(*state.chaos_frame) / max_frame;
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Chaos at frame %d (%.1f%%)",
                           *state.chaos_frame, chaos_pos * 100);
    }
}

int main(int argc, char* argv[]) {
    // Parse command-line config path (optional)
    std::string config_path = "config/gui.toml";
    if (argc > 1) {
        config_path = argv[1];
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL init error: " << SDL_GetError() << "\n";
        return 1;
    }

    // OpenGL settings
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // Create window
    SDL_Window* window = SDL_CreateWindow(
        "Double Pendulum - GUI", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!window) {
        std::cerr << "Window creation error: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // VSync

    // Detect DPI scale
    float dpi_scale = 1.0f;

    // Method 1: Compare window size to drawable size (works on macOS, some Linux)
    int window_w, window_h, drawable_w, drawable_h;
    SDL_GetWindowSize(window, &window_w, &window_h);
    SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
    float drawable_scale = static_cast<float>(drawable_w) / static_cast<float>(window_w);
    if (drawable_scale > 1.0f) {
        dpi_scale = drawable_scale;
    }

    // Method 2: Check environment variables (Linux desktop environments)
    if (dpi_scale <= 1.0f) {
        const char* gdk_scale = std::getenv("GDK_SCALE");
        const char* qt_scale = std::getenv("QT_SCALE_FACTOR");
        if (gdk_scale) {
            dpi_scale = std::stof(gdk_scale);
        } else if (qt_scale) {
            dpi_scale = std::stof(qt_scale);
        }
    }

    // Method 3: Check display DPI via SDL (if available)
    if (dpi_scale <= 1.0f) {
        float ddpi, hdpi, vdpi;
        if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0) {
            // Standard DPI is 96 on Linux, 72 on macOS
            dpi_scale = ddpi / 96.0f;
            if (dpi_scale < 1.0f)
                dpi_scale = 1.0f;
        }
    }

    // Method 4: Heuristic - if display is 4K or higher, assume scale 2
    if (dpi_scale <= 1.0f) {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
            if (mode.w >= 3840 || mode.h >= 2160) {
                dpi_scale = 2.0f;
            }
        }
    }

    std::cout << "DPI scale: " << dpi_scale << "\n";

    // Initialize ImGui and ImPlot
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    // Apply DPI scaling to ImGui style
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpi_scale);

    // Scale fonts for HiDPI
    io.FontGlobalScale = dpi_scale;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Initialize renderer
    GLRenderer renderer;
    if (!renderer.init(540, 540)) {
        std::cerr << "Failed to initialize GL renderer\n";
        return 1;
    }

    // Load config and presets
    AppState state;
    state.config = Config::load(config_path);
    state.initBoomMetrics(); // Initialize multi-metric boom state
    std::cout << "Loaded config from: " << config_path << "\n";
    state.presets = PresetLibrary::load("config/presets.toml");

    bool done = false;
    auto last_time = std::chrono::high_resolution_clock::now();

    while (!done) {
        // Event handling
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }

            // Keyboard shortcuts (only when not typing in a text field)
            if (event.type == SDL_KEYDOWN && !ImGui::GetIO().WantTextInput) {
                switch (event.key.keysym.sym) {
                case SDLK_SPACE:
                    if (!state.running) {
                        initSimulation(state, renderer);
                    } else if (state.replay_mode) {
                        // Toggle replay playback
                        if (state.replay_playing) {
                            stopReplay(state);
                        } else {
                            startReplay(state);
                        }
                    } else if (state.scrubbing && state.display_frame < state.current_frame) {
                        // Resume playback from scrubbed position using real-time replay
                        state.replay_mode = true; // Enter replay mode temporarily
                        startReplay(state);
                    } else {
                        state.paused = !state.paused;
                    }
                    break;
                case SDLK_r:
                    if (!state.running) {
                        randomizePhysics(state);
                    } else if (state.replay_mode) {
                        // Exit replay mode and start new simulation
                        state.replay_mode = false;
                        state.loaded_data.reset();
                        initSimulation(state, renderer);
                    } else {
                        initSimulation(state, renderer);
                    }
                    break;
                case SDLK_PERIOD:
                    if (state.running && state.paused && !state.replay_mode) {
                        state.paused = false;
                        stepSimulation(state, renderer);
                        state.paused = true;
                    }
                    break;
                case SDLK_b:
                    if (state.running && state.boom_frame.has_value()) {
                        int target = *state.boom_frame;
                        if (target < static_cast<int>(state.frame_history.size())) {
                            state.paused = true;
                            state.scrubbing = true;
                            state.display_frame = target;
                            renderFrameFromHistory(state, renderer, target);
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }

        // Calculate FPS
        auto now = std::chrono::high_resolution_clock::now();
        double frame_time = std::chrono::duration<double>(now - last_time).count();
        state.fps = 1.0 / frame_time;
        last_time = now;

        // Update simulation or replay
        if (state.running && !state.paused) {
            if (state.replay_mode) {
                // In replay mode, advance frames based on real time
                updateReplay(state, renderer);
            } else {
                // Normal simulation mode
                stepSimulation(state, renderer);
            }
        }

        // Re-render if needed (e.g., color/post-processing changed while paused)
        if (state.needs_redraw) {
            renderFrame(state, renderer);
            state.needs_redraw = false;

            // Update GPU metrics at the current display frame
            // This allows quality scores to update when changing colors/post-processing
            if (state.running && state.display_frame >= 0) {
                metrics::GPUMetricsBundle gpu_metrics;
                gpu_metrics.max_value = renderer.lastMax();
                gpu_metrics.brightness = renderer.lastBrightness();
                gpu_metrics.coverage = renderer.lastCoverage();
                state.metrics_collector.updateGPUMetricsAtFrame(gpu_metrics, state.display_frame);

                // Re-run causticness analyzer to update quality score
                if (state.boom_frame.has_value()) {
                    state.causticness_analyzer.analyze(state.metrics_collector,
                                                       state.event_detector);
                }
            }
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Draw UI windows
        drawControlPanel(state, renderer);

        // Preview window
        ImGui::Begin("Preview");
        ImVec2 preview_size(static_cast<float>(renderer.width()),
                            static_cast<float>(renderer.height()));
        // Flip UV vertically: OpenGL texture origin is bottom-left, ImGui expects top-left
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(renderer.getTextureID())),
                     preview_size, ImVec2(0, 0), ImVec2(1, 1));
        ImGui::End();

        // Quality Scores window (separate)
        // Primary score is based on causticness (angular distribution quality).
        // Boom timing is detected from max causticness, so causticness is the
        // single source of truth for visual quality assessment.
        ImGui::Begin("Quality");

        if (state.causticness_analyzer.hasResults()) {
            auto caustic_results = state.causticness_analyzer.toJSON();
            double caustic_score = state.causticness_analyzer.score();

            // Primary quality score (causticness-based)
            ImVec4 bar_color = caustic_score < 0.4f   ? ImVec4(0.8f, 0.2f, 0.2f, 1.0f)
                               : caustic_score < 0.7f ? ImVec4(0.8f, 0.8f, 0.2f, 1.0f)
                                                      : ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            ImGui::Text("Quality Score:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
            ImGui::ProgressBar(static_cast<float>(caustic_score), ImVec2(100, 0));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%.2f", caustic_score);

            ImGui::Separator();

            // Signal analysis details
            auto const& metrics = caustic_results["metrics"];
            double peak = metrics.value("peak_value", 0.0);
            int peak_frame = metrics.value("peak_frame", 0);
            double time_above = metrics.value("time_above_threshold", 0.0);

            // Use cached frame_duration for accurate time display
            double peak_seconds = static_cast<double>(peak_frame) * state.frame_duration;
            ImGui::Text("Peak: %.1f @ %.2fs", peak, peak_seconds);
            if (time_above > 0.0) {
                ImGui::Text("Time above threshold: %.1fs", time_above);
            }

            // Peak clarity (important for filtering)
            double peak_clarity = metrics.value("peak_clarity_score", 1.0);
            int competing_peaks = metrics.value("competing_peaks_count", 0);
            ImVec4 clarity_color = peak_clarity < 0.6f   ? ImVec4(0.8f, 0.2f, 0.2f, 1.0f)
                                   : peak_clarity < 0.8f ? ImVec4(0.8f, 0.8f, 0.2f, 1.0f)
                                                         : ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            ImGui::Text("Peak Clarity:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, clarity_color);
            ImGui::ProgressBar(static_cast<float>(peak_clarity), ImVec2(80, 0));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%.2f", peak_clarity);
            if (competing_peaks > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%d competing)", competing_peaks);
            }

            // Post-reference sustain
            double post_ref_area = metrics.value("post_ref_area_normalized", 0.0);
            ImGui::Text("Post-ref Sustain:");
            ImGui::SameLine();
            ImGui::ProgressBar(static_cast<float>(post_ref_area), ImVec2(80, 0));
            ImGui::SameLine();
            ImGui::Text("%.2f", post_ref_area);

            // Collapsible details
            if (ImGui::TreeNode("Details")) {
                int frames_above = metrics.value("frames_above_threshold", 0);
                double post_ref_avg = metrics.value("post_ref_average", 0.0);
                double post_ref_peak_val = metrics.value("post_ref_peak", 0.0);
                double max_competitor_ratio = metrics.value("max_competitor_ratio", 0.0);

                ImGui::Text("Frames above threshold: %d", frames_above);
                ImGui::Text("Post-ref average: %.2f", post_ref_avg);
                ImGui::Text("Post-ref peak: %.2f", post_ref_peak_val);
                if (competing_peaks > 0) {
                    ImGui::Text("Max competitor ratio: %.2f", max_competitor_ratio);
                }
                ImGui::TreePop();
            }

        } else {
            ImGui::TextDisabled("No quality data yet");
            ImGui::TextDisabled("Run simulation to analyze");
        }

        ImGui::End();

        // Detailed Analysis - full metric controls with derivatives
        ImGui::Begin("Detailed Analysis");

        // Plot mode selector and params button
        const char* plot_mode_names[] = {"Single Axis", "Multi-Axis", "Normalized"};
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("Scale", reinterpret_cast<int*>(&s_plot_mode), plot_mode_names, 3);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Single: All on one axis\nMulti: Y1=large, Y2=normalized, "
                              "Y3=medium\nNormalized: All scaled 0-1");
        }
        ImGui::SameLine();
        if (ImGui::Button("Params")) {
            state.show_metric_params_window = !state.show_metric_params_window;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open metric parameters window");
        }

        // Metric selector with derivative toggles
        auto metricWithDeriv = [](const char* label, bool* metric, bool* deriv, const char* id) {
            ImGui::Checkbox(label, metric);
            ImGui::SameLine(0, 2);
            ImGui::PushStyleColor(ImGuiCol_Text, *deriv ? ImVec4(0.4f, 0.8f, 0.4f, 1.0f)
                                                        : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 0));
            char btn_id[32];
            snprintf(btn_id, sizeof(btn_id), "d##%s", id);
            if (ImGui::SmallButton(btn_id)) {
                *deriv = !*deriv;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        };

        // Metric toggles organized by category from registry
        for (auto cat : {metrics::MetricCategory::Basic, metrics::MetricCategory::Caustic,
                         metrics::MetricCategory::LocalCoherence, metrics::MetricCategory::Velocity,
                         metrics::MetricCategory::GPU, metrics::MetricCategory::Other}) {
            auto cat_metrics = metrics::getByCategory(cat);
            if (cat_metrics.empty())
                continue;

            ImGui::Text("%s:", metrics::categoryName(cat));
            ImGui::SameLine();
            for (auto const* m : cat_metrics) {
                metricWithDeriv(m->short_name, state.detailed_flags.enabledPtr(m->name),
                                state.detailed_flags.derivPtr(m->name), m->name);
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }

        // Full metric graph
        ImVec2 metrics_graph_size = ImGui::GetContentRegionAvail();
        metrics_graph_size.y = std::max(150.0f, metrics_graph_size.y - 100.0f);
        drawMetricGraph(state, metrics_graph_size);

        // Current metric values table (updates with timeline scrubbing)
        ImGui::Separator();
        ImGui::Text("Current Values (frame %d):", state.display_frame);

        if (ImGui::BeginTable("##MetricValues", 4,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Metric");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Metric");
            ImGui::TableSetupColumn("Value");

            // Display all metrics from registry, 2 per row
            int col = 0;
            for (auto const& m : metrics::METRIC_REGISTRY) {
                if (col % 2 == 0) {
                    ImGui::TableNextRow();
                }
                double val = getMetricAtFrame(state.metrics_collector, m.name, state.display_frame);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.short_name);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", val);
                col++;
            }

            ImGui::EndTable();
        }

        ImGui::End();

        // Timeline
        ImGui::Begin("Timeline");
        drawTimeline(state, renderer);
        ImGui::End();

        // Metric Parameters window
        drawMetricParametersWindow(state);

        // Handle metric recomputation request
        if (state.needs_metric_recompute) {
            recomputeMetrics(state);
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        SDL_GetWindowSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Wait for export thread to finish if running
    if (state.export_state.export_thread.joinable()) {
        state.export_state.cancel_requested = true;
        state.export_state.export_thread.join();
    }

    // Cleanup
    renderer.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
