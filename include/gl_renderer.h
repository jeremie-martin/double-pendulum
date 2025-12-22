#pragma once

#include <GL/glew.h>
#include <vector>
#include <cstdint>

// OpenGL renderer using GL_RGBA32F floating-point texture for accurate additive blending
// This maintains full precision for millions of pendulums, same as CPU float buffer
class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    // Initialize with framebuffer size
    bool init(int width, int height);
    void shutdown();

    // Resize framebuffer
    void resize(int width, int height);

    // Clear the framebuffer
    void clear();

    // Draw a line with additive blending (color values are added)
    void drawLine(float x0, float y0, float x1, float y1,
                  float r, float g, float b, float intensity = 1.0f);

    // Read back the framebuffer to CPU (for saving/display)
    // Applies normalization to map float values to 0-255
    void readPixels(std::vector<uint8_t>& out, float gamma = 1.0f);

    // Get the texture ID for ImGui display
    GLuint getTextureID() const { return display_texture_; }

    // Update display texture from float buffer (with normalization)
    void updateDisplayTexture(float gamma = 1.0f, float brightness_target = 0.5f);

    int width() const { return width_; }
    int height() const { return height_; }

private:
    int width_ = 0;
    int height_ = 0;

    // Floating-point framebuffer for accumulation
    GLuint fbo_ = 0;
    GLuint float_texture_ = 0;  // GL_RGBA32F

    // Display texture (GL_RGBA8 for ImGui)
    GLuint display_texture_ = 0;

    // Shader program for line drawing
    GLuint shader_program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;

    // CPU buffer for readback
    std::vector<float> float_buffer_;

    bool createFramebuffer();
    bool createShaders();
    void deleteFramebuffer();
};

// Xiaolin Wu anti-aliased line with intensity normalization (same as CPU version)
// Returns list of (x, y, intensity) tuples
struct LinePixel {
    int x, y;
    float intensity;
};

std::vector<LinePixel> rasterizeLine(int x0, int y0, int x1, int y1);
