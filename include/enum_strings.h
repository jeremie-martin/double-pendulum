#pragma once

#include "config.h"
#include "optimize/prediction_target.h"

// Centralized enum-to-string conversions
// These replace duplicate lambdas scattered across main.cpp and simulation.cpp

constexpr const char* toString(PhysicsQuality q) {
    switch (q) {
    case PhysicsQuality::Low:
        return "low";
    case PhysicsQuality::Medium:
        return "medium";
    case PhysicsQuality::High:
        return "high";
    case PhysicsQuality::Ultra:
        return "ultra";
    case PhysicsQuality::Custom:
        return "custom";
    }
    return "unknown";
}

constexpr const char* toString(ToneMapOperator tm) {
    switch (tm) {
    case ToneMapOperator::None:
        return "none";
    case ToneMapOperator::Reinhard:
        return "reinhard";
    case ToneMapOperator::ReinhardExtended:
        return "reinhard_extended";
    case ToneMapOperator::ACES:
        return "aces";
    case ToneMapOperator::Logarithmic:
        return "logarithmic";
    }
    return "unknown";
}

constexpr const char* toString(ColorScheme cs) {
    switch (cs) {
    // Original
    case ColorScheme::Spectrum:
        return "spectrum";
    case ColorScheme::Rainbow:
        return "rainbow";
    case ColorScheme::Heat:
        return "heat";
    case ColorScheme::Cool:
        return "cool";
    case ColorScheme::Monochrome:
        return "monochrome";
    case ColorScheme::Plasma:
        return "plasma";
    case ColorScheme::Viridis:
        return "viridis";
    case ColorScheme::Inferno:
        return "inferno";
    case ColorScheme::Sunset:
        return "sunset";

    // New gradient-based
    case ColorScheme::Ember:
        return "ember";
    case ColorScheme::DeepOcean:
        return "deep_ocean";
    case ColorScheme::NeonViolet:
        return "neon_violet";
    case ColorScheme::Aurora:
        return "aurora";
    case ColorScheme::Pearl:
        return "pearl";
    case ColorScheme::TurboPop:
        return "turbo_pop";
    case ColorScheme::Nebula:
        return "nebula";
    case ColorScheme::Blackbody:
        return "blackbody";
    case ColorScheme::Magma:
        return "magma";
    case ColorScheme::Cyberpunk:
        return "cyberpunk";
    case ColorScheme::Biolume:
        return "biolume";
    case ColorScheme::Gold:
        return "gold";
    case ColorScheme::RoseGold:
        return "rose_gold";
    case ColorScheme::Twilight:
        return "twilight";
    case ColorScheme::ForestFire:
        return "forest_fire";

    // Curve-based
    case ColorScheme::AbyssalGlow:
        return "abyssal_glow";
    case ColorScheme::MoltenCore:
        return "molten_core";
    case ColorScheme::Iridescent:
        return "iridescent";
    case ColorScheme::StellarNursery:
        return "stellar_nursery";
    case ColorScheme::WhiskeyAmber:
        return "whiskey_amber";
    }
    return "unknown";
}

constexpr const char* toString(NormalizationMode nm) {
    switch (nm) {
    case NormalizationMode::PerFrame:
        return "per_frame";
    case NormalizationMode::ByCount:
        return "by_count";
    }
    return "unknown";
}

constexpr const char* toString(OutputFormat fmt) {
    switch (fmt) {
    case OutputFormat::PNG:
        return "png";
    case OutputFormat::Video:
        return "video";
    }
    return "unknown";
}

constexpr const char* toString(OutputMode mode) {
    switch (mode) {
    case OutputMode::Timestamped:
        return "timestamped";
    case OutputMode::Direct:
        return "direct";
    }
    return "unknown";
}

constexpr const char* toString(optimize::FrameDetectionMethod method) {
    switch (method) {
    case optimize::FrameDetectionMethod::MaxValue:
        return "max_value";
    case optimize::FrameDetectionMethod::FirstPeakPercent:
        return "first_peak_percent";
    case optimize::FrameDetectionMethod::DerivativePeak:
        return "derivative_peak";
    case optimize::FrameDetectionMethod::ThresholdCrossing:
        return "threshold_crossing";
    case optimize::FrameDetectionMethod::SecondDerivativePeak:
        return "second_derivative_peak";
    case optimize::FrameDetectionMethod::ConstantFrame:
        return "constant_frame";
    }
    return "unknown";
}

constexpr const char* toString(optimize::ScoreMethod method) {
    switch (method) {
    case optimize::ScoreMethod::PeakClarity:
        return "peak_clarity";
    case optimize::ScoreMethod::PostBoomSustain:
        return "post_boom_sustain";
    case optimize::ScoreMethod::Composite:
        return "composite";
    case optimize::ScoreMethod::DynamicRange:
        return "dynamic_range";
    case optimize::ScoreMethod::RiseTime:
        return "rise_time";
    case optimize::ScoreMethod::Smoothness:
        return "smoothness";
    case optimize::ScoreMethod::PreBoomContrast:
        return "pre_boom_contrast";
    case optimize::ScoreMethod::BoomSteepness:
        return "boom_steepness";
    case optimize::ScoreMethod::BuildupGradient:
        return "buildup_gradient";
    case optimize::ScoreMethod::PeakDominance:
        return "peak_dominance";
    case optimize::ScoreMethod::DecayRate:
        return "decay_rate";
    case optimize::ScoreMethod::MedianDominance:
        return "median_dominance";
    case optimize::ScoreMethod::TailWeight:
        return "tail_weight";
    }
    return "unknown";
}
