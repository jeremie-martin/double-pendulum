#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct Color {
    float r, g, b;

    Color operator*(float s) const { return {r * s, g * s, b * s}; }
    Color operator+(Color const& o) const { return {r + o.r, g + o.g, b + o.b}; }
};

class Image {
public:
    int width, height;
    std::vector<float> data;

    Image(int w, int h)
        : width(w), height(h), data(w * h * 3, 0.0f) {}

    void clear() {
        std::fill(data.begin(), data.end(), 0.0f);
    }

    Color get_pixel(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return {0, 0, 0};

        int index = (y * width + x) * 3;
        return {data[index], data[index + 1], data[index + 2]};
    }

    void set_pixel(int x, int y, Color const& color) {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;

        int index = (y * width + x) * 3;
        data[index] = color.r;
        data[index + 1] = color.g;
        data[index + 2] = color.b;
    }

    void add_pixel(int x, int y, Color const& color, float intensity) {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;

        int index = (y * width + x) * 3;
        data[index] += color.r * intensity;
        data[index + 1] += color.g * intensity;
        data[index + 2] += color.b * intensity;
    }

    // Xiaolin Wu's anti-aliased line drawing algorithm
    // With intensity normalization to fix diagonal dimming (rasterization bias)
    void draw_line(int x0, int y0, int x1, int y1, Color const& color) {
        auto ipart = [](float x) { return std::floor(x); };
        auto fpart = [](float x) { return x - std::floor(x); };
        auto rfpart = [fpart](float x) { return 1.0f - fpart(x); };

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

        // Intensity correction: diagonal lines cover more visual distance per pixel step
        // Scale by sqrt(1 + gradient²) to normalize brightness per unit length
        float intensity_scale = std::sqrt(1.0f + gradient * gradient);

        // Handle first endpoint
        int xend = static_cast<int>(ipart(x0 + 0.5f));
        float yend = y0 + gradient * (xend - x0);
        float xgap = rfpart(x0 + 0.5f);
        int xpxl1 = xend;
        int ypxl1 = static_cast<int>(ipart(yend));

        if (steep) {
            add_pixel(ypxl1, xpxl1, color, rfpart(yend) * xgap * intensity_scale);
            add_pixel(ypxl1 + 1, xpxl1, color, fpart(yend) * xgap * intensity_scale);
        } else {
            add_pixel(xpxl1, ypxl1, color, rfpart(yend) * xgap * intensity_scale);
            add_pixel(xpxl1, ypxl1 + 1, color, fpart(yend) * xgap * intensity_scale);
        }

        float intery = yend + gradient;

        // Handle second endpoint
        xend = static_cast<int>(ipart(x1 + 0.5f));
        yend = y1 + gradient * (xend - x1);
        xgap = fpart(x1 + 0.5f);
        int xpxl2 = xend;
        int ypxl2 = static_cast<int>(ipart(yend));

        if (steep) {
            add_pixel(ypxl2, xpxl2, color, rfpart(yend) * xgap * intensity_scale);
            add_pixel(ypxl2 + 1, xpxl2, color, fpart(yend) * xgap * intensity_scale);
        } else {
            add_pixel(xpxl2, ypxl2, color, rfpart(yend) * xgap * intensity_scale);
            add_pixel(xpxl2, ypxl2 + 1, color, fpart(yend) * xgap * intensity_scale);
        }

        // Main loop
        if (steep) {
            for (int x = xpxl1 + 1; x < xpxl2; ++x) {
                add_pixel(static_cast<int>(ipart(intery)), x, color, rfpart(intery) * intensity_scale);
                add_pixel(static_cast<int>(ipart(intery)) + 1, x, color, fpart(intery) * intensity_scale);
                intery += gradient;
            }
        } else {
            for (int x = xpxl1 + 1; x < xpxl2; ++x) {
                add_pixel(x, static_cast<int>(ipart(intery)), color, rfpart(intery) * intensity_scale);
                add_pixel(x, static_cast<int>(ipart(intery)) + 1, color, fpart(intery) * intensity_scale);
                intery += gradient;
            }
        }
    }

    // Convert to 8-bit RGB buffer
    void to_rgb8(std::vector<uint8_t>& out) const {
        out.resize(width * height * 3);
        for (size_t i = 0; i < data.size(); ++i) {
            float v = std::max(0.0f, std::min(255.0f, data[i]));
            out[i] = static_cast<uint8_t>(std::round(v));
        }
    }

    // Get raw data for direct manipulation
    float* raw_data() { return data.data(); }
    float const* raw_data() const { return data.data(); }
    size_t pixel_count() const { return width * height; }
};

// Forward declarations for rendering functions
struct PendulumState;

class Renderer {
public:
    Renderer(int width, int height)
        : width_(width), height_(height),
          centerX_(width / 2), centerY_(height / 2),
          scale_(width / 5.0) {}

    // Render a single pendulum state to the image
    void render_pendulum(Image& image, PendulumState const& state, Color const& color) const {
        int x0 = centerX_;
        int y0 = centerY_;
        int x1 = static_cast<int>(centerX_ + state.x1 * scale_);
        int y1 = static_cast<int>(centerY_ + state.y1 * scale_);
        int x2 = static_cast<int>(centerX_ + state.x2 * scale_);
        int y2 = static_cast<int>(centerY_ + state.y2 * scale_);

        image.draw_line(x0, y0, x1, y1, color);
        image.draw_line(x1, y1, x2, y2, color);
    }

private:
    int width_, height_;
    int centerX_, centerY_;
    double scale_;
};
