#pragma once

#include "color.h"
#include "config.h"

#include <algorithm>
#include <array>
#include <cmath>

// =============================================================================
// GRADIENT INFRASTRUCTURE
// Linear-light interpolation for better additive blending results
// =============================================================================

struct ColorStop {
    float t;
    Color c;
};

inline float clamp01(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}
inline float smoothstep01(float x) {
    x = clamp01(x);
    return x * x * (3.0f - 2.0f * x);
}

inline Color lerp(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// sRGB <-> Linear conversions for proper light interpolation
inline float srgbToLinear(float c) {
    return std::pow(clamp01(c), 2.2f);
}
inline float linearToSrgb(float c) {
    return std::pow(clamp01(c), 1.0f / 2.2f);
}

inline Color lerpLinearLight(Color a, Color b, float t) {
    Color al{srgbToLinear(a.r), srgbToLinear(a.g), srgbToLinear(a.b)};
    Color bl{srgbToLinear(b.r), srgbToLinear(b.g), srgbToLinear(b.b)};
    Color ml = lerp(al, bl, t);
    return {linearToSrgb(ml.r), linearToSrgb(ml.g), linearToSrgb(ml.b)};
}

template <size_t N>
inline Color sampleGradient(std::array<ColorStop, N> const& stops, float t, bool smooth = true) {
    t = clamp01(t);

    // Find segment
    size_t i = 0;
    while (i + 1 < N && t > stops[i + 1].t)
        i++;

    if (i + 1 >= N)
        return stops[N - 1].c;

    float t0 = stops[i].t, t1 = stops[i + 1].t;
    float u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
    if (smooth)
        u = smoothstep01(u);

    return lerpLinearLight(stops[i].c, stops[i + 1].c, u);
}

// Convenience for hex palettes
constexpr inline Color rgb255(int r, int g, int b) {
    return {r / 255.0f, g / 255.0f, b / 255.0f};
}

// =============================================================================
// CORE COLOR CONVERSIONS
// =============================================================================

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
        r = c;
        g = x;
        b = 0;
    } else if (h < 120) {
        r = x;
        g = c;
        b = 0;
    } else if (h < 180) {
        r = 0;
        g = c;
        b = x;
    } else if (h < 240) {
        r = 0;
        g = x;
        b = c;
    } else if (h < 300) {
        r = x;
        g = 0;
        b = c;
    } else {
        r = c;
        g = 0;
        b = x;
    }

    return {r + m, g + m, b + m};
}

// =============================================================================
// COLOR SCHEME GENERATOR
// =============================================================================

class ColorSchemeGenerator {
public:
    ColorSchemeGenerator(ColorParams const& params)
        : scheme_(params.scheme), start_(params.start), end_(params.end) {}

    void setParams(ColorParams const& params) {
        scheme_ = params.scheme;
        start_ = params.start;
        end_ = params.end;
    }

    Color getColor(float t) const {
        float mapped_t = start_ + t * (end_ - start_);

        switch (scheme_) {
        // === ORIGINAL SCHEMES ===
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
        case ColorScheme::Plasma:
            return getPlasmaColor(mapped_t);
        case ColorScheme::Viridis:
            return getViridisColor(mapped_t);
        case ColorScheme::Inferno:
            return getInfernoColor(mapped_t);
        case ColorScheme::Sunset:
            return getSunsetColor(mapped_t);

        // === NEW GRADIENT-BASED SCHEMES ===
        case ColorScheme::Ember:
            return getEmberColor(mapped_t);
        case ColorScheme::DeepOcean:
            return getDeepOceanColor(mapped_t);
        case ColorScheme::NeonViolet:
            return getNeonVioletColor(mapped_t);
        case ColorScheme::Aurora:
            return getAuroraColor(mapped_t);
        case ColorScheme::Pearl:
            return getPearlColor(mapped_t);
        case ColorScheme::TurboPop:
            return getTurboPopColor(mapped_t);
        case ColorScheme::Nebula:
            return getNebulaColor(mapped_t);
        case ColorScheme::Blackbody:
            return getBlackbodyColor(mapped_t);
        case ColorScheme::Magma:
            return getMagmaColor(mapped_t);
        case ColorScheme::Cyberpunk:
            return getCyberpunkColor(mapped_t);
        case ColorScheme::Biolume:
            return getBiolumeColor(mapped_t);
        case ColorScheme::Gold:
            return getGoldColor(mapped_t);
        case ColorScheme::RoseGold:
            return getRoseGoldColor(mapped_t);
        case ColorScheme::Twilight:
            return getTwilightColor(mapped_t);
        case ColorScheme::ForestFire:
            return getForestFireColor(mapped_t);
        case ColorScheme::AbyssalGlow:
            return getAbyssalGlowColor(mapped_t);
        case ColorScheme::MoltenCore:
            return getMoltenCoreColor(mapped_t);
        case ColorScheme::Iridescent:
            return getIridescentColor(mapped_t);
        case ColorScheme::StellarNursery:
            return getStellarNurseryColor(mapped_t);
        case ColorScheme::WhiskeyAmber:
            return getWhiskeyAmberColor(mapped_t);

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

    // =========================================================================
    // ORIGINAL SCHEMES (preserved)
    // =========================================================================

    Color getSpectrumColor(float t) const {
        float wavelength = 380.0f + t * 400.0f;
        return wavelengthToRGB(wavelength);
    }

    Color getRainbowColor(float t) const {
        float h = t * 360.0f;
        return hsvToRGB(h, 1.0f, 1.0f);
    }

    Color getHeatColor(float t) const {
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
        if (t < 0.5f) {
            float s = t / 0.5f;
            return {0.0f, s, 1.0f};
        } else {
            float s = (t - 0.5f) / 0.5f;
            return {s, 1.0f, 1.0f};
        }
    }

    Color getMonochromeColor(float t) const {
        float v = 0.3f + 0.7f * t;
        return {v, v, v};
    }

    Color getPlasmaColor(float t) const {
        float r = 0.050f + 0.850f * t + 0.100f * std::sin(t * 3.14159f);
        float g = 0.030f + 0.700f * t * t;
        float b = 0.530f + 0.470f * std::sin((1.0f - t) * 3.14159f * 0.5f);
        return {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)};
    }

    Color getViridisColor(float t) const {
        float r = 0.267f + 0.004f * t + 0.329f * t * t + 0.400f * t * t * t;
        float g = 0.004f + 0.873f * t - 0.377f * t * t;
        float b = 0.329f + 0.420f * t - 0.749f * t * t;
        return {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)};
    }

    Color getInfernoColor(float t) const {
        float r, g, b;
        if (t < 0.25f) {
            float s = t / 0.25f;
            r = 0.0f + 0.3f * s;
            g = 0.0f;
            b = 0.1f + 0.3f * s;
        } else if (t < 0.5f) {
            float s = (t - 0.25f) / 0.25f;
            r = 0.3f + 0.5f * s;
            g = 0.0f + 0.1f * s;
            b = 0.4f - 0.2f * s;
        } else if (t < 0.75f) {
            float s = (t - 0.5f) / 0.25f;
            r = 0.8f + 0.2f * s;
            g = 0.1f + 0.4f * s;
            b = 0.2f - 0.2f * s;
        } else {
            float s = (t - 0.75f) / 0.25f;
            r = 1.0f;
            g = 0.5f + 0.5f * s;
            b = 0.0f + 0.3f * s;
        }
        return {r, g, b};
    }

    Color getSunsetColor(float t) const {
        float r, g, b;
        if (t < 0.33f) {
            float s = t / 0.33f;
            r = 1.0f;
            g = 0.6f - 0.3f * s;
            b = 0.2f + 0.4f * s;
        } else if (t < 0.67f) {
            float s = (t - 0.33f) / 0.34f;
            r = 1.0f - 0.3f * s;
            g = 0.3f - 0.1f * s;
            b = 0.6f + 0.2f * s;
        } else {
            float s = (t - 0.67f) / 0.33f;
            r = 0.7f - 0.5f * s;
            g = 0.2f + 0.1f * s;
            b = 0.8f + 0.2f * s;
        }
        return {r, g, b};
    }

    // =========================================================================
    // NEW GRADIENT-BASED SCHEMES (using linear-light interpolation)
    // =========================================================================

    // Ember: Deep coal -> orange -> golden -> soft highlight
    // Great for warm caustics with concentrated bright cores
    Color getEmberColor(float t) const {
        static constexpr std::array<ColorStop, 5> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.20f, rgb255(43, 0, 0)},
                                                     {0.50f, rgb255(168, 61, 0)},
                                                     {0.80f, rgb255(246, 168, 11)},
                                                     {1.00f, rgb255(255, 242, 198)}}};
        return sampleGradient(s, t, true);
    }

    // DeepOcean: Inky black -> deep teal -> cyan -> ice
    // Perfect for "glass" or underwater caustics
    Color getDeepOceanColor(float t) const {
        static constexpr std::array<ColorStop, 6> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.22f, rgb255(0, 31, 41)},
                                                     {0.45f, rgb255(0, 51, 77)},
                                                     {0.70f, rgb255(8, 125, 174)},
                                                     {0.88f, rgb255(37, 198, 234)},
                                                     {1.00f, rgb255(232, 251, 255)}}};
        return sampleGradient(s, t, true);
    }

    // NeonViolet: Dark purple -> magenta -> pink glow
    // Moody, stays in purple/pink range
    Color getNeonVioletColor(float t) const {
        static constexpr std::array<ColorStop, 6> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.20f, rgb255(57, 17, 51)},
                                                     {0.45f, rgb255(77, 31, 69)},
                                                     {0.70f, rgb255(168, 83, 141)},
                                                     {0.86f, rgb255(216, 110, 178)},
                                                     {1.00f, rgb255(255, 214, 242)}}};
        return sampleGradient(s, t, true);
    }

    // Aurora: Night blue -> teal -> green -> warm spark
    // Northern lights with multiple hue shifts
    Color getAuroraColor(float t) const {
        static constexpr std::array<ColorStop, 6> s{{{0.00f, rgb255(0, 8, 20)},
                                                     {0.30f, rgb255(0, 51, 77)},
                                                     {0.55f, rgb255(10, 166, 166)},
                                                     {0.72f, rgb255(109, 176, 147)},
                                                     {0.88f, rgb255(233, 206, 83)},
                                                     {1.00f, rgb255(255, 247, 194)}}};
        return sampleGradient(s, t, true);
    }

    // Pearl: Espresso -> cream -> lilac sheen
    // Elegant, soft, high-contrast organic feel
    Color getPearlColor(float t) const {
        static constexpr std::array<ColorStop, 5> s{{{0.00f, rgb255(5, 2, 1)},
                                                     {0.35f, rgb255(58, 46, 35)},
                                                     {0.62f, rgb255(195, 164, 132)},
                                                     {0.85f, rgb255(243, 225, 209)},
                                                     {1.00f, rgb255(226, 206, 234)}}};
        return sampleGradient(s, t, true);
    }

    // TurboPop: High-energy rainbow with dark lows
    // Vibrant without washing out to white
    Color getTurboPopColor(float t) const {
        static constexpr std::array<ColorStop, 7> s{{{0.00f, rgb255(48, 18, 59)},
                                                     {0.18f, rgb255(28, 79, 215)},
                                                     {0.36f, rgb255(0, 181, 255)},
                                                     {0.52f, rgb255(0, 224, 138)},
                                                     {0.70f, rgb255(255, 230, 0)},
                                                     {0.86f, rgb255(255, 122, 0)},
                                                     {1.00f, rgb255(204, 15, 15)}}};
        return sampleGradient(s, t, true);
    }

    // Nebula: Deep space -> purple -> magenta -> cyan wisps
    // Multiple hue shifts create color mixing in overlaps
    Color getNebulaColor(float t) const {
        static constexpr std::array<ColorStop, 6> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.25f, rgb255(26, 13, 51)},
                                                     {0.45f, rgb255(77, 26, 77)},
                                                     {0.65f, rgb255(179, 51, 153)},
                                                     {0.82f, rgb255(128, 179, 204)},
                                                     {1.00f, rgb255(230, 242, 255)}}};
        return sampleGradient(s, t, true);
    }

    // Blackbody: Physically accurate heated object radiation
    // Dark -> red -> orange -> yellow -> white -> slight blue
    Color getBlackbodyColor(float t) const {
        static constexpr std::array<ColorStop, 6> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.25f, rgb255(128, 0, 0)},
                                                     {0.45f, rgb255(230, 77, 0)},
                                                     {0.65f, rgb255(255, 191, 0)},
                                                     {0.85f, rgb255(255, 255, 230)},
                                                     {1.00f, rgb255(230, 240, 255)}}};
        return sampleGradient(s, t, true);
    }

    // Magma: Matplotlib's Magma - darker/moodier than Inferno
    // Black -> deep purple -> red/orange -> pale yellow
    Color getMagmaColor(float t) const {
        static constexpr std::array<ColorStop, 6> s{{{0.00f, rgb255(0, 0, 4)},
                                                     {0.20f, rgb255(40, 11, 84)},
                                                     {0.40f, rgb255(120, 28, 109)},
                                                     {0.60f, rgb255(212, 72, 66)},
                                                     {0.80f, rgb255(253, 174, 97)},
                                                     {1.00f, rgb255(252, 253, 191)}}};
        return sampleGradient(s, t, true);
    }

    // Cyberpunk: Hot pink -> purple -> electric blue -> acid green
    // High-energy synthetic neon look
    Color getCyberpunkColor(float t) const {
        static constexpr std::array<ColorStop, 5> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.25f, rgb255(255, 0, 102)},
                                                     {0.50f, rgb255(102, 0, 204)},
                                                     {0.75f, rgb255(0, 153, 255)},
                                                     {1.00f, rgb255(0, 255, 136)}}};
        return sampleGradient(s, t, true);
    }

    // Bioluminescence: Deep sea organisms
    // Dark navy -> ghostly teal -> electric lime
    Color getBiolumeColor(float t) const {
        static constexpr std::array<ColorStop, 5> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.30f, rgb255(0, 20, 40)},
                                                     {0.55f, rgb255(0, 77, 77)},
                                                     {0.78f, rgb255(51, 179, 128)},
                                                     {1.00f, rgb255(179, 255, 204)}}};
        return sampleGradient(s, t, true);
    }

    // Gold: Ethereal gold - chocolate -> bronze -> gold -> white
    // Silk/polished brass under warm light
    Color getGoldColor(float t) const {
        static constexpr std::array<ColorStop, 5> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.30f, rgb255(51, 31, 10)},
                                                     {0.55f, rgb255(153, 102, 26)},
                                                     {0.78f, rgb255(230, 184, 77)},
                                                     {1.00f, rgb255(255, 247, 220)}}};
        return sampleGradient(s, t, true);
    }

    // RoseGold: Deep rose -> rose gold -> champagne -> cream
    // Elegant metallics with subtle warmth
    Color getRoseGoldColor(float t) const {
        static constexpr std::array<ColorStop, 5> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.30f, rgb255(77, 38, 51)},
                                                     {0.55f, rgb255(179, 128, 128)},
                                                     {0.78f, rgb255(230, 194, 179)},
                                                     {1.00f, rgb255(255, 240, 235)}}};
        return sampleGradient(s, t, true);
    }

    // Twilight: Sunset to night
    // Orange -> pink -> purple -> deep blue
    Color getTwilightColor(float t) const {
        static constexpr std::array<ColorStop, 6> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.20f, rgb255(255, 128, 64)},
                                                     {0.40f, rgb255(230, 102, 153)},
                                                     {0.60f, rgb255(153, 51, 153)},
                                                     {0.80f, rgb255(51, 51, 128)},
                                                     {1.00f, rgb255(179, 204, 230)}}};
        return sampleGradient(s, t, true);
    }

    // ForestFire: Dark greens igniting
    // Deep forest -> amber -> flame orange -> bright yellow
    Color getForestFireColor(float t) const {
        static constexpr std::array<ColorStop, 5> s{{{0.00f, rgb255(0, 0, 0)},
                                                     {0.25f, rgb255(26, 51, 13)},
                                                     {0.50f, rgb255(128, 77, 13)},
                                                     {0.75f, rgb255(230, 128, 26)},
                                                     {1.00f, rgb255(255, 240, 179)}}};
        return sampleGradient(s, t, true);
    }

    // =========================================================================
    // MY ORIGINAL SCHEMES (curve-based for unique character)
    // =========================================================================

    // AbyssalGlow: Bioluminescent cyan-green emerging from deep black
    // "Something alive in the deep" quality
    Color getAbyssalGlowColor(float t) const {
        float r = 0.02f + 0.25f * std::pow(t, 2.5f);
        float g = 0.04f + 0.96f * std::pow(t, 1.4f);
        float b = 0.08f + 0.72f * std::pow(t, 1.1f);
        // Slight warmth injection at peak
        r += 0.15f * std::pow(t, 4.0f);
        return {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)};
    }

    // MoltenCore: Volcanic with controlled incandescent peaks
    // Slower red-orange transition, proper incandescent white
    Color getMoltenCoreColor(float t) const {
        float r, g, b;
        if (t < 0.35f) {
            float s = t / 0.35f;
            r = std::pow(s, 0.7f) * 0.7f;
            g = 0.0f;
            b = std::pow(s, 1.5f) * 0.15f;
        } else if (t < 0.65f) {
            float s = (t - 0.35f) / 0.30f;
            r = 0.7f + 0.3f * s;
            g = std::pow(s, 1.3f) * 0.6f;
            b = 0.15f * (1.0f - s);
        } else {
            float s = (t - 0.65f) / 0.35f;
            r = 1.0f;
            g = 0.6f + 0.4f * std::pow(s, 0.8f);
            b = std::pow(s, 1.5f) * 0.9f;
        }
        return {r, g, b};
    }

    // Iridescent: Thin-film interference - shifting hues
    // Purple-pink-gold-green with overlapping color mixing
    Color getIridescentColor(float t) const {
        float phase = t * 2.5f;
        float r = 0.3f + 0.4f * std::sin(phase * 3.14159f) + 0.3f * t;
        float g = 0.2f + 0.3f * std::sin(phase * 3.14159f + 2.1f) + 0.5f * t;
        float b = 0.4f + 0.4f * std::sin(phase * 3.14159f + 4.2f) + 0.2f * t;
        float intensity = std::pow(t, 0.6f);
        return {std::clamp(r * intensity, 0.0f, 1.0f), std::clamp(g * intensity, 0.0f, 1.0f),
                std::clamp(b * intensity, 0.0f, 1.0f)};
    }

    // StellarNursery: Cosmic emission nebula
    // Deep space -> purple haze -> emission pink -> teal -> golden cores
    Color getStellarNurseryColor(float t) const {
        float r, g, b;
        if (t < 0.3f) {
            float s = t / 0.3f;
            r = 0.15f * std::pow(s, 0.8f);
            g = 0.02f * s;
            b = 0.25f * std::pow(s, 0.6f);
        } else if (t < 0.55f) {
            float s = (t - 0.3f) / 0.25f;
            r = 0.15f + 0.55f * s;
            g = 0.02f + 0.18f * s;
            b = 0.25f + 0.15f * s;
        } else if (t < 0.8f) {
            float s = (t - 0.55f) / 0.25f;
            r = 0.7f - 0.25f * s;
            g = 0.2f + 0.5f * s;
            b = 0.4f + 0.35f * s;
        } else {
            float s = (t - 0.8f) / 0.2f;
            r = 0.45f + 0.55f * s;
            g = 0.7f + 0.3f * s;
            b = 0.75f - 0.1f * s;
        }
        return {r, g, b};
    }

    // WhiskeyAmber: Warm, luxurious, organic
    // Light through aged bourbon - mahogany -> amber -> honey -> cream
    Color getWhiskeyAmberColor(float t) const {
        float r = 0.08f + 0.92f * std::pow(t, 0.9f);
        float g = 0.03f + 0.72f * std::pow(t, 1.4f);
        float b = 0.01f + 0.45f * std::pow(t, 2.8f);
        return {r, g, b};
    }
};