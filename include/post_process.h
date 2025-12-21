#pragma once

#include "renderer.h"
#include "config.h"
#include <algorithm>
#include <cmath>
#include <numeric>

class PostProcessor {
public:
    PostProcessor(PostProcessParams const& params)
        : gamma_(params.gamma),
          target_brightness_(params.target_brightness),
          contrast_(params.contrast),
          auto_normalize_(params.auto_normalize) {}

    // Apply all post-processing to the image
    // This normalizes brightness CONSISTENTLY regardless of pendulum count
    void apply(Image& image) const {
        if (image.data.empty()) return;

        // Step 1: Find min/max values
        auto [min_it, max_it] = std::minmax_element(image.data.begin(), image.data.end());
        float min_val = *min_it;
        float max_val = *max_it;
        float range = max_val - min_val;

        // Avoid division by zero
        if (range < 1e-6f) {
            range = 1.0f;
            min_val = 0.0f;
        }

        // Step 2: Calculate average brightness after normalization (for auto-brightness)
        double sum = 0.0;
        for (float v : image.data) {
            sum += (v - min_val) / range;
        }
        float avg_brightness = static_cast<float>(sum / image.data.size());

        // Step 3: Calculate brightness multiplier to hit target
        // This is the KEY fix: scale to target brightness regardless of pendulum count
        float brightness_mult = 1.0f;
        if (auto_normalize_ && avg_brightness > 1e-6f) {
            brightness_mult = target_brightness_ / avg_brightness;
            // Clamp to reasonable bounds
            brightness_mult = std::clamp(brightness_mult, 0.1f, 10.0f);
        }

        // Step 4: Apply all transformations
        for (float& v : image.data) {
            // Normalize to [0, 1]
            v = (v - min_val) / range;

            // Apply gamma correction
            v = std::pow(v, 1.0f / static_cast<float>(gamma_));

            // Apply brightness and contrast
            v = v * brightness_mult * static_cast<float>(contrast_);

            // Scale to [0, 255]
            v = v * 255.0f;
        }
    }

    // Apply with custom parameters (for preview/GUI adjustment)
    void apply(Image& image, double gamma, double brightness, double contrast) const {
        if (image.data.empty()) return;

        auto [min_it, max_it] = std::minmax_element(image.data.begin(), image.data.end());
        float min_val = *min_it;
        float max_val = *max_it;
        float range = max_val - min_val;

        if (range < 1e-6f) {
            range = 1.0f;
            min_val = 0.0f;
        }

        for (float& v : image.data) {
            v = (v - min_val) / range;
            v = std::pow(v, 1.0f / static_cast<float>(gamma));
            v = v * static_cast<float>(brightness * contrast) * 255.0f;
        }
    }

    // Getters for current parameters
    double gamma() const { return gamma_; }
    double target_brightness() const { return target_brightness_; }
    double contrast() const { return contrast_; }

private:
    double gamma_;
    double target_brightness_;
    double contrast_;
    bool auto_normalize_;
};
