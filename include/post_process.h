#pragma once

#include "config.h"
#include "renderer.h"

#include <algorithm>
#include <cmath>

// Standard post-processing pipeline:
// 1. Normalize to [0,1] using max value
// 2. Apply exposure (multiplicative, in stops: value * 2^exposure)
// 3. Apply tone mapping (HDR -> SDR compression)
// 4. Apply contrast (centered at 0.5: (v - 0.5) * contrast + 0.5)
// 5. Clamp to [0,1]
// 6. Apply gamma correction (v^(1/gamma), typically gamma=2.2 for sRGB)
// 7. Scale to [0,255]

namespace PostProcess {

// Tone mapping function - converts HDR values to [0,1] with soft shoulder
inline float toneMap(float v, ToneMapOperator op, float white_point = 1.0f) {
    switch (op) {
        case ToneMapOperator::Reinhard:
            // Simple Reinhard: x / (1 + x)
            return v / (1.0f + v);
        case ToneMapOperator::ReinhardExtended: {
            // Extended Reinhard with white point control
            float w2 = white_point * white_point;
            return (v * (1.0f + v / w2)) / (1.0f + v);
        }
        case ToneMapOperator::ACES: {
            // Narkowicz ACES Filmic approximation
            float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
            return std::max(0.0f, std::min(1.0f, (v * (a * v + b)) / (v * (c * v + d) + e)));
        }
        case ToneMapOperator::Logarithmic:
            // Logarithmic compression - very aggressive for extreme dynamic range
            // log(1 + v) compresses large values much more than Reinhard
            return std::log(1.0f + v) / std::log(1.0f + white_point);
        case ToneMapOperator::None:
        default:
            // Linear clamp (original behavior)
            return std::max(0.0f, std::min(1.0f, v));
    }
}

// Apply standard post-processing to a float buffer
// Input: HDR float values (accumulated intensities)
// Output: Values in [0,255] range ready for 8-bit conversion
//
// Parameters:
//   exposure: Brightness adjustment in stops (0 = no change, 1 = 2x brighter, -1 = 2x darker)
//   contrast: Contrast multiplier centered at 0.5 (1.0 = no change, >1 = more contrast)
//   gamma: Display gamma (2.2 for sRGB, 1.0 for linear)
//   tone_map_op: Tone mapping operator (None, Reinhard, ReinhardExtended, ACES)
//   white_point: White point for ReinhardExtended (default 1.0)
inline void apply(float* data, size_t size, float exposure, float contrast, float gamma,
                  ToneMapOperator tone_map_op = ToneMapOperator::None, float white_point = 1.0f) {
    if (size == 0)
        return;

    // Step 1: Find max value for normalization
    float max_val = 0.0f;
    for (size_t i = 0; i < size; ++i) {
        max_val = std::max(max_val, data[i]);
    }

    // Avoid division by zero
    if (max_val < 1e-6f) {
        max_val = 1.0f;
    }

    // Precompute exposure multiplier: 2^exposure
    float exposure_mult = std::pow(2.0f, exposure);

    // Precompute inverse gamma
    float inv_gamma = 1.0f / gamma;

    // Apply all transformations
    for (size_t i = 0; i < size; ++i) {
        float v = data[i];

        // Normalize to [0,1]
        v = v / max_val;

        // Apply exposure (gain in HDR space)
        v = v * exposure_mult;

        // Apply tone mapping (HDR -> SDR)
        v = toneMap(v, tone_map_op, white_point);

        // Apply contrast (centered at 0.5)
        v = (v - 0.5f) * contrast + 0.5f;

        // Clamp to [0,1]
        v = std::max(0.0f, std::min(1.0f, v));

        // Apply gamma correction
        v = std::pow(v, inv_gamma);

        // Scale to [0,255]
        data[i] = v * 255.0f;
    }
}

// Apply to Image class
inline void applyToImage(Image& image, float exposure, float contrast, float gamma,
                         ToneMapOperator tone_map_op = ToneMapOperator::None,
                         float white_point = 1.0f) {
    apply(image.raw_data(), image.data.size(), exposure, contrast, gamma, tone_map_op, white_point);
}

// Apply to RGBA float buffer (4 channels, skip alpha)
inline void applyToRGBA(float* data, size_t pixel_count, float exposure, float contrast,
                        float gamma, ToneMapOperator tone_map_op = ToneMapOperator::None,
                        float white_point = 1.0f) {
    if (pixel_count == 0)
        return;

    // Step 1: Find max value across RGB channels
    float max_val = 0.0f;
    for (size_t i = 0; i < pixel_count; ++i) {
        size_t idx = i * 4;
        max_val = std::max(max_val, data[idx]);     // R
        max_val = std::max(max_val, data[idx + 1]); // G
        max_val = std::max(max_val, data[idx + 2]); // B
    }

    if (max_val < 1e-6f) {
        max_val = 1.0f;
    }

    float exposure_mult = std::pow(2.0f, exposure);
    float inv_gamma = 1.0f / gamma;

    // Apply to each pixel
    for (size_t i = 0; i < pixel_count; ++i) {
        size_t idx = i * 4;

        for (int c = 0; c < 3; ++c) {
            float v = data[idx + c];

            // Normalize
            v = v / max_val;

            // Exposure (gain in HDR space)
            v = v * exposure_mult;

            // Tone mapping (HDR -> SDR)
            v = toneMap(v, tone_map_op, white_point);

            // Contrast (centered)
            v = (v - 0.5f) * contrast + 0.5f;

            // Clamp
            v = std::max(0.0f, std::min(1.0f, v));

            // Gamma
            v = std::pow(v, inv_gamma);

            data[idx + c] = v;
        }
        // Alpha unchanged
    }
}

// Convert processed RGBA floats [0,1] to RGBA8 bytes
inline void rgbaFloatToBytes(float const* src, uint8_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        size_t idx = i * 4;
        dst[idx] = static_cast<uint8_t>(std::round(src[idx] * 255.0f));
        dst[idx + 1] = static_cast<uint8_t>(std::round(src[idx + 1] * 255.0f));
        dst[idx + 2] = static_cast<uint8_t>(std::round(src[idx + 2] * 255.0f));
        dst[idx + 3] = 255;
    }
}

} // namespace PostProcess

// Legacy wrapper class for compatibility with existing code
class PostProcessor {
public:
    PostProcessor(PostProcessParams const& params)
        : tone_map_(params.tone_map), white_point_(params.reinhard_white_point),
          exposure_(params.exposure), contrast_(params.contrast), gamma_(params.gamma) {}

    // Apply all post-processing to the image
    void apply(Image& image) const {
        PostProcess::applyToImage(image, static_cast<float>(exposure_),
                                  static_cast<float>(contrast_), static_cast<float>(gamma_),
                                  tone_map_, static_cast<float>(white_point_));
    }

    // Apply with custom parameters (for preview/GUI adjustment)
    void apply(Image& image, double exposure, double contrast, double gamma,
               ToneMapOperator tone_map, double white_point) const {
        PostProcess::applyToImage(image, static_cast<float>(exposure), static_cast<float>(contrast),
                                  static_cast<float>(gamma), tone_map,
                                  static_cast<float>(white_point));
    }

    ToneMapOperator tone_map() const { return tone_map_; }
    double white_point() const { return white_point_; }
    double exposure() const { return exposure_; }
    double contrast() const { return contrast_; }
    double gamma() const { return gamma_; }

private:
    ToneMapOperator tone_map_;
    double white_point_;
    double exposure_;
    double contrast_;
    double gamma_;
};
