#include "gl_renderer.h"
#include <GL/glew.h>
#include <cmath>
#include <algorithm>
#include <iostream>

// Vertex shader - simple passthrough
static const char* vertex_shader_src = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aColor;
out vec4 vertexColor;

uniform vec2 uResolution;

void main() {
    // Convert from pixel coordinates to NDC (-1 to 1)
    vec2 ndc = (aPos / uResolution) * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y
    gl_Position = vec4(ndc, 0.0, 1.0);
    vertexColor = aColor;
}
)";

// Fragment shader - output color directly (additive blending done by GL)
static const char* fragment_shader_src = R"(
#version 330 core
in vec4 vertexColor;
out vec4 FragColor;

void main() {
    FragColor = vertexColor;
}
)";

GLRenderer::GLRenderer() {}

GLRenderer::~GLRenderer() {
    shutdown();
}

bool GLRenderer::init(int width, int height) {
    width_ = width;
    height_ = height;

    // Initialize GLEW
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW\n";
        return false;
    }

    if (!createFramebuffer()) {
        return false;
    }

    if (!createShaders()) {
        return false;
    }

    // Create VAO and VBO for line drawing
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    float_buffer_.resize(width * height * 4);

    return true;
}

void GLRenderer::shutdown() {
    deleteFramebuffer();

    if (shader_program_) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
}

bool GLRenderer::createFramebuffer() {
    // Create floating-point texture for accumulation
    glGenTextures(1, &float_texture_);
    glBindTexture(GL_TEXTURE_2D, float_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create framebuffer
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, float_texture_, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Framebuffer not complete!\n";
        return false;
    }

    // Create display texture (8-bit for ImGui)
    glGenTextures(1, &display_texture_);
    glBindTexture(GL_TEXTURE_2D, display_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return true;
}

bool GLRenderer::createShaders() {
    // Compile vertex shader
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_src, nullptr);
    glCompileShader(vertex_shader);

    GLint success;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(vertex_shader, 512, nullptr, log);
        std::cerr << "Vertex shader error: " << log << "\n";
        return false;
    }

    // Compile fragment shader
    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_src, nullptr);
    glCompileShader(fragment_shader);

    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(fragment_shader, 512, nullptr, log);
        std::cerr << "Fragment shader error: " << log << "\n";
        return false;
    }

    // Link program
    shader_program_ = glCreateProgram();
    glAttachShader(shader_program_, vertex_shader);
    glAttachShader(shader_program_, fragment_shader);
    glLinkProgram(shader_program_);

    glGetProgramiv(shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(shader_program_, 512, nullptr, log);
        std::cerr << "Shader link error: " << log << "\n";
        return false;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    return true;
}

void GLRenderer::deleteFramebuffer() {
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (float_texture_) {
        glDeleteTextures(1, &float_texture_);
        float_texture_ = 0;
    }
    if (display_texture_) {
        glDeleteTextures(1, &display_texture_);
        display_texture_ = 0;
    }
}

void GLRenderer::resize(int width, int height) {
    if (width == width_ && height == height_) return;

    width_ = width;
    height_ = height;

    deleteFramebuffer();
    createFramebuffer();

    float_buffer_.resize(width * height * 4);
}

void GLRenderer::clear() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLRenderer::drawLine(float x0, float y0, float x1, float y1,
                          float r, float g, float b, float intensity) {
    // Rasterize line with anti-aliasing
    auto pixels = rasterizeLine(
        static_cast<int>(x0), static_cast<int>(y0),
        static_cast<int>(x1), static_cast<int>(y1)
    );

    if (pixels.empty()) return;

    // Build vertex data: each pixel becomes a point
    std::vector<float> vertices;
    vertices.reserve(pixels.size() * 6);

    for (auto const& p : pixels) {
        float px = static_cast<float>(p.x) + 0.5f;
        float py = static_cast<float>(p.y) + 0.5f;
        float alpha = p.intensity * intensity;

        vertices.push_back(px);
        vertices.push_back(py);
        vertices.push_back(r * alpha);
        vertices.push_back(g * alpha);
        vertices.push_back(b * alpha);
        vertices.push_back(alpha);
    }

    // Render to floating-point framebuffer with additive blending
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);  // Additive blending

    glUseProgram(shader_program_);
    glUniform2f(glGetUniformLocation(shader_program_, "uResolution"),
                static_cast<float>(width_), static_cast<float>(height_));

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_DYNAMIC_DRAW);

    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(pixels.size()));

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLRenderer::readPixels(std::vector<uint8_t>& out, float gamma) {
    out.resize(width_ * height_ * 3);

    // Read floating-point data
    glBindTexture(GL_TEXTURE_2D, float_texture_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, float_buffer_.data());

    // Find max value for normalization
    float max_val = 0.0f;
    for (size_t i = 0; i < float_buffer_.size(); i += 4) {
        max_val = std::max(max_val, float_buffer_[i]);
        max_val = std::max(max_val, float_buffer_[i + 1]);
        max_val = std::max(max_val, float_buffer_[i + 2]);
    }

    if (max_val < 0.001f) max_val = 1.0f;

    // Normalize and apply gamma
    float inv_gamma = 1.0f / gamma;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            size_t src_idx = (y * width_ + x) * 4;
            size_t dst_idx = (y * width_ + x) * 3;

            for (int c = 0; c < 3; ++c) {
                float v = float_buffer_[src_idx + c] / max_val;
                v = std::pow(v, inv_gamma);
                v = std::max(0.0f, std::min(1.0f, v));
                out[dst_idx + c] = static_cast<uint8_t>(v * 255.0f);
            }
        }
    }
}

void GLRenderer::updateDisplayTexture(float gamma, float brightness_target) {
    // Read floating-point data
    glBindTexture(GL_TEXTURE_2D, float_texture_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, float_buffer_.data());

    // Find max and average for normalization
    float max_val = 0.0f;
    double sum = 0.0;
    size_t count = 0;

    for (size_t i = 0; i < float_buffer_.size(); i += 4) {
        float lum = 0.299f * float_buffer_[i] + 0.587f * float_buffer_[i+1] + 0.114f * float_buffer_[i+2];
        max_val = std::max(max_val, lum);
        sum += lum;
        count++;
    }

    float avg = static_cast<float>(sum / count);
    if (max_val < 0.001f) max_val = 1.0f;
    if (avg < 0.001f) avg = 0.001f;

    // Scale to target brightness
    float scale = brightness_target / avg;
    scale = std::min(scale, 255.0f / max_val);  // Don't clip

    // Create 8-bit buffer
    std::vector<uint8_t> rgba(width_ * height_ * 4);

    float inv_gamma = 1.0f / gamma;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            size_t src_idx = (y * width_ + x) * 4;
            size_t dst_idx = (y * width_ + x) * 4;

            for (int c = 0; c < 3; ++c) {
                float v = float_buffer_[src_idx + c] * scale / 255.0f;
                v = std::pow(v, inv_gamma);
                v = std::max(0.0f, std::min(1.0f, v));
                rgba[dst_idx + c] = static_cast<uint8_t>(v * 255.0f);
            }
            rgba[dst_idx + 3] = 255;
        }
    }

    // Upload to display texture
    glBindTexture(GL_TEXTURE_2D, display_texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

// Xiaolin Wu anti-aliased line rasterization with intensity normalization
std::vector<LinePixel> rasterizeLine(int x0, int y0, int x1, int y1) {
    std::vector<LinePixel> pixels;

    auto ipart = [](float x) { return std::floor(x); };
    auto fpart = [](float x) { return x - std::floor(x); };
    auto rfpart = [&fpart](float x) { return 1.0f - fpart(x); };

    bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep) {
        std::swap(x0, y0);
        std::swap(x1, y1);
    }
    if (x0 > x1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }

    float dx = static_cast<float>(x1 - x0);
    float dy = static_cast<float>(y1 - y0);
    float gradient = (dx == 0.0f) ? 1.0f : dy / dx;

    // Intensity correction for diagonal lines
    float intensity_scale = std::sqrt(1.0f + gradient * gradient);

    // First endpoint
    int xend = static_cast<int>(ipart(x0 + 0.5f));
    float yend = y0 + gradient * (xend - x0);
    float xgap = rfpart(x0 + 0.5f);
    int xpxl1 = xend;
    int ypxl1 = static_cast<int>(ipart(yend));

    if (steep) {
        pixels.push_back({ypxl1, xpxl1, rfpart(yend) * xgap * intensity_scale});
        pixels.push_back({ypxl1 + 1, xpxl1, fpart(yend) * xgap * intensity_scale});
    } else {
        pixels.push_back({xpxl1, ypxl1, rfpart(yend) * xgap * intensity_scale});
        pixels.push_back({xpxl1, ypxl1 + 1, fpart(yend) * xgap * intensity_scale});
    }

    float intery = yend + gradient;

    // Second endpoint
    xend = static_cast<int>(ipart(x1 + 0.5f));
    yend = y1 + gradient * (xend - x1);
    xgap = fpart(x1 + 0.5f);
    int xpxl2 = xend;
    int ypxl2 = static_cast<int>(ipart(yend));

    if (steep) {
        pixels.push_back({ypxl2, xpxl2, rfpart(yend) * xgap * intensity_scale});
        pixels.push_back({ypxl2 + 1, xpxl2, fpart(yend) * xgap * intensity_scale});
    } else {
        pixels.push_back({xpxl2, ypxl2, rfpart(yend) * xgap * intensity_scale});
        pixels.push_back({xpxl2, ypxl2 + 1, fpart(yend) * xgap * intensity_scale});
    }

    // Main loop
    if (steep) {
        for (int x = xpxl1 + 1; x < xpxl2; ++x) {
            pixels.push_back({static_cast<int>(ipart(intery)), x, rfpart(intery) * intensity_scale});
            pixels.push_back({static_cast<int>(ipart(intery)) + 1, x, fpart(intery) * intensity_scale});
            intery += gradient;
        }
    } else {
        for (int x = xpxl1 + 1; x < xpxl2; ++x) {
            pixels.push_back({x, static_cast<int>(ipart(intery)), rfpart(intery) * intensity_scale});
            pixels.push_back({x, static_cast<int>(ipart(intery)) + 1, fpart(intery) * intensity_scale});
            intery += gradient;
        }
    }

    return pixels;
}
