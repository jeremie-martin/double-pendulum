#include "color_scheme.h"
#include "config.h"
#include "gl_renderer.h"
#include "metrics/boom_analyzer.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/event_detector.h"
#include "metrics/metrics_collector.h"
#include "pendulum.h"
#include "preset_library.h"
#include "simulation.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <implot.h>
#include <iostream>
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
    double max_dt = 0.007;  // Computed from quality

    // Compute substeps for a given frame duration
    int substeps(double frame_dt) const {
        return std::max(1, static_cast<int>(std::ceil(frame_dt / max_dt)));
    }
};

// Plot scaling mode for multi-axis support
enum class PlotMode {
    SingleAxis,   // All metrics on one Y-axis (auto-fit)
    MultiAxis,    // Group by scale: Y1=large, Y2=normalized, Y3=medium
    Normalized    // All metrics scaled to 0-1
};

// Graph metric flags for multi-select (with derivative toggles)
struct MetricFlags {
    bool variance = true;
    bool variance_deriv = false;
    bool brightness = false;
    bool brightness_deriv = false;
    bool energy = false;
    bool energy_deriv = false;
    bool uniformity = false;  // Renamed from spread
    bool uniformity_deriv = false;
    bool contrast_stddev = false;
    bool contrast_stddev_deriv = false;
    bool contrast_range = false;
    bool contrast_range_deriv = false;
    bool edge_energy = false;
    bool edge_energy_deriv = false;
    bool color_variance = false;
    bool color_variance_deriv = false;
    bool coverage = false;
    bool coverage_deriv = false;
    bool causticness = false;
    bool causticness_deriv = false;
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
    std::string loaded_color_preset;      // Name of currently loaded preset (empty if none)
    ColorParams loaded_color_values;      // Values when preset was loaded (for detecting changes)
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
        if (loaded_color_preset.empty()) return false;
        return current.scheme != loaded_color_values.scheme ||
               std::abs(current.start - loaded_color_values.start) > 0.001 ||
               std::abs(current.end - loaded_color_values.end) > 0.001;
    }

    // Check if post-process values have been modified
    bool isPPModified(PostProcessParams const& current) const {
        if (loaded_pp_preset.empty()) return false;
        return current.tone_map != loaded_pp_values.tone_map ||
               std::abs(current.exposure - loaded_pp_values.exposure) > 0.001 ||
               std::abs(current.contrast - loaded_pp_values.contrast) > 0.001 ||
               std::abs(current.gamma - loaded_pp_values.gamma) > 0.001 ||
               std::abs(current.reinhard_white_point - loaded_pp_values.reinhard_white_point) > 0.001 ||
               current.normalization != loaded_pp_values.normalization;
    }
};

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
    metrics::BoomAnalyzer boom_analyzer;
    metrics::CausticnessAnalyzer causticness_analyzer;
    MetricFlags highlevel_flags;  // For High-Level Analysis window
    MetricFlags detailed_flags;   // For Detailed Analysis window

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
    double boom_variance = 0.0;
    std::optional<int> chaos_frame;  // Renamed from white_frame
    double chaos_variance = 0.0;

    // Timing
    double fps = 0.0;
    double sim_time_ms = 0.0;
    double render_time_ms = 0.0;

    // Export
    ExportState export_state;
};

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

    // Reset metrics system
    state.metrics_collector.reset();
    state.metrics_collector.registerStandardMetrics();
    state.metrics_collector.registerGPUMetrics();
    state.boom_analyzer.reset();
    state.causticness_analyzer.reset();

    // Setup event detector
    state.event_detector.clearCriteria();
    state.event_detector.addBoomCriteria(state.config.detection.boom_threshold,
                                          state.config.detection.boom_confirmation,
                                          metrics::MetricNames::Variance);
    state.event_detector.addChaosCriteria(state.config.detection.chaos_threshold,
                                           state.config.detection.chaos_confirmation,
                                           metrics::MetricNames::Variance);
    state.event_detector.reset();

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
        float y1 = cy + s.y1 * scale;  // Positive Y = below pivot (screen Y also increases downward)
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
    double frame_dt = state.config.simulation.duration_seconds / state.config.simulation.total_frames;
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

    // Track variance and spread via new metrics system
    std::vector<double> angle1s, angle2s;
    angle1s.reserve(n);
    angle2s.reserve(n);
    for (auto const& s : state.states) {
        angle1s.push_back(s.th1);
        angle2s.push_back(s.th2);
    }
    state.metrics_collector.updateFromAngles(angle1s, angle2s);

    // Compute total energy for extended analysis
    double total_energy = 0.0;
    for (auto const& p : state.pendulums) {
        total_energy += p.totalEnergy();
    }
    state.metrics_collector.setMetric(metrics::MetricNames::TotalEnergy, total_energy);

    // Frame duration for event timing
    double frame_duration = state.config.simulation.duration_seconds /
                            state.config.simulation.total_frames;

    // Update event detection
    state.event_detector.update(state.metrics_collector, frame_duration);

    // End frame metrics collection
    state.metrics_collector.endFrame();

    // Extract events for state
    if (auto boom_event = state.event_detector.getEvent(metrics::EventNames::Boom)) {
        if (boom_event->detected() && !state.boom_frame) {
            state.boom_frame = boom_event->frame;
            state.boom_variance = boom_event->value;
        }
    }
    if (auto chaos_event = state.event_detector.getEvent(metrics::EventNames::Chaos)) {
        if (chaos_event->detected() && !state.chaos_frame) {
            state.chaos_frame = chaos_event->frame;
            state.chaos_variance = chaos_event->value;
        }
    }

    // Run analyzers periodically after boom (every 30 frames)
    if (state.boom_frame && (state.current_frame % 30 == 0)) {
        state.boom_analyzer.analyze(state.metrics_collector, state.event_detector);
        state.causticness_analyzer.analyze(state.metrics_collector, state.event_detector);
    }

    auto sim_end = std::chrono::high_resolution_clock::now();
    state.sim_time_ms = std::chrono::duration<double, std::milli>(sim_end - start).count();

    // Save to frame history (if under limit)
    if (static_cast<int>(state.frame_history.size()) < state.max_history_frames) {
        state.frame_history.push_back(state.states);
    }

    // Render
    renderFrame(state, renderer);

    // Update metrics collector with GPU stats after rendering
    metrics::GPUMetricsBundle gpu_metrics;
    gpu_metrics.max_value = renderer.lastMax();
    gpu_metrics.brightness = renderer.lastBrightness();
    gpu_metrics.contrast_stddev = renderer.lastContrastStddev();
    gpu_metrics.contrast_range = renderer.lastContrastRange();
    gpu_metrics.edge_energy = renderer.lastEdgeEnergy();
    gpu_metrics.color_variance = renderer.lastColorVariance();
    gpu_metrics.coverage = renderer.lastCoverage();
    gpu_metrics.peak_median_ratio = renderer.lastPeakMedianRatio();
    state.metrics_collector.setGPUMetrics(gpu_metrics);

    state.current_frame++;
    state.display_frame = state.current_frame;
}

// Static plot mode for persistence across frames
static PlotMode s_plot_mode = PlotMode::MultiAxis;

// Helper to get metric value at a specific frame (for timeline scrubbing)
double getMetricAtFrame(metrics::MetricsCollector const& collector,
                        std::string const& name, int frame) {
    auto const* series = collector.getMetric(name);
    if (!series || series->empty()) return 0.0;
    if (frame < 0) return 0.0;
    auto const& values = series->values();
    size_t idx = std::min(static_cast<size_t>(frame), values.size() - 1);
    return values[idx];
}

// Draw analysis graph with selected metrics
void drawAnalysisGraph(AppState& state, MetricFlags const& flags, ImVec2 size) {
    auto* variance_series = state.metrics_collector.getMetric(metrics::MetricNames::Variance);
    if (!variance_series || variance_series->empty()) {
        ImGui::Text("No data yet");
        return;
    }

    size_t data_size = variance_series->size();

    std::vector<double> frames(data_size);
    for (size_t i = 0; i < data_size; ++i) {
        frames[i] = static_cast<double>(i);
    }

    double current_frame_d = static_cast<double>(state.display_frame);

    // Metric colors
    const ImVec4 color_variance(0.4f, 0.8f, 0.4f, 1.0f);
    const ImVec4 color_brightness(0.9f, 0.7f, 0.2f, 1.0f);
    const ImVec4 color_uniformity(0.2f, 0.6f, 0.9f, 1.0f);
    const ImVec4 color_causticness(0.9f, 0.3f, 0.5f, 1.0f);
    const ImVec4 color_energy(0.6f, 0.4f, 0.8f, 1.0f);

    if (ImPlot::BeginPlot("##Analysis", size, ImPlotFlags_NoTitle)) {
        // Use multi-axis: Y1 for large scale (variance, energy), Y2 for normalized (0-1)
        ImPlot::SetupAxes("Frame", "Value", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxis(ImAxis_Y2, "[0-1]", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);

        // Plot variance (Y1 - large scale)
        if (flags.variance) {
            auto const& values = variance_series->values();
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::SetNextLineStyle(color_variance, 2.0f);
            ImPlot::PlotLine("Variance", frames.data(), values.data(), data_size);

            // Threshold lines only when variance is shown
            double boom_line = state.config.detection.boom_threshold;
            double chaos_line = state.config.detection.chaos_threshold;
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 0.5f), 1.0f);
            ImPlot::PlotInfLines("##boom", &boom_line, 1, ImPlotInfLinesFlags_Horizontal);
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 0.5f), 1.0f);
            ImPlot::PlotInfLines("##chaos", &chaos_line, 1, ImPlotInfLinesFlags_Horizontal);
        }

        // Plot energy (Y1 - large scale)
        if (flags.energy) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::TotalEnergy);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::SetNextLineStyle(color_energy, 2.0f);
                ImPlot::PlotLine("Energy", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Plot brightness (Y2 - normalized)
        if (flags.brightness) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::Brightness);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlot::SetNextLineStyle(color_brightness, 2.0f);
                ImPlot::PlotLine("Brightness", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Plot uniformity (Y2 - normalized)
        if (flags.uniformity) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::CircularSpread);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlot::SetNextLineStyle(color_uniformity, 2.0f);
                ImPlot::PlotLine("Uniformity", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Plot causticness (Y1 - can be large values)
        if (flags.causticness) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::Causticness);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::SetNextLineStyle(color_causticness, 2.0f);
                ImPlot::PlotLine("Causticness", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Plot contrast stddev (Y1 - medium scale)
        if (flags.contrast_stddev) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::ContrastStddev);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.5f, 0.2f, 1.0f), 2.0f);
                ImPlot::PlotLine("Contrast", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Plot contrast range (Y1)
        if (flags.contrast_range) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::ContrastRange);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::SetNextLineStyle(ImVec4(0.9f, 0.6f, 0.3f, 1.0f), 2.0f);
                ImPlot::PlotLine("Contr.Range", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Plot edge energy (Y1)
        if (flags.edge_energy) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::EdgeEnergy);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.7f, 0.7f, 1.0f), 2.0f);
                ImPlot::PlotLine("EdgeEnergy", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Plot color variance (Y1)
        if (flags.color_variance) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::ColorVariance);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
                ImPlot::SetNextLineStyle(ImVec4(0.7f, 0.3f, 0.7f, 1.0f), 2.0f);
                ImPlot::PlotLine("ColorVar", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Plot coverage (Y2 - normalized 0-1)
        if (flags.coverage) {
            auto* series = state.metrics_collector.getMetric(metrics::MetricNames::Coverage);
            if (series && !series->empty()) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlot::SetNextLineStyle(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), 2.0f);
                ImPlot::PlotLine("Coverage", frames.data(), series->values().data(),
                                std::min(data_size, series->size()));
            }
        }

        // Boom marker (vertical line)
        if (state.boom_frame.has_value()) {
            double boom_x = static_cast<double>(*state.boom_frame);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 2.0f);
            ImPlot::PlotInfLines("##boom_marker", &boom_x, 1);
        }

        // Chaos marker (vertical line)
        if (state.chaos_frame.has_value()) {
            double chaos_x = static_cast<double>(*state.chaos_frame);
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 2.0f);
            ImPlot::PlotInfLines("##chaos_marker", &chaos_x, 1);
        }

        // Draggable current frame marker
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
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

void drawMetricGraph(AppState& state, ImVec2 size) {
    auto* variance_series = state.metrics_collector.getMetric(metrics::MetricNames::Variance);
    auto* brightness_series = state.metrics_collector.getMetric(metrics::MetricNames::Brightness);
    auto* energy_series = state.metrics_collector.getMetric(metrics::MetricNames::TotalEnergy);
    auto* causticness_series = state.metrics_collector.getMetric(metrics::MetricNames::Causticness);
    auto* contrast_stddev_series = state.metrics_collector.getMetric(metrics::MetricNames::ContrastStddev);
    auto* contrast_range_series = state.metrics_collector.getMetric(metrics::MetricNames::ContrastRange);
    auto* edge_energy_series = state.metrics_collector.getMetric(metrics::MetricNames::EdgeEnergy);
    auto* color_variance_series = state.metrics_collector.getMetric(metrics::MetricNames::ColorVariance);
    auto* coverage_series = state.metrics_collector.getMetric(metrics::MetricNames::Coverage);

    if (!variance_series || variance_series->empty()) {
        ImGui::Text("No data yet");
        return;
    }

    size_t data_size = variance_series->size();
    auto const& variance_values = variance_series->values();

    // Create frame index array for x-axis
    std::vector<double> frames(data_size);
    for (size_t i = 0; i < data_size; ++i) {
        frames[i] = static_cast<double>(i);
    }

    // Current frame marker (for dragging)
    double current_frame_d = static_cast<double>(state.display_frame);

    // Helper to normalize a series to 0-1 range
    auto normalizeData = [](std::vector<double> const& data) -> std::vector<double> {
        if (data.empty()) return {};
        double min_val = *std::min_element(data.begin(), data.end());
        double max_val = *std::max_element(data.begin(), data.end());
        double range = max_val - min_val;
        if (range < 1e-10) range = 1.0;  // Avoid division by zero
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
            ImPlot::SetupAxis(ImAxis_Y2, "[0-1]", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxis(ImAxis_Y3, "Med", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
        } else if (s_plot_mode == PlotMode::Normalized) {
            // Normalized mode: all metrics on 0-1 scale
            ImPlot::SetupAxes("Frame", "[0-1]", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
        } else {
            ImPlot::SetupAxes("Frame", nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        }

        // Plot variance (Y1 - large scale)
        if (state.detailed_flags.variance && !variance_values.empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            }
            ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(variance_values);
                ImPlot::PlotLine("Variance", frames.data(), normalized.data(), data_size);
            } else {
                ImPlot::PlotLine("Variance", frames.data(), variance_values.data(), data_size);

                // Draw threshold lines (only in non-normalized modes)
                double boom_line = state.config.detection.boom_threshold;
                double chaos_line = state.config.detection.chaos_threshold;
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 0.5f), 1.0f);
                ImPlot::PlotInfLines("##boom_thresh", &boom_line, 1, ImPlotInfLinesFlags_Horizontal);
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 0.5f), 1.0f);
                ImPlot::PlotInfLines("##chaos_thresh", &chaos_line, 1, ImPlotInfLinesFlags_Horizontal);
            }
        }
        // Plot variance derivative
        if (state.detailed_flags.variance_deriv && variance_series != nullptr && variance_series->size() > 1) {
            auto derivs = variance_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.8f, 0.4f, 0.4f), 1.0f);
            if (s_plot_mode == PlotMode::Normalized && !derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Variance'", frames.data() + 1, normalized.data(), derivs.size());
            } else if (!derivs.empty()) {
                ImPlot::PlotLine("Variance'", frames.data() + 1, derivs.data(), derivs.size());
            }
        }

        // Plot brightness (Y2 - normalized 0-1)
        if (state.detailed_flags.brightness && brightness_series != nullptr && !brightness_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            }
            auto const& brightness_values = brightness_series->values();
            ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.8f, 0.4f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(brightness_values);
                ImPlot::PlotLine("Brightness", frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine("Brightness", frames.data(), brightness_values.data(),
                                 brightness_values.size());
            }
        }
        // Plot brightness derivative
        if (state.detailed_flags.brightness_deriv && brightness_series != nullptr && brightness_series->size() > 1) {
            auto derivs = brightness_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.8f, 0.4f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Brightness'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Plot energy (Y1 - large scale)
        if (state.detailed_flags.energy && energy_series != nullptr && !energy_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            }
            auto const& energy_values = energy_series->values();
            ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(energy_values);
                ImPlot::PlotLine("Energy", frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine("Energy", frames.data(), energy_values.data(),
                                 energy_values.size());
            }
        }
        // Plot energy derivative
        if (state.detailed_flags.energy_deriv && energy_series != nullptr && energy_series->size() > 1) {
            auto derivs = energy_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.6f, 1.0f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Energy'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Plot uniformity (Y2 - normalized 0-1)
        auto const* uniformity_series = state.metrics_collector.getMetric(metrics::MetricNames::CircularSpread);
        if (state.detailed_flags.uniformity && uniformity_series != nullptr && !uniformity_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            }
            // Raw uniformity (from metric series - single source of truth)
            auto const& uniformity_values = uniformity_series->values();
            // Smoothed uniformity (5-frame window)
            auto smoothed = uniformity_series->smoothedHistory(5);

            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized_raw = normalizeData(uniformity_values);
                auto normalized_smooth = normalizeData(smoothed);
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.6f, 0.4f, 0.4f));
                ImPlot::PlotLine("Uniformity (raw)", frames.data(), normalized_raw.data(), normalized_raw.size());
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), 2.0f);
                ImPlot::PlotLine("Uniformity", frames.data(), normalized_smooth.data(), normalized_smooth.size());
            } else {
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.6f, 0.4f, 0.4f)); // Faint raw
                ImPlot::PlotLine("Uniformity (raw)", frames.data(), uniformity_values.data(), uniformity_values.size());
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), 2.0f); // Bold smoothed
                ImPlot::PlotLine("Uniformity", frames.data(), smoothed.data(), smoothed.size());
            }
        }
        // Plot uniformity derivative
        if (state.detailed_flags.uniformity_deriv && uniformity_series != nullptr && uniformity_series->size() > 1) {
            auto derivs = uniformity_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.6f, 0.4f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Uniformity'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Plot contrast stddev (Y3 - medium scale)
        if (state.detailed_flags.contrast_stddev && contrast_stddev_series != nullptr && !contrast_stddev_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
            }
            auto const& contrast_stddev_values = contrast_stddev_series->values();
            ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.4f, 0.8f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(contrast_stddev_values);
                ImPlot::PlotLine("Contrast StdDev", frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine("Contrast StdDev", frames.data(), contrast_stddev_values.data(),
                                 contrast_stddev_values.size());
            }
        }
        // Plot contrast stddev derivative
        if (state.detailed_flags.contrast_stddev_deriv && contrast_stddev_series != nullptr && contrast_stddev_series->size() > 1) {
            auto derivs = contrast_stddev_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.4f, 0.8f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Contrast StdDev'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Plot contrast range (Y3 - medium scale)
        if (state.detailed_flags.contrast_range && contrast_range_series != nullptr && !contrast_range_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
            }
            auto const& contrast_range_values = contrast_range_series->values();
            ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.8f, 0.8f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(contrast_range_values);
                ImPlot::PlotLine("Contrast Range", frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine("Contrast Range", frames.data(), contrast_range_values.data(),
                                 contrast_range_values.size());
            }
        }
        // Plot contrast range derivative
        if (state.detailed_flags.contrast_range_deriv && contrast_range_series != nullptr && contrast_range_series->size() > 1) {
            auto derivs = contrast_range_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.8f, 0.8f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Contrast Range'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Plot edge energy (Y3 - medium scale)
        if (state.detailed_flags.edge_energy && edge_energy_series != nullptr && !edge_energy_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
            }
            auto const& edge_energy_values = edge_energy_series->values();
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(edge_energy_values);
                ImPlot::PlotLine("Edge Energy", frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine("Edge Energy", frames.data(), edge_energy_values.data(),
                                 edge_energy_values.size());
            }
        }
        // Plot edge energy derivative
        if (state.detailed_flags.edge_energy_deriv && edge_energy_series != nullptr && edge_energy_series->size() > 1) {
            auto derivs = edge_energy_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 0.4f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Edge Energy'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Plot color variance (Y3 - medium scale)
        if (state.detailed_flags.color_variance && color_variance_series != nullptr && !color_variance_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
            }
            auto const& color_variance_values = color_variance_series->values();
            ImPlot::SetNextLineStyle(ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(color_variance_values);
                ImPlot::PlotLine("Color Variance", frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine("Color Variance", frames.data(), color_variance_values.data(),
                                 color_variance_values.size());
            }
        }
        // Plot color variance derivative
        if (state.detailed_flags.color_variance_deriv && color_variance_series != nullptr && color_variance_series->size() > 1) {
            auto derivs = color_variance_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(0.4f, 1.0f, 0.4f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Color Variance'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Plot coverage (Y2 - normalized 0-1)
        if (state.detailed_flags.coverage && coverage_series != nullptr && !coverage_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            }
            auto const& coverage_values = coverage_series->values();
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(coverage_values);
                ImPlot::PlotLine("Coverage", frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine("Coverage", frames.data(), coverage_values.data(),
                                 coverage_values.size());
            }
        }
        // Plot coverage derivative
        if (state.detailed_flags.coverage_deriv && coverage_series != nullptr && coverage_series->size() > 1) {
            auto derivs = coverage_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.4f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Coverage'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Plot causticness (Y3 - medium scale)
        if (state.detailed_flags.causticness && causticness_series != nullptr && !causticness_series->empty()) {
            if (s_plot_mode == PlotMode::MultiAxis) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
            }
            auto const& causticness_values = causticness_series->values();
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.2f, 1.0f, 1.0f));
            if (s_plot_mode == PlotMode::Normalized) {
                auto normalized = normalizeData(causticness_values);
                ImPlot::PlotLine("Causticness", frames.data(), normalized.data(), normalized.size());
            } else {
                ImPlot::PlotLine("Causticness", frames.data(), causticness_values.data(),
                                 causticness_values.size());
            }
        }
        // Plot causticness derivative
        if (state.detailed_flags.causticness_deriv && causticness_series != nullptr && causticness_series->size() > 1) {
            auto derivs = causticness_series->derivativeHistory();
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.2f, 1.0f, 0.4f), 1.0f);
            if (!derivs.empty()) {
                auto normalized = normalizeData(derivs);
                ImPlot::PlotLine("Causticness'", frames.data() + 1, normalized.data(), derivs.size());
            }
        }

        // Draw boom marker
        if (state.boom_frame.has_value()) {
            double boom_x = static_cast<double>(*state.boom_frame);
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 2.0f);
            ImPlot::PlotInfLines("##boom", &boom_x, 1);
        }

        // Draw chaos marker
        if (state.chaos_frame.has_value()) {
            double chaos_x = static_cast<double>(*state.chaos_frame);
            ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 2.0f);
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
    std::uniform_real_distribution<double> angle_dist(100.0, 260.0);  // degrees, around 180
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
    draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                       IM_COL32(100, 100, 100, 255));

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
        double frame_dt = state.config.simulation.duration_seconds / state.config.simulation.total_frames;
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
        }
        tooltip("Total simulation time in physical seconds");

        ImGui::SliderInt("Total Frames", &state.config.simulation.total_frames, 60, 3600);
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
        const char* schemes[] = {"Spectrum", "Rainbow", "Heat", "Cool", "Monochrome",
                                 "Plasma", "Viridis", "Inferno", "Sunset"};
        int scheme_idx = static_cast<int>(state.config.color.scheme);
        if (ImGui::Combo("Scheme", &scheme_idx, schemes, 9)) {
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

void drawDetectionSection(AppState& state) {
    if (ImGui::CollapsingHeader("Detection")) {
        auto boom_thresh = static_cast<float>(state.config.detection.boom_threshold);
        if (ImGui::SliderFloat("Boom Threshold", &boom_thresh, 0.01f, 1.0f, "%.3f rad^2")) {
            state.config.detection.boom_threshold = boom_thresh;
        }
        tooltip("Variance threshold for chaos onset detection");

        ImGui::SliderInt("Boom Confirm", &state.config.detection.boom_confirmation, 1, 30);
        tooltip("Consecutive frames above threshold to confirm boom");

        auto chaos_thresh = static_cast<float>(state.config.detection.chaos_threshold);
        if (ImGui::InputFloat("Chaos Threshold", &chaos_thresh, 10.0f, 100.0f, "%.1f rad^2")) {
            state.config.detection.chaos_threshold = chaos_thresh;
        }
        tooltip("Variance threshold for full chaos");

        ImGui::SliderInt("Chaos Confirm", &state.config.detection.chaos_confirmation, 1, 30);
        tooltip("Consecutive frames above threshold to confirm chaos");
    }
}

void drawControlPanel(AppState& state, GLRenderer& renderer) {
    ImGui::Begin("Controls");

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
    } else {
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
    ImGui::Text("Frame: %d", state.current_frame);
    ImGui::Text("FPS: %.1f", state.fps);
    ImGui::Text("Sim: %.2f ms", state.sim_time_ms);
    ImGui::Text("Render: %.2f ms", state.render_time_ms);

    // Event status
    if (state.boom_frame.has_value()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Boom: frame %d", *state.boom_frame);
    }
    if (state.chaos_frame.has_value()) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Chaos: frame %d", *state.chaos_frame);
    }

    ImGui::Separator();

    // Draw each section
    drawPreviewSection(state);
    drawPhysicsSection(state);
    drawSimulationSection(state);
    drawColorSection(state);
    drawPostProcessSection(state);
    drawDetectionSection(state);

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
    ImGui::Separator();

    // Playback controls
    if (ImGui::Button(state.paused ? "Play" : "Pause")) {
        state.paused = !state.paused;
        if (!state.paused) {
            // Resume from current display frame
            state.scrubbing = false;
        }
    }
    ImGui::SameLine();

    // Step backward
    if (ImGui::Button("<<") && state.display_frame > 0) {
        state.paused = true;
        state.display_frame--;
        state.scrubbing = true;
        renderFrameFromHistory(state, renderer, state.display_frame);
    }
    ImGui::SameLine();

    // Step forward (only within history)
    if (ImGui::Button(">>") && state.display_frame < history_size - 1) {
        state.paused = true;
        state.display_frame++;
        state.scrubbing = true;
        renderFrameFromHistory(state, renderer, state.display_frame);
    }
    ImGui::SameLine();

    // Jump to live
    if (ImGui::Button("Live")) {
        state.scrubbing = false;
        state.display_frame = state.current_frame;
        if (!state.frame_history.empty()) {
            renderFrameFromHistory(state, renderer,
                                   std::min(state.display_frame, history_size - 1));
        }
    }

    // Timeline slider
    int max_frame = std::max(1, history_size - 1);
    int slider_frame = std::min(state.display_frame, max_frame);

    if (ImGui::SliderInt("Frame", &slider_frame, 0, max_frame)) {
        state.paused = true;
        state.scrubbing = true;
        state.display_frame = slider_frame;
        renderFrameFromHistory(state, renderer, state.display_frame);
    }

    // Frame info
    ImGui::Text("Displaying: %d / %d", state.display_frame, history_size - 1);
    if (history_size >= state.max_history_frames) {
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
    (void)argc;
    (void)argv;

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
    state.config = Config::load("config/default.toml");
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
                    } else {
                        state.paused = !state.paused;
                    }
                    break;
                case SDLK_r:
                    if (!state.running) {
                        randomizePhysics(state);
                    } else {
                        initSimulation(state, renderer);
                    }
                    break;
                case SDLK_PERIOD:
                    if (state.running && state.paused) {
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

        // Step simulation if running
        if (state.running && !state.paused) {
            stepSimulation(state, renderer);
        }

        // Re-render if needed (e.g., color/post-processing changed while paused)
        if (state.needs_redraw) {
            renderFrame(state, renderer);
            state.needs_redraw = false;
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

        // High-Level Analysis - metric graph with simple selectors
        ImGui::Begin("High-Level Analysis");

        // Metric checkboxes - row 1
        ImGui::Checkbox("Var", &state.highlevel_flags.variance);
        ImGui::SameLine();
        ImGui::Checkbox("Bright", &state.highlevel_flags.brightness);
        ImGui::SameLine();
        ImGui::Checkbox("Unif", &state.highlevel_flags.uniformity);
        ImGui::SameLine();
        ImGui::Checkbox("Caustic", &state.highlevel_flags.causticness);
        ImGui::SameLine();
        ImGui::Checkbox("Energy", &state.highlevel_flags.energy);

        // Metric checkboxes - row 2
        ImGui::Checkbox("Contrast", &state.highlevel_flags.contrast_stddev);
        ImGui::SameLine();
        ImGui::Checkbox("Contr.Rng", &state.highlevel_flags.contrast_range);
        ImGui::SameLine();
        ImGui::Checkbox("Edge", &state.highlevel_flags.edge_energy);
        ImGui::SameLine();
        ImGui::Checkbox("ColorVar", &state.highlevel_flags.color_variance);
        ImGui::SameLine();
        ImGui::Checkbox("Coverage", &state.highlevel_flags.coverage);

        // Graph with selected metrics
        ImVec2 graph_size = ImGui::GetContentRegionAvail();
        graph_size.y = std::max(100.0f, graph_size.y);
        drawAnalysisGraph(state, state.highlevel_flags, graph_size);

        ImGui::End();

        // Quality Scores window (separate)
        ImGui::Begin("Quality");

        if (state.boom_analyzer.hasResults() || state.causticness_analyzer.hasResults()) {
            // Get scores
            double boom_score = state.boom_analyzer.hasResults() ? state.boom_analyzer.score() : 0.0;
            double caustic_score = state.causticness_analyzer.hasResults() ? state.causticness_analyzer.score() : 0.0;
            double composite = (boom_score + caustic_score) / 2.0;

            // Composite score at top (most important)
            ImVec4 bar_color = composite < 0.4f ? ImVec4(0.8f, 0.2f, 0.2f, 1.0f) :
                               composite < 0.7f ? ImVec4(0.8f, 0.8f, 0.2f, 1.0f) :
                                                  ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            ImGui::Text("Composite:");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
            ImGui::ProgressBar(static_cast<float>(composite), ImVec2(100, 0));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%.2f", composite);

            ImGui::Separator();

            // Boom Analysis section
            if (state.boom_analyzer.hasResults()) {
                auto boom_results = state.boom_analyzer.toJSON();
                double sharpness = boom_results.value("sharpness_ratio", 0.0);
                std::string type = boom_results.value("boom_type", "unknown");
                int peak_frame = boom_results.value("peak_derivative_frame", 0);

                ImGui::Text("Boom:");
                ImGui::SameLine();
                ImGui::ProgressBar(static_cast<float>(boom_score), ImVec2(80, 0));
                ImGui::SameLine();
                ImGui::Text("%.2f", boom_score);
                ImGui::Text("  Type: %s | Sharpness: %.2f | Peak: frame %d", type.c_str(), sharpness, peak_frame);

                if (ImGui::TreeNode("Boom Details")) {
                    double peak_deriv = boom_results.value("peak_derivative", 0.0);
                    double init_accel = boom_results.value("initial_acceleration", 0.0);
                    double pre_boom_mean = boom_results.value("pre_boom_variance_mean", 0.0);
                    double post_boom_max = boom_results.value("post_boom_variance_max", 0.0);
                    int frames_to_peak = boom_results.value("frames_to_peak", 0);

                    ImGui::Text("Peak derivative: %.3f", peak_deriv);
                    ImGui::Text("Frames to peak: %d", frames_to_peak);
                    ImGui::Text("Initial acceleration: %.4f", init_accel);
                    ImGui::Text("Pre-boom variance mean: %.3f", pre_boom_mean);
                    ImGui::Text("Post-boom variance max: %.3f", post_boom_max);
                    ImGui::TreePop();
                }
            }

            // Causticness section
            if (state.causticness_analyzer.hasResults()) {
                auto caustic_results = state.causticness_analyzer.toJSON();
                double peak = caustic_results.value("peak_causticness", 0.0);
                double avg = caustic_results.value("average_causticness", 0.0);
                int peak_frame = caustic_results.value("peak_frame", 0);
                double time_above = caustic_results.value("time_above_threshold", 0.0);

                ImGui::Text("Causticness:");
                ImGui::SameLine();
                ImGui::ProgressBar(static_cast<float>(caustic_score), ImVec2(80, 0));
                ImGui::SameLine();
                ImGui::Text("%.2f", caustic_score);

                double peak_seconds = static_cast<double>(peak_frame) / state.config.output.video_fps;
                ImGui::Text("  Peak: %.1f @ %.1fs | Avg: %.1f | Time above: %.1fs", peak, peak_seconds, avg, time_above);

                if (ImGui::TreeNode("Causticness Details")) {
                    int frames_above = caustic_results.value("frames_above_threshold", 0);
                    double post_boom_avg = caustic_results.value("post_boom_average", 0.0);
                    double post_boom_peak = caustic_results.value("post_boom_peak", 0.0);
                    double threshold = caustic_results.value("threshold", 0.0);

                    ImGui::Text("Threshold: %.1f", threshold);
                    ImGui::Text("Frames above threshold: %d", frames_above);
                    ImGui::Text("Post-boom average: %.2f", post_boom_avg);
                    ImGui::Text("Post-boom peak: %.2f", post_boom_peak);
                    ImGui::TreePop();
                }
            }
        } else {
            ImGui::TextDisabled("No quality data yet");
            ImGui::TextDisabled("Run simulation to analyze");
        }

        ImGui::End();

        // Detailed Analysis - full metric controls with derivatives
        ImGui::Begin("Detailed Analysis");

        // Plot mode selector
        const char* plot_mode_names[] = {"Single Axis", "Multi-Axis", "Normalized"};
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("Scale", reinterpret_cast<int*>(&s_plot_mode), plot_mode_names, 3);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Single: All on one axis\nMulti: Y1=large, Y2=normalized, Y3=medium\nNormalized: All scaled 0-1");
        }

        // Metric selector with derivative toggles
        auto metricWithDeriv = [](const char* label, bool* metric, bool* deriv, const char* id) {
            ImGui::Checkbox(label, metric);
            ImGui::SameLine(0, 2);
            ImGui::PushStyleColor(ImGuiCol_Text, *deriv ? ImVec4(0.4f, 0.8f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 0));
            char btn_id[32];
            snprintf(btn_id, sizeof(btn_id), "d##%s", id);
            if (ImGui::SmallButton(btn_id)) {
                *deriv = !*deriv;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        };

        ImGui::Text("Metrics:");
        ImGui::SameLine();
        metricWithDeriv("Var", &state.detailed_flags.variance, &state.detailed_flags.variance_deriv, "var");
        ImGui::SameLine();
        metricWithDeriv("Bright", &state.detailed_flags.brightness, &state.detailed_flags.brightness_deriv, "bright");
        ImGui::SameLine();
        metricWithDeriv("Unif", &state.detailed_flags.uniformity, &state.detailed_flags.uniformity_deriv, "unif");
        ImGui::SameLine();
        metricWithDeriv("Edge", &state.detailed_flags.edge_energy, &state.detailed_flags.edge_energy_deriv, "edge");
        ImGui::SameLine();
        metricWithDeriv("Caustic", &state.detailed_flags.causticness, &state.detailed_flags.causticness_deriv, "caustic");

        // Second row of metrics
        metricWithDeriv("Energy", &state.detailed_flags.energy, &state.detailed_flags.energy_deriv, "energy");
        ImGui::SameLine();
        metricWithDeriv("Contr.Std", &state.detailed_flags.contrast_stddev, &state.detailed_flags.contrast_stddev_deriv, "cstd");
        ImGui::SameLine();
        metricWithDeriv("Contr.Rng", &state.detailed_flags.contrast_range, &state.detailed_flags.contrast_range_deriv, "crng");
        ImGui::SameLine();
        metricWithDeriv("ColorVar", &state.detailed_flags.color_variance, &state.detailed_flags.color_variance_deriv, "cvar");
        ImGui::SameLine();
        metricWithDeriv("Coverage", &state.detailed_flags.coverage, &state.detailed_flags.coverage_deriv, "cov");

        // Full metric graph
        ImVec2 metrics_graph_size = ImGui::GetContentRegionAvail();
        metrics_graph_size.y = std::max(150.0f, metrics_graph_size.y - 100.0f);
        drawMetricGraph(state, metrics_graph_size);

        // Current metric values table (updates with timeline scrubbing)
        ImGui::Separator();
        ImGui::Text("Current Values (frame %d):", state.display_frame);

        if (ImGui::BeginTable("##MetricValues", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Metric");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Metric");
            ImGui::TableSetupColumn("Value");

            auto showMetric = [&](const char* name, const std::string& metric_name) {
                double val = getMetricAtFrame(state.metrics_collector, metric_name, state.display_frame);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", val);
            };

            ImGui::TableNextRow();
            showMetric("Variance", metrics::MetricNames::Variance);
            showMetric("Brightness", metrics::MetricNames::Brightness);

            ImGui::TableNextRow();
            showMetric("Uniformity", metrics::MetricNames::CircularSpread);
            showMetric("Energy", metrics::MetricNames::TotalEnergy);

            ImGui::TableNextRow();
            showMetric("Causticness", metrics::MetricNames::Causticness);
            showMetric("Coverage", metrics::MetricNames::Coverage);

            ImGui::TableNextRow();
            showMetric("Contrast", metrics::MetricNames::ContrastStddev);
            showMetric("EdgeEnergy", metrics::MetricNames::EdgeEnergy);

            ImGui::EndTable();
        }

        ImGui::End();

        // Timeline
        ImGui::Begin("Timeline");
        drawTimeline(state, renderer);
        ImGui::End();

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
