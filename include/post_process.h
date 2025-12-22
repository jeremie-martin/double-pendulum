#pragma once

#include "renderer.h"
#include "config.h"
#include <algorithm>
#include <cmath>

// Standard post-processing pipeline:
// 1. Normalize to [0,1] using max value
// 2. Apply exposure (multiplicative, in stops: value * 2^exposure)
// 3. Apply contrast (centered at 0.5: (v - 0.5) * contrast + 0.5)
// 4. Clamp to [0,1]
// 5. Apply gamma correction (v^(1/gamma), typically gamma=2.2 for sRGB)
// 6. Scale to [0,255]

namespace PostProcess {

// Apply standard post-processing to a float buffer
// Input: HDR float values (accumulated intensities)
// Output: Values in [0,255] range ready for 8-bit conversion
//
// Parameters:
//   exposure: Brightness adjustment in stops (0 = no change, 1 = 2x brighter, -1 = 2x darker)
//   contrast: Contrast multiplier centered at 0.5 (1.0 = no change, >1 = more contrast)
//   gamma: Display gamma (2.2 for sRGB, 1.0 for linear)
inline void apply(float* data, size_t size, float exposure, float contrast, float gamma) {
    if (size == 0) return;

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

    // Step 2-6: Apply all transformations
    for (size_t i = 0; i < size; ++i) {
        float v = data[i];

        // Normalize to [0,1]
        v = v / max_val;

        // Apply exposure
        v = v * exposure_mult;

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
inline void applyToImage(Image& image, float exposure, float contrast, float gamma) {
    apply(image.raw_data(), image.data.size(), exposure, contrast, gamma);
}

// Apply to RGBA float buffer (4 channels, skip alpha)
inline void applyToRGBA(float* data, size_t pixel_count, float exposure, float contrast, float gamma) {
    if (pixel_count == 0) return;

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

            // Exposure
            v = v * exposure_mult;

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
        : exposure_(params.exposure),
          contrast_(params.contrast),
          gamma_(params.gamma) {}

    // Apply all post-processing to the image
    void apply(Image& image) const {
        PostProcess::applyToImage(image,
            static_cast<float>(exposure_),
            static_cast<float>(contrast_),
            static_cast<float>(gamma_));
    }

    // Apply with custom parameters (for preview/GUI adjustment)
    void apply(Image& image, double exposure, double contrast, double gamma) const {
        PostProcess::applyToImage(image,
            static_cast<float>(exposure),
            static_cast<float>(contrast),
            static_cast<float>(gamma));
    }

    double exposure() const { return exposure_; }
    double contrast() const { return contrast_; }
    double gamma() const { return gamma_; }

private:
    double exposure_;
    double contrast_;
    double gamma_;
};
