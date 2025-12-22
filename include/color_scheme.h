#pragma once

#include "renderer.h"
#include "config.h"
#include <cmath>

// Visible light spectrum wavelength to RGB conversion
inline Color wavelengthToRGB(float wavelength) {
    Color color = {0.0f, 0.0f, 0.0f};

    if (wavelength >= 380 && wavelength < 440) {
        color.r = -(wavelength - 440) / (440 - 380);
        color.g = 0.0f;
        color.b = 1.0f;
    } else if (wavelength >= 440 && wavelength < 490) {
        color.r = 0.0f;
        color.g = (wavelength - 440) / (490 - 440);
        color.b = 1.0f;
    } else if (wavelength >= 490 && wavelength < 510) {
        color.r = 0.0f;
        color.g = 1.0f;
        color.b = -(wavelength - 510) / (510 - 490);
    } else if (wavelength >= 510 && wavelength < 580) {
        color.r = (wavelength - 510) / (580 - 510);
        color.g = 1.0f;
        color.b = 0.0f;
    } else if (wavelength >= 580 && wavelength < 645) {
        color.r = 1.0f;
        color.g = -(wavelength - 645) / (645 - 580);
        color.b = 0.0f;
    } else if (wavelength >= 645 && wavelength <= 780) {
        color.r = 1.0f;
        color.g = 0.0f;
        color.b = 0.0f;
    }

    // Intensity correction for edges of visible spectrum
    float intensity;
    if (wavelength >= 380 && wavelength < 420) {
        intensity = 0.3f + 0.7f * (wavelength - 380) / (420 - 380);
    } else if (wavelength >= 420 && wavelength < 645) {
        intensity = 1.0f;
    } else if (wavelength >= 645 && wavelength <= 780) {
        intensity = 0.3f + 0.7f * (780 - wavelength) / (780 - 645);
    } else {
        intensity = 0.0f;
    }

    // Apply gamma correction
    color.r = std::pow(color.r * intensity, 0.8f);
    color.g = std::pow(color.g * intensity, 0.8f);
    color.b = std::pow(color.b * intensity, 0.8f);

    return color;
}

// HSV to RGB conversion
inline Color hsvToRGB(float h, float s, float v) {
    float c = v * s;
    float x = c * (1 - std::abs(std::fmod(h / 60.0f, 2.0f) - 1));
    float m = v - c;

    float r, g, b;
    if (h < 60) {
        r = c; g = x; b = 0;
    } else if (h < 120) {
        r = x; g = c; b = 0;
    } else if (h < 180) {
        r = 0; g = c; b = x;
    } else if (h < 240) {
        r = 0; g = x; b = c;
    } else if (h < 300) {
        r = x; g = 0; b = c;
    } else {
        r = c; g = 0; b = x;
    }

    return {r + m, g + m, b + m};
}

// Color scheme generator
class ColorSchemeGenerator {
public:
    ColorSchemeGenerator(ColorParams const& params)
        : scheme_(params.scheme),
          start_(params.start),
          end_(params.end) {}

    // Update parameters (for live preview)
    void setParams(ColorParams const& params) {
        scheme_ = params.scheme;
        start_ = params.start;
        end_ = params.end;
    }

    // Get color for pendulum at index i out of total n pendulums
    // t is in range [0, 1], mapped to [start_, end_]
    Color getColor(float t) const {
        // Map t from [0,1] to [start_, end_]
        float mapped_t = start_ + t * (end_ - start_);

        switch (scheme_) {
            case ColorScheme::Spectrum:
                return getSpectrumColor(mapped_t);
            case ColorScheme::Rainbow:
                return getRainbowColor(mapped_t);
            case ColorScheme::Heat:
                return getHeatColor(mapped_t);
            case ColorScheme::Cool:
                return getCoolColor(mapped_t);
            case ColorScheme::Monochrome:
                return getMonochromeColor(mapped_t);
            default:
                return getSpectrumColor(mapped_t);
        }
    }

    Color getColorForIndex(int index, int total) const {
        float t = (total > 1) ? static_cast<float>(index) / (total - 1) : 0.0f;
        return getColor(t);
    }

private:
    ColorScheme scheme_;
    double start_;
    double end_;

    Color getSpectrumColor(float t) const {
        // Map t [0,1] to visible wavelength range [380, 780] nm
        float wavelength = 380.0f + t * 400.0f;
        return wavelengthToRGB(wavelength);
    }

    Color getRainbowColor(float t) const {
        // Map t [0,1] to full hue cycle (0-360)
        float h = t * 360.0f;
        return hsvToRGB(h, 1.0f, 1.0f);
    }

    Color getHeatColor(float t) const {
        // Black -> Red -> Yellow -> White
        if (t < 0.33f) {
            float s = t / 0.33f;
            return {s, 0.0f, 0.0f};
        } else if (t < 0.67f) {
            float s = (t - 0.33f) / 0.34f;
            return {1.0f, s, 0.0f};
        } else {
            float s = (t - 0.67f) / 0.33f;
            return {1.0f, 1.0f, s};
        }
    }

    Color getCoolColor(float t) const {
        // Blue -> Cyan -> White
        if (t < 0.5f) {
            float s = t / 0.5f;
            return {0.0f, s, 1.0f};
        } else {
            float s = (t - 0.5f) / 0.5f;
            return {s, 1.0f, 1.0f};
        }
    }

    Color getMonochromeColor(float t) const {
        // Just varying intensity of white
        float v = 0.3f + 0.7f * t;
        return {v, v, v};
    }
};
