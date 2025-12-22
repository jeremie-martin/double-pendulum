#include "color_scheme.h"
#include "config.h"
#include "gl_renderer.h"
#include "pendulum.h"
#include "variance_tracker.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// Preview parameters (lower resolution for real-time)
struct PreviewParams
{
    int width = 540;
    int height = 540;
    int pendulum_count = 10000;
    int substeps = 10;
};

// Application state
struct AppState
{
    Config config;
    PreviewParams preview;

    // Simulation state
    std::vector<Pendulum> pendulums;
    std::vector<PendulumState> states;
    std::vector<Color> colors;
    VarianceTracker variance_tracker;

    // Control
    bool running = false;
    bool paused = false;
    int current_frame = 0;

    // Detection results
    std::optional<int> boom_frame;
    double boom_variance = 0.0;
    std::optional<int> white_frame;
    double white_variance = 0.0;

    // Timing
    double fps = 0.0;
    double sim_time_ms = 0.0;
    double render_time_ms = 0.0;
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
            state.config.physics.gravity,
            state.config.physics.length1,
            state.config.physics.length2,
            state.config.physics.mass1,
            state.config.physics.mass2,
            th1,
            state.config.physics.initial_angle2,
            state.config.physics.initial_velocity1,
            state.config.physics.initial_velocity2
        );

        state.colors[i] = color_gen.getColorForIndex(i, n);
    }

    state.variance_tracker.reset();
    state.boom_frame.reset();
    state.white_frame.reset();
    state.current_frame = 0;
    state.running = true;
    state.paused = false;

    renderer.resize(state.preview.width, state.preview.height);
}

void stepSimulation(AppState& state, GLRenderer& renderer) {
    if (!state.running || state.paused)
        return;

    auto start = std::chrono::high_resolution_clock::now();

    int n = state.pendulums.size();
    double dt = state.config.simulation.duration_seconds / (state.config.simulation.total_frames * state.preview.substeps);

    // Physics step
    for (int s = 0; s < state.preview.substeps; ++s) {
        for (int i = 0; i < n; ++i) {
            state.states[i] = state.pendulums[i].step(dt);
        }
    }

    // Track variance
    std::vector<double> angles;
    angles.reserve(n);
    for (auto const& s : state.states) {
        angles.push_back(s.th2);
    }
    state.variance_tracker.update(angles);

    // Check for boom
    if (!state.boom_frame.has_value()) {
        int boom = VarianceUtils::checkThresholdCrossing(
            state.variance_tracker.getHistory(),
            state.config.detection.boom_threshold,
            state.config.detection.boom_confirmation
        );
        if (boom >= 0) {
            state.boom_frame = boom;
            state.boom_variance = state.variance_tracker.getVarianceAt(boom);
        }
    }

    // Check for white
    if (state.boom_frame.has_value() && !state.white_frame.has_value()) {
        int white = VarianceUtils::checkThresholdCrossing(
            state.variance_tracker.getHistory(),
            state.config.detection.white_threshold,
            state.config.detection.white_confirmation
        );
        if (white >= 0) {
            state.white_frame = white;
            state.white_variance = state.variance_tracker.getVarianceAt(white);
        }
    }

    auto sim_end = std::chrono::high_resolution_clock::now();
    state.sim_time_ms = std::chrono::duration<double, std::milli>(sim_end - start).count();

    // Render
    auto render_start = std::chrono::high_resolution_clock::now();

    renderer.clear();

    float scale = state.preview.width / 5.0f;
    float cx = state.preview.width / 2.0f;
    float cy = state.preview.height / 2.0f;

    for (int i = 0; i < n; ++i) {
        auto const& s = state.states[i];
        auto const& c = state.colors[i];

        float x0 = cx;
        float y0 = cy;
        float x1 = cx + s.x1 * scale;
        float y1 = cy + s.y1 * scale;
        float x2 = cx + s.x2 * scale;
        float y2 = cy + s.y2 * scale;

        renderer.drawLine(x0, y0, x1, y1, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f);
        renderer.drawLine(x1, y1, x2, y2, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f);
    }

    renderer.updateDisplayTexture(state.config.post_process.gamma, state.config.post_process.target_brightness);

    auto render_end = std::chrono::high_resolution_clock::now();
    state.render_time_ms = std::chrono::duration<double, std::milli>(render_end - render_start).count();

    state.current_frame++;
}

void drawVarianceGraph(AppState const& state, ImVec2 size) {
    auto const& history = state.variance_tracker.getHistory();
    if (history.empty())
        return;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Background
    draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(30, 30, 30, 255));

    // Find max variance for scaling
    double max_var = 1.0;
    for (double v : history) {
        max_var = std::max(max_var, v);
    }

    // Draw variance line
    float x_scale = size.x / std::max(1.0f, static_cast<float>(history.size() - 1));

    for (size_t i = 1; i < history.size(); ++i) {
        float x0 = pos.x + (i - 1) * x_scale;
        float x1 = pos.x + i * x_scale;
        float y0 = pos.y + size.y - (history[i - 1] / max_var) * size.y;
        float y1 = pos.y + size.y - (history[i] / max_var) * size.y;

        draw_list->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(100, 200, 100, 255));
    }

    // Draw boom threshold line
    float boom_y = pos.y + size.y - (state.config.detection.boom_threshold / max_var) * size.y;
    draw_list->AddLine(ImVec2(pos.x, boom_y), ImVec2(pos.x + size.x, boom_y), IM_COL32(255, 200, 50, 100));

    // Draw white threshold line
    float white_y = pos.y + size.y - (state.config.detection.white_threshold / max_var) * size.y;
    draw_list->AddLine(ImVec2(pos.x, white_y), ImVec2(pos.x + size.x, white_y), IM_COL32(255, 255, 255, 100));

    // Draw boom marker
    if (state.boom_frame.has_value()) {
        float x = pos.x + *state.boom_frame * x_scale;
        draw_list->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), IM_COL32(255, 200, 50, 255));
    }

    // Draw white marker
    if (state.white_frame.has_value()) {
        float x = pos.x + *state.white_frame * x_scale;
        draw_list->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), IM_COL32(255, 255, 255, 255));
    }

    ImGui::Dummy(size);
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
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // Create window
    SDL_Window* window = SDL_CreateWindow(
        "Double Pendulum - GUI",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window) {
        std::cerr << "Window creation error: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // VSync

    // Detect DPI scale (compare window size to drawable size)
    int window_w, window_h, drawable_w, drawable_h;
    SDL_GetWindowSize(window, &window_w, &window_h);
    SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
    float dpi_scale = static_cast<float>(drawable_w) / static_cast<float>(window_w);
    if (dpi_scale < 1.0f) dpi_scale = 1.0f;  // Ensure minimum scale of 1
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
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) {
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

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Control panel
        ImGui::Begin("Controls");

        if (!state.running) {
            if (ImGui::Button("Start Simulation")) {
                initSimulation(state, renderer);
            }
        }
        else {
            if (ImGui::Button(state.paused ? "Resume" : "Pause")) {
                state.paused = !state.paused;
            }
            ImGui::SameLine();
            if (state.paused) {
                if (ImGui::Button("Step")) {
                    // Single frame step
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

        ImGui::Separator();
        ImGui::Text("Frame: %d", state.current_frame);
        ImGui::Text("FPS: %.1f", state.fps);
        ImGui::Text("Sim: %.2f ms", state.sim_time_ms);
        ImGui::Text("Render: %.2f ms", state.render_time_ms);
        ImGui::Text("Variance: %.4f", state.variance_tracker.getCurrentVariance());

        if (state.boom_frame.has_value()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Boom: frame %d (var=%.4f)", *state.boom_frame, state.boom_variance);
        }
        if (state.white_frame.has_value()) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "White: frame %d (var=%.4f)", *state.white_frame, state.white_variance);
        }

        ImGui::Separator();

        // Preview settings
        if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderInt("Pendulums", &state.preview.pendulum_count, 1000, 100000);
            ImGui::SliderInt("Preview Size", &state.preview.width, 270, 1080);
            state.preview.height = state.preview.width; // Keep square
            ImGui::SliderInt("Substeps", &state.preview.substeps, 1, 50);
        }

        // Physics parameters
        if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
            float gravity = static_cast<float>(state.config.physics.gravity);
            if (ImGui::SliderFloat("Gravity", &gravity, 0.1f, 20.0f)) {
                state.config.physics.gravity = gravity;
            }

            float length1 = static_cast<float>(state.config.physics.length1);
            if (ImGui::SliderFloat("Length 1", &length1, 0.1f, 3.0f)) {
                state.config.physics.length1 = length1;
            }

            float length2 = static_cast<float>(state.config.physics.length2);
            if (ImGui::SliderFloat("Length 2", &length2, 0.1f, 3.0f)) {
                state.config.physics.length2 = length2;
            }

            float mass1 = static_cast<float>(state.config.physics.mass1);
            if (ImGui::SliderFloat("Mass 1", &mass1, 0.1f, 5.0f)) {
                state.config.physics.mass1 = mass1;
            }

            float mass2 = static_cast<float>(state.config.physics.mass2);
            if (ImGui::SliderFloat("Mass 2", &mass2, 0.1f, 5.0f)) {
                state.config.physics.mass2 = mass2;
            }

            float angle1_deg = rad2deg(state.config.physics.initial_angle1);
            if (ImGui::SliderFloat("Initial Angle 1", &angle1_deg, -180.0f, 180.0f)) {
                state.config.physics.initial_angle1 = deg2rad(angle1_deg);
            }

            float angle2_deg = rad2deg(state.config.physics.initial_angle2);
            if (ImGui::SliderFloat("Initial Angle 2", &angle2_deg, -180.0f, 180.0f)) {
                state.config.physics.initial_angle2 = deg2rad(angle2_deg);
            }

            float vel1 = static_cast<float>(state.config.physics.initial_velocity1);
            if (ImGui::SliderFloat("Initial Vel 1", &vel1, -10.0f, 10.0f)) {
                state.config.physics.initial_velocity1 = vel1;
            }

            float vel2 = static_cast<float>(state.config.physics.initial_velocity2);
            if (ImGui::SliderFloat("Initial Vel 2", &vel2, -10.0f, 10.0f)) {
                state.config.physics.initial_velocity2 = vel2;
            }
        }

        // Simulation parameters
        if (ImGui::CollapsingHeader("Simulation")) {
            float variation_deg = rad2deg(state.config.simulation.angle_variation);
            if (ImGui::SliderFloat("Angle Variation", &variation_deg, 0.001f, 5.0f, "%.3f deg")) {
                state.config.simulation.angle_variation = deg2rad(variation_deg);
            }

            float duration = static_cast<float>(state.config.simulation.duration_seconds);
            if (ImGui::SliderFloat("Duration (s)", &duration, 1.0f, 60.0f)) {
                state.config.simulation.duration_seconds = duration;
            }

            ImGui::SliderInt("Total Frames", &state.config.simulation.total_frames, 60, 3600);
            ImGui::SliderInt("Substeps/Frame", &state.config.simulation.substeps_per_frame, 1, 100);

            // Display computed dt
            ImGui::Text("dt = %.6f s", state.config.simulation.dt());
        }

        // Color parameters
        if (ImGui::CollapsingHeader("Color")) {
            char const* schemes[] = { "Spectrum", "Rainbow", "Heat", "Cool", "Monochrome" };
            int scheme_idx = static_cast<int>(state.config.color.scheme);
            if (ImGui::Combo("Color Scheme", &scheme_idx, schemes, 5)) {
                state.config.color.scheme = static_cast<ColorScheme>(scheme_idx);
            }

            float wl_start = static_cast<float>(state.config.color.wavelength_start);
            if (ImGui::SliderFloat("Wavelength Start", &wl_start, 380.0f, 700.0f, "%.0f nm")) {
                state.config.color.wavelength_start = wl_start;
            }

            float wl_end = static_cast<float>(state.config.color.wavelength_end);
            if (ImGui::SliderFloat("Wavelength End", &wl_end, 450.0f, 780.0f, "%.0f nm")) {
                state.config.color.wavelength_end = wl_end;
            }
        }

        // Post-processing parameters
        if (ImGui::CollapsingHeader("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
            float gamma = static_cast<float>(state.config.post_process.gamma);
            if (ImGui::SliderFloat("Gamma", &gamma, 0.5f, 2.5f)) {
                state.config.post_process.gamma = gamma;
            }

            float brightness = static_cast<float>(state.config.post_process.target_brightness);
            if (ImGui::SliderFloat("Brightness", &brightness, 0.1f, 1.0f)) {
                state.config.post_process.target_brightness = brightness;
            }

            float contrast = static_cast<float>(state.config.post_process.contrast);
            if (ImGui::SliderFloat("Contrast", &contrast, 0.5f, 2.0f)) {
                state.config.post_process.contrast = contrast;
            }
        }

        // Detection thresholds
        if (ImGui::CollapsingHeader("Detection")) {
            float boom_thresh = static_cast<float>(state.config.detection.boom_threshold);
            if (ImGui::SliderFloat("Boom Threshold", &boom_thresh, 0.01f, 1.0f, "%.3f rad^2")) {
                state.config.detection.boom_threshold = boom_thresh;
            }
            ImGui::SliderInt("Boom Confirm", &state.config.detection.boom_confirmation, 1, 30);

            float white_thresh = static_cast<float>(state.config.detection.white_threshold);
            if (ImGui::InputFloat("White Threshold", &white_thresh, 10.0f, 100.0f, "%.1f rad^2")) {
                state.config.detection.white_threshold = white_thresh;
            }
            ImGui::SliderInt("White Confirm", &state.config.detection.white_confirmation, 1, 30);
        }

        ImGui::End();

        // Preview window
        ImGui::Begin("Preview");
        ImVec2 preview_size(static_cast<float>(renderer.width()), static_cast<float>(renderer.height()));
        // Flip UV vertically: OpenGL texture origin is bottom-left, ImGui expects top-left
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(renderer.getTextureID())), preview_size, ImVec2(0, 1), ImVec2(1, 0));
        ImGui::End();

        // Variance graph
        ImGui::Begin("Variance");
        ImVec2 graph_size = ImGui::GetContentRegionAvail();
        graph_size.y = std::max(100.0f, graph_size.y);
        drawVarianceGraph(state, graph_size);
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
