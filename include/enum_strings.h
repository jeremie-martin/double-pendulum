#pragma once

#include "config.h"

// Centralized enum-to-string conversions
// These replace duplicate lambdas scattered across main.cpp and simulation.cpp

constexpr const char* toString(PhysicsQuality q) {
    switch (q) {
    case PhysicsQuality::Low: return "low";
    case PhysicsQuality::Medium: return "medium";
    case PhysicsQuality::High: return "high";
    case PhysicsQuality::Ultra: return "ultra";
    case PhysicsQuality::Custom: return "custom";
    }
    return "unknown";
}

constexpr const char* toString(ToneMapOperator tm) {
    switch (tm) {
    case ToneMapOperator::None: return "none";
    case ToneMapOperator::Reinhard: return "reinhard";
    case ToneMapOperator::ReinhardExtended: return "reinhard_extended";
    case ToneMapOperator::ACES: return "aces";
    case ToneMapOperator::Logarithmic: return "logarithmic";
    }
    return "unknown";
}

constexpr const char* toString(ColorScheme cs) {
    switch (cs) {
    case ColorScheme::Spectrum: return "spectrum";
    case ColorScheme::Rainbow: return "rainbow";
    case ColorScheme::Heat: return "heat";
    case ColorScheme::Cool: return "cool";
    case ColorScheme::Monochrome: return "monochrome";
    }
    return "unknown";
}

constexpr const char* toString(NormalizationMode nm) {
    switch (nm) {
    case NormalizationMode::PerFrame: return "per_frame";
    case NormalizationMode::ByCount: return "by_count";
    }
    return "unknown";
}

constexpr const char* toString(OutputFormat fmt) {
    switch (fmt) {
    case OutputFormat::PNG: return "png";
    case OutputFormat::Video: return "video";
    }
    return "unknown";
}

constexpr const char* toString(OutputMode mode) {
    switch (mode) {
    case OutputMode::Timestamped: return "timestamped";
    case OutputMode::Direct: return "direct";
    }
    return "unknown";
}
