#pragma once

#include "config.h"

#include <GL/glew.h>
#include <cstdint>
#include <vector>

// OpenGL renderer using GL_RGBA32F floating-point texture for accurate additive blending
// Uses GPU-native line rendering with shader-based anti-aliasing
class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    // Initialize with framebuffer size
    bool init(int width, int height);
    void shutdown();

    // Resize framebuffer
    void resize(int width, int height);

    // Clear the framebuffer and line buffer
    void clear();

    // Queue a line for rendering (batched, not drawn immediately)
    void drawLine(float x0, float y0, float x1, float y1, float r, float g, float b,
                  float intensity = 1.0f);

    // Flush all queued lines to the framebuffer
    void flush();

    // Read back the framebuffer to CPU (for saving/display)
    // Applies full post-processing pipeline: normalize -> exposure -> tone_map -> contrast -> gamma
    // normalization: PerFrame (divide by max) or ByCount (divide by pendulum_count)
    void readPixels(std::vector<uint8_t>& out, float exposure = 0.0f, float contrast = 1.0f,
                    float gamma = 2.2f, ToneMapOperator tone_map = ToneMapOperator::None,
                    float white_point = 1.0f,
                    NormalizationMode normalization = NormalizationMode::PerFrame,
                    int pendulum_count = 1);

    // Get the texture ID for ImGui display
    GLuint getTextureID() const { return display_texture_; }

    // Update display texture from float buffer (with standard post-processing)
    // Uses same pipeline as CPU: normalize -> exposure -> tone_map -> contrast -> gamma
    // normalization: PerFrame (divide by max) or ByCount (divide by pendulum_count)
    void updateDisplayTexture(float exposure = 0.0f, float contrast = 1.0f, float gamma = 2.2f,
                              ToneMapOperator tone_map = ToneMapOperator::None,
                              float white_point = 1.0f,
                              NormalizationMode normalization = NormalizationMode::PerFrame,
                              int pendulum_count = 1);

    int width() const { return width_; }
    int height() const { return height_; }

    // Get the last computed/used max value (for diagnostics)
    float lastMax() const { return last_max_; }

    // Get the last computed mean brightness (0-1 range, analysis mode)
    float lastBrightness() const { return last_brightness_; }

    // Get the last computed coverage (fraction of non-black pixels)
    float lastCoverage() const { return last_coverage_; }

    // Compute brightness/contrast metrics from current display texture
    // Lightweight version for GUI preview (doesn't return pixel data)
    void computeMetrics();

private:
    int width_ = 0;
    int height_ = 0;

    // Floating-point framebuffer for accumulation
    GLuint fbo_ = 0;
    GLuint float_texture_ = 0; // GL_RGBA32F

    // Display texture (GL_RGBA8 for ImGui)
    GLuint display_texture_ = 0;

    // Shader program for line drawing
    GLuint shader_program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;

    // Post-processing shader and resources
    GLuint pp_shader_program_ = 0;
    GLuint pp_vao_ = 0;
    GLuint display_fbo_ = 0;

    // Line data buffer (x0, y0, x1, y1, r, g, b, intensity per line)
    struct LineData {
        float x0, y0, x1, y1;
        float r, g, b;
        float intensity;
    };
    std::vector<LineData> line_buffer_;

    // Vertex buffer for rendering (expanded quads)
    std::vector<float> vertex_buffer_;

    // CPU buffer for readback
    std::vector<float> float_buffer_;

    // Compute shader for GPU max reduction (GL 4.3+)
    GLuint max_compute_shader_ = 0;
    GLuint max_ssbo_ = 0; // Shader storage buffer for reduction result
    bool has_compute_shaders_ = false;

    // Last computed/used max value (for diagnostics)
    float last_max_ = 0.0f;

    // Last computed mean brightness (0-1 range, analysis mode)
    float last_brightness_ = 0.0f;

    // Last computed coverage (fraction of non-black pixels)
    float last_coverage_ = 0.0f;

    bool createFramebuffer();
    bool createComputeShader();
    float computeMaxGPU(); // Returns max using compute shader
    bool createShaders();
    bool createPostProcessShader();
    void deleteFramebuffer();

    // Helper to compute metrics from RGBA data (avoids code duplication)
    void computeMetricsFromRGBA(uint8_t const* rgba, int pixel_count);
};
