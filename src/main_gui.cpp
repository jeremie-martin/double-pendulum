#include "analysis_tracker.h"
#include "color_scheme.h"
#include "config.h"
#include "gl_renderer.h"
#include "pendulum.h"
#include "simulation.h"
#include "variance_tracker.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// Preview parameters (lower resolution for real-time)
struct PreviewParams {
    int width = 540;
    int height = 540;
    int pendulum_count = 10000;
    int substeps = 10;
};

// Graph metric selection
enum class GraphMetric { Variance, Brightness, Energy, Spread, ContrastStddev, ContrastRange };

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

// Application state
struct AppState {
    Config config;
    PreviewParams preview;

    // Simulation state
    std::vector<Pendulum> pendulums;
    std::vector<PendulumState> states;
    std::vector<Color> colors;
    VarianceTracker variance_tracker;
    AnalysisTracker analysis_tracker;
    GraphMetric selected_metric = GraphMetric::Variance;

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
    std::optional<int> white_frame;
    double white_variance = 0.0;

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

    state.variance_tracker.reset();
    state.boom_frame.reset();
    state.white_frame.reset();
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
        float y1 = cy + s.y1 * scale;
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
    double dt = state.config.simulation.duration_seconds /
                (state.config.simulation.total_frames * state.preview.substeps);

    // Physics step
    for (int s = 0; s < state.preview.substeps; ++s) {
        for (int i = 0; i < n; ++i) {
            state.states[i] = state.pendulums[i].step(dt);
        }
    }

    // Track variance and spread
    std::vector<double> angle1s, angle2s;
    angle1s.reserve(n);
    angle2s.reserve(n);
    for (auto const& s : state.states) {
        angle1s.push_back(s.th1);
        angle2s.push_back(s.th2);
    }
    state.variance_tracker.updateWithSpread(angle2s, angle1s);

    // Extended analysis tracking (includes energy and brightness)
    state.analysis_tracker.update(state.pendulums, 0.0f, 0.0f);

    // Update detection using shared utility
    VarianceUtils::ThresholdResults detection{state.boom_frame, state.boom_variance,
                                              state.white_frame, state.white_variance};
    VarianceUtils::updateDetection(
        detection, state.variance_tracker, state.config.detection.boom_threshold,
        state.config.detection.boom_confirmation, state.config.detection.white_threshold,
        state.config.detection.white_confirmation);
    state.boom_frame = detection.boom_frame;
    state.boom_variance = detection.boom_variance;
    state.white_frame = detection.white_frame;
    state.white_variance = detection.white_variance;

    auto sim_end = std::chrono::high_resolution_clock::now();
    state.sim_time_ms = std::chrono::duration<double, std::milli>(sim_end - start).count();

    // Save to frame history (if under limit)
    if (static_cast<int>(state.frame_history.size()) < state.max_history_frames) {
        state.frame_history.push_back(state.states);
    }

    // Render
    renderFrame(state, renderer);

    // Update analysis tracker with GPU stats after rendering
    state.analysis_tracker.updateGPUStats(renderer.lastMax(), renderer.lastBrightness(),
                                          renderer.lastContrastStddev(),
                                          renderer.lastContrastRange());

    state.current_frame++;
    state.display_frame = state.current_frame;
}

void drawMetricGraph(AppState const& state, ImVec2 size) {
    auto const& analysis = state.analysis_tracker.getHistory();
    auto const& variance_history = state.variance_tracker.getHistory();
    if (variance_history.empty()) {
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Background
    draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                             IM_COL32(30, 30, 30, 255));

    size_t data_size = variance_history.size();
    float x_scale = size.x / std::max(1.0f, static_cast<float>(data_size - 1));

    // Get data for selected metric and find max for scaling
    std::vector<double> data;
    data.reserve(data_size);
    double max_val = 1.0;
    ImU32 line_color = IM_COL32(100, 200, 100, 255);
    char const* metric_label = "Variance";

    switch (state.selected_metric) {
    case GraphMetric::Variance:
        data = variance_history;
        line_color = IM_COL32(100, 200, 100, 255);
        metric_label = "Variance";
        break;
    case GraphMetric::Brightness:
        for (auto const& a : analysis) {
            data.push_back(a.brightness);
        }
        line_color = IM_COL32(200, 200, 100, 255);
        metric_label = "Brightness";
        break;
    case GraphMetric::Energy:
        for (auto const& a : analysis) {
            data.push_back(a.total_energy);
        }
        line_color = IM_COL32(100, 150, 255, 255);
        metric_label = "Energy";
        break;
    case GraphMetric::Spread:
        for (auto const& s : state.variance_tracker.getSpreadHistory()) {
            data.push_back(s.spread_ratio);
        }
        line_color = IM_COL32(255, 150, 100, 255);
        metric_label = "Spread";
        max_val = 1.0; // Spread ratio is always 0-1
        break;
    case GraphMetric::ContrastStddev:
        for (auto const& a : analysis) {
            data.push_back(a.contrast_stddev);
        }
        line_color = IM_COL32(200, 100, 200, 255);
        metric_label = "Contrast (StdDev)";
        max_val = 0.5; // Stddev typically 0-0.5 for normalized luminance
        break;
    case GraphMetric::ContrastRange:
        for (auto const& a : analysis) {
            data.push_back(a.contrast_range);
        }
        line_color = IM_COL32(100, 200, 200, 255);
        metric_label = "Contrast (Range)";
        max_val = 1.0; // p95-p5 range is 0-1
        break;
    }

    // Pad data if analysis tracker has fewer entries
    while (data.size() < data_size) {
        data.push_back(0.0);
    }

    for (double v : data) {
        max_val = std::max(max_val, std::abs(v));
    }

    // Draw metric line
    for (size_t i = 1; i < data.size(); ++i) {
        float x0 = pos.x + (i - 1) * x_scale;
        float x1 = pos.x + i * x_scale;
        float y0 = pos.y + size.y - static_cast<float>((data[i - 1] / max_val) * size.y);
        float y1 = pos.y + size.y - static_cast<float>((data[i] / max_val) * size.y);

        draw_list->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), line_color);
    }

    // Draw threshold lines only for variance
    if (state.selected_metric == GraphMetric::Variance) {
        float boom_y =
            pos.y + size.y -
            static_cast<float>((state.config.detection.boom_threshold / max_val) * size.y);
        draw_list->AddLine(ImVec2(pos.x, boom_y), ImVec2(pos.x + size.x, boom_y),
                           IM_COL32(255, 200, 50, 100));

        float white_y =
            pos.y + size.y -
            static_cast<float>((state.config.detection.white_threshold / max_val) * size.y);
        draw_list->AddLine(ImVec2(pos.x, white_y), ImVec2(pos.x + size.x, white_y),
                           IM_COL32(255, 255, 255, 100));
    }

    // Draw boom/white markers (always shown)
    if (state.boom_frame.has_value()) {
        float x = pos.x + *state.boom_frame * x_scale;
        draw_list->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y),
                           IM_COL32(255, 200, 50, 255));
    }

    if (state.white_frame.has_value()) {
        float x = pos.x + *state.white_frame * x_scale;
        draw_list->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y),
                           IM_COL32(255, 255, 255, 255));
    }

    // Draw current frame position indicator (cyan line)
    if (state.display_frame >= 0 && state.display_frame < static_cast<int>(data_size)) {
        float x = pos.x + state.display_frame * x_scale;
        draw_list->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), IM_COL32(0, 200, 255, 200));
    }

    // Show current value and metric label
    if (!data.empty()) {
        char label[64];
        double current_val =
            (state.display_frame >= 0 && state.display_frame < static_cast<int>(data.size()))
                ? data[state.display_frame]
                : data.back();
        snprintf(label, sizeof(label), "%s: %.4g (max: %.4g)", metric_label, current_val, max_val);
        draw_list->AddText(ImVec2(pos.x + 5, pos.y + 5), IM_COL32(255, 255, 255, 200), label);
    }

    ImGui::Dummy(size);
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

void drawPreviewSection(AppState& state) {
    if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderInt("Pendulums", &state.preview.pendulum_count, 1000, 100000);
        tooltip("Number of pendulums in preview (lower = faster)");

        ImGui::SliderInt("Preview Size", &state.preview.width, 270, 1080);
        tooltip("Preview resolution (lower = faster)");
        state.preview.height = state.preview.width;

        ImGui::SliderInt("Substeps", &state.preview.substeps, 1, 50);
        tooltip("Physics substeps per frame (higher = more accurate)");
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
    if (ImGui::CollapsingHeader("Color")) {
        bool color_changed = false;

        const char* schemes[] = {"Spectrum", "Rainbow", "Heat", "Cool", "Monochrome"};
        int scheme_idx = static_cast<int>(state.config.color.scheme);
        if (ImGui::Combo("Color Scheme", &scheme_idx, schemes, 5)) {
            state.config.color.scheme = static_cast<ColorScheme>(scheme_idx);
            color_changed = true;
        }
        tooltip("Color mapping for pendulum index");

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

        auto white_thresh = static_cast<float>(state.config.detection.white_threshold);
        if (ImGui::InputFloat("White Threshold", &white_thresh, 10.0f, 100.0f, "%.1f rad^2")) {
            state.config.detection.white_threshold = white_thresh;
        }
        tooltip("Variance threshold for full chaos (white noise)");

        ImGui::SliderInt("White Confirm", &state.config.detection.white_confirmation, 1, 30);
        tooltip("Consecutive frames above threshold to confirm white");
    }
}

void drawControlPanel(AppState& state, GLRenderer& renderer) {
    ImGui::Begin("Controls");

    // Simulation control buttons
    if (!state.running) {
        if (ImGui::Button("Start Simulation")) {
            initSimulation(state, renderer);
        }
    } else {
        if (ImGui::Button(state.paused ? "Resume" : "Pause")) {
            state.paused = !state.paused;
        }
        ImGui::SameLine();
        if (state.paused) {
            if (ImGui::Button("Step")) {
                state.paused = false;
                stepSimulation(state, renderer);
                state.paused = true;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Restart")) {
            initSimulation(state, renderer);
        }
    }

    // Status display
    ImGui::Separator();
    ImGui::Text("Frame: %d", state.current_frame);
    ImGui::Text("FPS: %.1f", state.fps);
    ImGui::Text("Sim: %.2f ms", state.sim_time_ms);
    ImGui::Text("Render: %.2f ms", state.render_time_ms);

    // Analysis metrics
    ImGui::Separator();
    ImGui::Text("Variance: %.4f", state.variance_tracker.getCurrentVariance());
    ImGui::Text("Spread:   %.1f%% above",
                state.variance_tracker.getCurrentSpread().spread_ratio * 100);
    auto const& current = state.analysis_tracker.getCurrent();
    ImGui::Text("Energy:   %.2f", current.total_energy);

    if (state.boom_frame.has_value()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Boom: frame %d (var=%.4f)",
                           *state.boom_frame, state.boom_variance);
    }
    if (state.white_frame.has_value()) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "White: frame %d (var=%.4f)",
                           *state.white_frame, state.white_variance);
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
    if (state.white_frame.has_value() && *state.white_frame < history_size) {
        float white_pos = static_cast<float>(*state.white_frame) / max_frame;
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "White at frame %d (%.1f%%)",
                           *state.white_frame, white_pos * 100);
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

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
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

    // Load config
    AppState state;
    state.config = Config::load("config/default.toml");

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
                     preview_size, ImVec2(0, 1), ImVec2(1, 0));
        ImGui::End();

        // Analysis graph with metric selector
        ImGui::Begin("Analysis");

        // Metric selector
        char const* metric_names[] = {"Variance", "Brightness",        "Energy",
                                      "Spread",   "Contrast (StdDev)", "Contrast (Range)"};
        int current_metric = static_cast<int>(state.selected_metric);
        if (ImGui::Combo("Metric", &current_metric, metric_names, 6)) {
            state.selected_metric = static_cast<GraphMetric>(current_metric);
        }

        ImVec2 graph_size = ImGui::GetContentRegionAvail();
        graph_size.y = std::max(100.0f, graph_size.y - 60.0f); // Leave room for score
        drawMetricGraph(state, graph_size);

        // Display current and peak metrics
        ImGui::Separator();
        auto const& current = state.analysis_tracker.getCurrent();
        ImGui::Text("Current: Brightness %.3f  Contrast %.3f", current.brightness,
                    current.contrast_stddev);

        // Calculate peak causticness from history
        if (!state.analysis_tracker.getHistory().empty() && state.boom_frame) {
            auto const& history = state.analysis_tracker.getHistory();
            double peak_causticness = 0.0;
            int best_frame = -1;
            for (size_t i = *state.boom_frame; i < history.size(); ++i) {
                double causticness = history[i].causticness();
                if (causticness > peak_causticness) {
                    peak_causticness = causticness;
                    best_frame = static_cast<int>(i);
                }
            }
            ImGui::Text("Peak Causticness: %.4f (frame %d)", peak_causticness, best_frame);
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
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
