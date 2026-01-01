#pragma once

#include "config.h"
#include "enum_utils.h"
#include "optimize/prediction_target.h"

// Centralized enum-to-string conversions using magic_enum
// These provide snake_case output for display/logging

constexpr const char* toString(PhysicsQuality q) {
    // Using static storage for constexpr compatibility
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

// ColorScheme toString - uses magic_enum for automatic maintenance
inline std::string toString(ColorScheme cs) {
    return enum_utils::toString(cs);
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
