#pragma once

#include "color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Float image buffer for CPU post-processing (used by GUI preview when needed)
class Image {
public:
    int width, height;
    std::vector<float> data;

    Image(int w, int h) : width(w), height(h), data(w * h * 3, 0.0f) {}

    void clear() { std::fill(data.begin(), data.end(), 0.0f); }

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
