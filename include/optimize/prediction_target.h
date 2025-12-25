#pragma once

#include <string>
#include <variant>
#include <vector>

namespace optimize {

// ============================================================================
// PREDICTION TYPES
// ============================================================================

// Two kinds of predictions
enum class PredictionType {
    Frame,  // Predict a frame number (boom_frame, chaos_frame)
    Score   // Predict a 0-1 quality score (boom_quality)
};

// ============================================================================
// FRAME DETECTION
// ============================================================================

// Detection methods for frame-based targets
// These are the same algorithms previously in BoomDetectionMethod
enum class FrameDetectionMethod {
    MaxValue,            // Frame with maximum metric value
    FirstPeakPercent,    // First peak >= X% of max
    DerivativePeak,      // When d(metric)/dt is maximum
    ThresholdCrossing,   // First sustained crossing of threshold
    SecondDerivativePeak // When d²(metric)/dt² is maximum
};

// Parameters for frame detection
struct FrameDetectionParams {
    FrameDetectionMethod method = FrameDetectionMethod::MaxValue;
    std::string metric_name = "angular_causticness";

    // Offset applied after detection for visual alignment
    double offset_seconds = 0.0;

    // For FirstPeakPercent: threshold as fraction of max peak
    double peak_percent_threshold = 0.6;

    // For peak detection: minimum prominence to count as peak
    double min_peak_prominence = 0.05;

    // For DerivativePeak/SecondDerivativePeak: smoothing window
    int smoothing_window = 5;

    // For ThresholdCrossing: threshold as fraction of max
    double crossing_threshold = 0.3;

    // For ThresholdCrossing: consecutive frames above threshold
    int crossing_confirmation = 3;
};

// ============================================================================
// SCORE PREDICTION
// ============================================================================

// Methods for computing quality scores
enum class ScoreMethod {
    PeakClarity,     // Peak clarity from causticness analyzer
    PostBoomSustain, // Post-boom area normalized
    Composite        // Weighted combination of scores
};

// Parameters for score prediction
struct ScoreParams {
    ScoreMethod method = ScoreMethod::PeakClarity;
    std::string metric_name = "angular_causticness";

    // For Composite: pairs of (score_name, weight)
    std::vector<std::pair<std::string, double>> weights;
};

// ============================================================================
// PREDICTION RESULTS
// ============================================================================

// Result of a prediction
struct PredictionResult {
    std::string target_name;
    PredictionType type = PredictionType::Frame;

    // For Frame predictions
    int predicted_frame = -1;
    double predicted_seconds = 0.0;

    // For Score predictions (also stores metric value for Frame predictions)
    double predicted_score = 0.0;

    // Confidence in the prediction (optional, for future use)
    double confidence = 1.0;

    bool valid() const {
        if (type == PredictionType::Frame) {
            return predicted_frame >= 0;
        }
        return true;  // Scores are always valid
    }

    bool isFrame() const { return type == PredictionType::Frame; }
    bool isScore() const { return type == PredictionType::Score; }
};

// ============================================================================
// PREDICTION TARGET
// ============================================================================

// Complete target definition
struct PredictionTarget {
    std::string name;  // e.g., "boom", "chaos", "boom_quality"
    PredictionType type = PredictionType::Frame;

    // Parameters depend on type
    std::variant<FrameDetectionParams, ScoreParams> params;

    // Convenience accessors
    FrameDetectionParams const& frameParams() const {
        return std::get<FrameDetectionParams>(params);
    }

    FrameDetectionParams& frameParams() {
        return std::get<FrameDetectionParams>(params);
    }

    ScoreParams const& scoreParams() const {
        return std::get<ScoreParams>(params);
    }

    ScoreParams& scoreParams() {
        return std::get<ScoreParams>(params);
    }

    bool isFrame() const { return type == PredictionType::Frame; }
    bool isScore() const { return type == PredictionType::Score; }

    // Get the metric name regardless of type
    std::string metricName() const {
        if (type == PredictionType::Frame) {
            return std::get<FrameDetectionParams>(params).metric_name;
        } else {
            return std::get<ScoreParams>(params).metric_name;
        }
    }
};

// ============================================================================
// STRING CONVERSIONS
// ============================================================================

inline std::string toString(FrameDetectionMethod method) {
    switch (method) {
    case FrameDetectionMethod::MaxValue:
        return "max_value";
    case FrameDetectionMethod::FirstPeakPercent:
        return "first_peak_percent";
    case FrameDetectionMethod::DerivativePeak:
        return "derivative_peak";
    case FrameDetectionMethod::ThresholdCrossing:
        return "threshold_crossing";
    case FrameDetectionMethod::SecondDerivativePeak:
        return "second_derivative_peak";
    default:
        return "max_value";
    }
}

inline FrameDetectionMethod parseFrameDetectionMethod(std::string const& s) {
    if (s == "max_value" || s == "max_causticness" || s == "max")
        return FrameDetectionMethod::MaxValue;
    if (s == "first_peak_percent" || s == "first_peak")
        return FrameDetectionMethod::FirstPeakPercent;
    if (s == "derivative_peak" || s == "deriv")
        return FrameDetectionMethod::DerivativePeak;
    if (s == "threshold_crossing" || s == "crossing")
        return FrameDetectionMethod::ThresholdCrossing;
    if (s == "second_derivative_peak" || s == "accel")
        return FrameDetectionMethod::SecondDerivativePeak;
    return FrameDetectionMethod::MaxValue;
}

inline std::string toString(ScoreMethod method) {
    switch (method) {
    case ScoreMethod::PeakClarity:
        return "peak_clarity";
    case ScoreMethod::PostBoomSustain:
        return "post_boom_sustain";
    case ScoreMethod::Composite:
        return "composite";
    default:
        return "peak_clarity";
    }
}

inline ScoreMethod parseScoreMethod(std::string const& s) {
    if (s == "peak_clarity" || s == "clarity")
        return ScoreMethod::PeakClarity;
    if (s == "post_boom_sustain" || s == "sustain")
        return ScoreMethod::PostBoomSustain;
    if (s == "composite" || s == "weighted")
        return ScoreMethod::Composite;
    return ScoreMethod::PeakClarity;
}

inline std::string toString(PredictionType type) {
    return type == PredictionType::Frame ? "frame" : "score";
}

inline PredictionType parsePredictionType(std::string const& s) {
    if (s == "score" || s == "quality")
        return PredictionType::Score;
    return PredictionType::Frame;
}

}  // namespace optimize
