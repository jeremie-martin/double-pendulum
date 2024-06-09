#pragma once

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string_view>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Clamp function to keep values in the range [0, 255]
unsigned char clamp(float value) { return static_cast<unsigned char>(round(std::max(0.0f, std::min(255.0f, value)))); }

struct Color
{
    float r, g, b;
};

class Image
{
public:
    int width, height;
    std::vector<float> data;

    Image(int w, int h)
        : width(w)
        , height(h)
        , data(w * h * 3, 0) { }

    Color get_pixel(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return { 0, 0, 0 };

        int index = (y * width + x) * 3;
        return { data[index], data[index + 1], data[index + 2] };
    }

    void set_pixel(int x, int y, Color const& color) {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;

        int index = (y * width + x) * 3;
        data[index] = color.r;
        data[index + 1] = color.g;
        data[index + 2] = color.b;
    }

    void draw_pixel(int x, int y, Color const& color, float intensity) {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;

        int index = (y * width + x) * 3;

        data[index] += color.r * intensity;
        data[index + 1] += color.g * intensity;
        data[index + 2] += color.b * intensity;
    }

    void draw_line(int x0, int y0, int x1, int y1, Color const& color) {
        auto ipart = [](float x) { return std::floor(x); };
        auto fpart = [](float x) { return x - std::floor(x); };
        auto rfpart = [fpart](float x) { return 1.0 - fpart(x); };

        bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
        if (steep) {
            std::swap(x0, y0);
            std::swap(x1, y1);
        }
        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }

        float dx = x1 - x0;
        float dy = y1 - y0;
        float gradient = dy / dx;
        if (dx == 0.0)
            gradient = 1.0;

        int xend = ipart(x0 + 0.5);
        float yend = y0 + gradient * (xend - x0);
        float xgap = rfpart(x0 + 0.5);
        int xpxl1 = xend;
        int ypxl1 = ipart(yend);

        if (steep) {
            draw_pixel(ypxl1, xpxl1, color, rfpart(yend) * xgap);
            draw_pixel(ypxl1 + 1, xpxl1, color, fpart(yend) * xgap);
        }
        else {
            draw_pixel(xpxl1, ypxl1, color, rfpart(yend) * xgap);
            draw_pixel(xpxl1, ypxl1 + 1, color, fpart(yend) * xgap);
        }

        float intery = yend + gradient;

        xend = ipart(x1 + 0.5);
        yend = y1 + gradient * (xend - x1);
        xgap = fpart(x1 + 0.5);
        int xpxl2 = xend;
        int ypxl2 = ipart(yend);

        if (steep) {
            draw_pixel(ypxl2, xpxl2, color, rfpart(yend) * xgap);
            draw_pixel(ypxl2 + 1, xpxl2, color, fpart(yend) * xgap);
        }
        else {
            draw_pixel(xpxl2, ypxl2, color, rfpart(yend) * xgap);
            draw_pixel(xpxl2, ypxl2 + 1, color, fpart(yend) * xgap);
        }

        if (steep) {
            for (int x = xpxl1 + 1; x < xpxl2; x++) {
                draw_pixel(ipart(intery), x, color, rfpart(intery));
                draw_pixel(ipart(intery) + 1, x, color, fpart(intery));
                intery += gradient;
            }
        }
        else {
            for (int x = xpxl1 + 1; x < xpxl2; x++) {
                draw_pixel(x, ipart(intery), color, rfpart(intery));
                draw_pixel(x, ipart(intery) + 1, color, fpart(intery));
                intery += gradient;
            }
        }
    }

    void save_to_png(char const* file_path) {
        std::vector<unsigned char> image_data_8bit(width * height * 3);
        for (int i = 0; i < image_data_8bit.size(); ++i) {
            image_data_8bit[i] = clamp(data[i]);
            // float x = std::max(0.0f, std::min(255.0f, data[i]));
            // image_data_8bit[i] = static_cast<unsigned char>(x);
        }
        stbi_write_png(file_path, width, height, 3, image_data_8bit.data(), width * 3);
    }
};

double adjust_color(double color, double factor, double gamma, double intensity_max) {
    if (color == 0.0) {
        return 0.0;
    }
    else {
        return intensity_max * std::pow(color * factor, gamma);
    }
}

Color wavelengthToRGB(float wavelength) {
    Color color = { 0.0f, 0.0f, 0.0f };

    if (wavelength >= 380 && wavelength < 440) {
        color.r = -(wavelength - 440) / (440 - 380);
        color.g = 0.0f;
        color.b = 1.0f;
    }
    else if (wavelength >= 440 && wavelength < 490) {
        color.r = 0.0f;
        color.g = (wavelength - 440) / (490 - 440);
        color.b = 1.0f;
    }
    else if (wavelength >= 490 && wavelength < 510) {
        color.r = 0.0f;
        color.g = 1.0f;
        color.b = -(wavelength - 510) / (510 - 490);
    }
    else if (wavelength >= 510 && wavelength < 580) {
        color.r = (wavelength - 510) / (580 - 510);
        color.g = 1.0f;
        color.b = 0.0f;
    }
    else if (wavelength >= 580 && wavelength < 645) {
        color.r = 1.0f;
        color.g = -(wavelength - 645) / (645 - 580);
        color.b = 0.0f;
    }
    else if (wavelength >= 645 && wavelength <= 780) {
        color.r = 1.0f;
        color.g = 0.0f;
        color.b = 0.0f;
    }

    // Intensity correction
    float intensity;
    if (wavelength >= 380 && wavelength < 420) {
        intensity = 0.3f + 0.7f * (wavelength - 380) / (420 - 380);
    }
    else if (wavelength >= 420 && wavelength < 645) {
        intensity = 1.0f;
    }
    else if (wavelength >= 645 && wavelength <= 780) {
        intensity = 0.3f + 0.7f * (780 - wavelength) / (780 - 645);
    }
    else {
        intensity = 0.0f; // Outside visible range
    }

    color.r = std::pow(color.r * intensity, 0.8f);
    color.g = std::pow(color.g * intensity, 0.8f);
    color.b = std::pow(color.b * intensity, 0.8f);

    return color;
}