#pragma once

// ============================================================================
// PREDICTOR REGISTRY
// ============================================================================
// Central registry for all prediction methods, similar to METRIC_REGISTRY.
// Provides a single source of truth for:
// - Available frame detection methods (outputs frame index)
// - Available score prediction methods (outputs 0-1 value)
//
// This registry is parallel to metric_registry.h and follows the same pattern:
// - constexpr array as single source of truth
// - Lookup helpers for finding predictors by name
// - Category filtering for UI generation
// ============================================================================

#include <array>
#include <string_view>
#include <vector>

namespace optimize {

// Type of prediction output
enum class PredictorType {
    Frame,  // Outputs frame index (int)
    Score   // Outputs normalized value (double 0-1)
};

// Category for grouping predictors in UI
enum class PredictorCategory {
    Detection,  // Frame detection methods (boom, chaos)
    Signal      // Signal analysis score methods (quality scoring)
};

// Definition of a predictor method
struct PredictorDef {
    const char* name;           // Canonical name (e.g., "max_value")
    const char* short_name;     // Short display name (e.g., "Max")
    const char* description;    // Human-readable description
    PredictorType type;         // Frame or Score output
    PredictorCategory category; // For UI grouping
    bool requires_reference;    // Needs boom/reference frame first?
};

// ============================================================================
// PREDICTOR REGISTRY
// ============================================================================
// All available prediction methods in one place.
// Add new predictors here - they'll automatically appear in config parsing and UI.

inline constexpr std::array<PredictorDef, 8> PREDICTOR_REGISTRY = {{
    // ========================================================================
    // Frame Detection Methods (from FrameDetector)
    // ========================================================================
    // These find a specific frame in the time series

    {"max_value", "Max",
     "Frame with maximum value",
     PredictorType::Frame, PredictorCategory::Detection, false},

    {"first_peak_percent", "FirstPeak",
     "First peak >= X% of max",
     PredictorType::Frame, PredictorCategory::Detection, false},

    {"derivative_peak", "DerivPeak",
     "Maximum d(metric)/dt",
     PredictorType::Frame, PredictorCategory::Detection, false},

    {"threshold_crossing", "Crossing",
     "First sustained crossing of threshold",
     PredictorType::Frame, PredictorCategory::Detection, false},

    {"second_derivative_peak", "AccelPeak",
     "Maximum acceleration (d²/dt²)",
     PredictorType::Frame, PredictorCategory::Detection, false},

    // ========================================================================
    // Score Prediction Methods (from SignalAnalyzer)
    // ========================================================================
    // These compute quality scores (0-1) from signal analysis

    {"peak_clarity", "Clarity",
     "Peak dominance over competitors",
     PredictorType::Score, PredictorCategory::Signal, true},

    {"post_boom_sustain", "Sustain",
     "Post-reference area normalized",
     PredictorType::Score, PredictorCategory::Signal, true},

    {"composite", "Composite",
     "Weighted combination of scores",
     PredictorType::Score, PredictorCategory::Signal, true},
}};

// Number of predictors in registry
inline constexpr size_t PREDICTOR_COUNT = PREDICTOR_REGISTRY.size();

// ============================================================================
// LOOKUP HELPERS
// ============================================================================

// Find predictor by canonical name
inline constexpr PredictorDef const* findPredictor(std::string_view name) {
    for (auto const& p : PREDICTOR_REGISTRY) {
        if (std::string_view(p.name) == name) {
            return &p;
        }
    }
    return nullptr;
}

// Find predictor by short name
inline constexpr PredictorDef const* findPredictorByShortName(std::string_view short_name) {
    for (auto const& p : PREDICTOR_REGISTRY) {
        if (std::string_view(p.short_name) == short_name) {
            return &p;
        }
    }
    return nullptr;
}

// Check if a predictor name exists
inline constexpr bool predictorExists(std::string_view name) {
    return findPredictor(name) != nullptr;
}

// ============================================================================
// CATEGORY FILTERING
// ============================================================================

// Get all frame detection predictors
inline std::vector<PredictorDef const*> getFramePredictors() {
    std::vector<PredictorDef const*> result;
    for (auto const& p : PREDICTOR_REGISTRY) {
        if (p.type == PredictorType::Frame) {
            result.push_back(&p);
        }
    }
    return result;
}

// Get all score prediction predictors
inline std::vector<PredictorDef const*> getScorePredictors() {
    std::vector<PredictorDef const*> result;
    for (auto const& p : PREDICTOR_REGISTRY) {
        if (p.type == PredictorType::Score) {
            result.push_back(&p);
        }
    }
    return result;
}

// Get all predictors in a category
inline std::vector<PredictorDef const*> getPredictorsByCategory(PredictorCategory category) {
    std::vector<PredictorDef const*> result;
    for (auto const& p : PREDICTOR_REGISTRY) {
        if (p.category == category) {
            result.push_back(&p);
        }
    }
    return result;
}

// Get all predictors that require a reference frame
inline std::vector<PredictorDef const*> getPredictorsRequiringReference() {
    std::vector<PredictorDef const*> result;
    for (auto const& p : PREDICTOR_REGISTRY) {
        if (p.requires_reference) {
            result.push_back(&p);
        }
    }
    return result;
}

// ============================================================================
// NAME GENERATION
// ============================================================================

// Get all predictor names (for config validation)
inline std::vector<std::string_view> getAllPredictorNames() {
    std::vector<std::string_view> result;
    result.reserve(PREDICTOR_COUNT);
    for (auto const& p : PREDICTOR_REGISTRY) {
        result.push_back(p.name);
    }
    return result;
}

// Get all frame predictor names
inline std::vector<std::string_view> getFramePredictorNames() {
    std::vector<std::string_view> result;
    for (auto const& p : PREDICTOR_REGISTRY) {
        if (p.type == PredictorType::Frame) {
            result.push_back(p.name);
        }
    }
    return result;
}

// Get all score predictor names
inline std::vector<std::string_view> getScorePredictorNames() {
    std::vector<std::string_view> result;
    for (auto const& p : PREDICTOR_REGISTRY) {
        if (p.type == PredictorType::Score) {
            result.push_back(p.name);
        }
    }
    return result;
}

}  // namespace optimize
