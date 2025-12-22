#include "gl_renderer.h"
#include "post_process.h"

#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <iostream>

// Vertex shader - expands line quad and passes line parameters to fragment
static const char* vertex_shader_src = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aColor;
layout (location = 2) in vec2 aLineDir;      // Normalized line direction
layout (location = 3) in float aLineDist;    // Signed distance from line center (-1 to 1 across width)

out vec4 vertexColor;
out float lineDist;

uniform vec2 uResolution;

void main() {
    // Convert from pixel coordinates to NDC (-1 to 1)
    vec2 ndc = (aPos / uResolution) * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y
    gl_Position = vec4(ndc, 0.0, 1.0);
    vertexColor = aColor;
    lineDist = aLineDist;
}
)";

// Fragment shader - applies smooth anti-aliasing based on distance from line center
static const char* fragment_shader_src = R"(
#version 330 core
in vec4 vertexColor;
in float lineDist;
out vec4 FragColor;

void main() {
    // Smooth falloff for anti-aliasing
    // lineDist is -1 to 1 across the line width
    // Use smoothstep for soft edges at the boundary
    float alpha = 1.0 - smoothstep(0.5, 1.0, abs(lineDist));
    FragColor = vertexColor * alpha;
}
)";

// Post-processing vertex shader - fullscreen triangle
static const char* pp_vertex_shader_src = R"(
#version 330 core
out vec2 TexCoord;

void main() {
    // Fullscreen triangle trick: 3 vertices cover the screen
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 texcoords[3] = vec2[](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0)
    );
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    TexCoord = texcoords[gl_VertexID];
}
)";

// Post-processing fragment shader - full pipeline on GPU
static const char* pp_fragment_shader_src = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D uFloatTexture;
uniform float uMaxVal;
uniform float uExposureMult;
uniform float uContrast;
uniform float uInvGamma;
uniform int uToneMapOp;
uniform float uWhitePoint;

// Tone mapping functions
float toneMapReinhard(float v) {
    return v / (1.0 + v);
}

float toneMapReinhardExtended(float v, float wp) {
    float w2 = wp * wp;
    return (v * (1.0 + v / w2)) / (1.0 + v);
}

float toneMapACES(float v) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((v * (a * v + b)) / (v * (c * v + d) + e), 0.0, 1.0);
}

float toneMapLogarithmic(float v, float wp) {
    return log(1.0 + v) / log(1.0 + wp);
}

float toneMap(float v, int op, float wp) {
    if (op == 1) return toneMapReinhard(v);
    if (op == 2) return toneMapReinhardExtended(v, wp);
    if (op == 3) return toneMapACES(v);
    if (op == 4) return toneMapLogarithmic(v, wp);
    return clamp(v, 0.0, 1.0); // None
}

void main() {
    // Sample float texture (flip Y for correct orientation)
    vec2 flippedCoord = vec2(TexCoord.x, 1.0 - TexCoord.y);
    vec4 hdr = texture(uFloatTexture, flippedCoord);

    vec3 color;
    for (int c = 0; c < 3; c++) {
        float v = hdr[c];

        // 1. Normalize
        v = v / uMaxVal;

        // 2. Exposure
        v = v * uExposureMult;

        // 3. Tone mapping
        v = toneMap(v, uToneMapOp, uWhitePoint);

        // 4. Contrast
        v = (v - 0.5) * uContrast + 0.5;

        // 5. Clamp
        v = clamp(v, 0.0, 1.0);

        // 6. Gamma
        v = pow(v, uInvGamma);

        color[c] = v;
    }

    FragColor = vec4(color, 1.0);
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

    // Vertex format: pos(2) + color(4) + lineDir(2) + lineDist(1) = 9 floats
    size_t stride = 9 * sizeof(float);

    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Line direction attribute
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // Line distance attribute
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);

    // Create empty VAO for post-processing (fullscreen triangle uses gl_VertexID)
    glGenVertexArrays(1, &pp_vao_);

    if (!createPostProcessShader()) {
        return false;
    }

    float_buffer_.resize(width * height * 4);

    return true;
}

void GLRenderer::shutdown() {
    deleteFramebuffer();

    if (shader_program_) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }
    if (pp_shader_program_) {
        glDeleteProgram(pp_shader_program_);
        pp_shader_program_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (pp_vao_) {
        glDeleteVertexArrays(1, &pp_vao_);
        pp_vao_ = 0;
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Create FBO for rendering to display texture (post-processing output)
    glGenFramebuffers(1, &display_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, display_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, display_texture_, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Display framebuffer not complete!\n";
        return false;
    }

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

bool GLRenderer::createPostProcessShader() {
    // Compile vertex shader
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &pp_vertex_shader_src, nullptr);
    glCompileShader(vertex_shader);

    GLint success;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(vertex_shader, 512, nullptr, log);
        std::cerr << "PP vertex shader error: " << log << "\n";
        return false;
    }

    // Compile fragment shader
    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &pp_fragment_shader_src, nullptr);
    glCompileShader(fragment_shader);

    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(fragment_shader, 512, nullptr, log);
        std::cerr << "PP fragment shader error: " << log << "\n";
        return false;
    }

    // Link program
    pp_shader_program_ = glCreateProgram();
    glAttachShader(pp_shader_program_, vertex_shader);
    glAttachShader(pp_shader_program_, fragment_shader);
    glLinkProgram(pp_shader_program_);

    glGetProgramiv(pp_shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(pp_shader_program_, 512, nullptr, log);
        std::cerr << "PP shader link error: " << log << "\n";
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
    if (display_fbo_) {
        glDeleteFramebuffers(1, &display_fbo_);
        display_fbo_ = 0;
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
    if (width == width_ && height == height_)
        return;

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

    // Clear line buffer
    line_buffer_.clear();
}

void GLRenderer::drawLine(float x0, float y0, float x1, float y1, float r, float g, float b,
                          float intensity) {
    // Buffer the line for batch rendering
    line_buffer_.push_back({x0, y0, x1, y1, r, g, b, intensity});
}

void GLRenderer::flush() {
    if (line_buffer_.empty())
        return;

    // Line thickness in pixels (for AA coverage)
    const float thickness = 1.5f;

    // Build vertex buffer - 6 vertices per line (2 triangles forming a quad)
    // Each vertex: pos(2) + color(4) + lineDir(2) + lineDist(1) = 9 floats
    vertex_buffer_.clear();
    vertex_buffer_.reserve(line_buffer_.size() * 6 * 9);

    for (const auto& line : line_buffer_) {
        float dx = line.x1 - line.x0;
        float dy = line.y1 - line.y0;
        float len = std::sqrt(dx * dx + dy * dy);

        if (len < 0.001f)
            continue; // Skip degenerate lines

        // Normalized direction along line
        float dirX = dx / len;
        float dirY = dy / len;

        // Perpendicular direction (for thickness)
        float perpX = -dirY * thickness;
        float perpY = dirX * thickness;

        // Color with intensity (no angle-based correction needed for GPU quad rendering)
        float cr = line.r * line.intensity;
        float cg = line.g * line.intensity;
        float cb = line.b * line.intensity;
        float ca = line.intensity;

        // Four corners of the quad
        float p0x = line.x0 - perpX;
        float p0y = line.y0 - perpY;
        float p1x = line.x0 + perpX;
        float p1y = line.y0 + perpY;
        float p2x = line.x1 - perpX;
        float p2y = line.y1 - perpY;
        float p3x = line.x1 + perpX;
        float p3y = line.y1 + perpY;

        // Helper to add a vertex
        auto addVertex = [&](float px, float py, float dist) {
            vertex_buffer_.push_back(px);
            vertex_buffer_.push_back(py);
            vertex_buffer_.push_back(cr);
            vertex_buffer_.push_back(cg);
            vertex_buffer_.push_back(cb);
            vertex_buffer_.push_back(ca);
            vertex_buffer_.push_back(dirX);
            vertex_buffer_.push_back(dirY);
            vertex_buffer_.push_back(dist);
        };

        // Triangle 1: p0, p1, p2
        addVertex(p0x, p0y, -1.0f);
        addVertex(p1x, p1y, 1.0f);
        addVertex(p2x, p2y, -1.0f);

        // Triangle 2: p1, p3, p2
        addVertex(p1x, p1y, 1.0f);
        addVertex(p3x, p3y, 1.0f);
        addVertex(p2x, p2y, -1.0f);
    }

    if (vertex_buffer_.empty())
        return;

    // Render all lines in one draw call
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE); // Additive blending

    glUseProgram(shader_program_);
    glUniform2f(glGetUniformLocation(shader_program_, "uResolution"), static_cast<float>(width_),
                static_cast<float>(height_));

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, vertex_buffer_.size() * sizeof(float), vertex_buffer_.data(),
                 GL_DYNAMIC_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertex_buffer_.size() / 9));

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Clear line buffer after flush
    line_buffer_.clear();
}

void GLRenderer::readPixels(std::vector<uint8_t>& out, float exposure, float contrast, float gamma,
                            ToneMapOperator tone_map, float white_point) {
    // Process on GPU first
    updateDisplayTexture(exposure, contrast, gamma, tone_map, white_point);

    // Read back 8-bit display texture (4x smaller transfer than float texture)
    std::vector<uint8_t> rgba(static_cast<size_t>(width_) * height_ * 4);
    glBindTexture(GL_TEXTURE_2D, display_texture_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // Convert RGBA to RGB (shader already flipped Y)
    out.resize(static_cast<size_t>(width_) * height_ * 3);
    for (int i = 0; i < width_ * height_; ++i) {
        out[i * 3 + 0] = rgba[i * 4 + 0];
        out[i * 3 + 1] = rgba[i * 4 + 1];
        out[i * 3 + 2] = rgba[i * 4 + 2];
    }
}

void GLRenderer::updateDisplayTexture(float exposure, float contrast, float gamma,
                                      ToneMapOperator tone_map, float white_point) {
    // Flush any pending lines first
    flush();

    // Read float texture to find max value (CPU - required for proper normalization)
    glBindTexture(GL_TEXTURE_2D, float_texture_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, float_buffer_.data());

    float max_val = 0.0f;
    for (size_t i = 0; i < float_buffer_.size(); i += 4) {
        max_val = std::max(max_val, float_buffer_[i]);
        max_val = std::max(max_val, float_buffer_[i + 1]);
        max_val = std::max(max_val, float_buffer_[i + 2]);
    }
    if (max_val < 1e-6f) {
        max_val = 1.0f;
    }

    // Precompute shader uniforms
    float exposure_mult = std::pow(2.0f, exposure);
    float inv_gamma = 1.0f / gamma;
    int tone_map_op = static_cast<int>(tone_map);

    // Render post-processing on GPU
    glBindFramebuffer(GL_FRAMEBUFFER, display_fbo_);
    glViewport(0, 0, width_, height_);
    glDisable(GL_BLEND);

    glUseProgram(pp_shader_program_);

    // Set uniforms
    glUniform1f(glGetUniformLocation(pp_shader_program_, "uMaxVal"), max_val);
    glUniform1f(glGetUniformLocation(pp_shader_program_, "uExposureMult"), exposure_mult);
    glUniform1f(glGetUniformLocation(pp_shader_program_, "uContrast"), contrast);
    glUniform1f(glGetUniformLocation(pp_shader_program_, "uInvGamma"), inv_gamma);
    glUniform1i(glGetUniformLocation(pp_shader_program_, "uToneMapOp"), tone_map_op);
    glUniform1f(glGetUniformLocation(pp_shader_program_, "uWhitePoint"), white_point);

    // Bind float texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, float_texture_);
    glUniform1i(glGetUniformLocation(pp_shader_program_, "uFloatTexture"), 0);

    // Draw fullscreen triangle
    glBindVertexArray(pp_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
