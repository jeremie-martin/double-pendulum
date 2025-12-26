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
    SecondDerivativePeak, // When d²(metric)/dt² is maximum
    ConstantFrame        // Always returns configured frame (for testing)
};

// Parameters for frame detection
struct FrameDetectionParams {
    FrameDetectionMethod method = FrameDetectionMethod::MaxValue;
    std::string metric_name;  // REQUIRED: Must be set from config, no default

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

    // For ConstantFrame: the frame to always return (for testing)
    int constant_frame = 100;
};

// ============================================================================
// SCORE PREDICTION
// ============================================================================

// Methods for computing quality scores
enum class ScoreMethod {
    // Boom-dependent methods (require reference frame)
    PeakClarity,     // Peak clarity from causticness analyzer
    PostBoomSustain, // Post-boom area normalized
    Composite,       // Weighted combination of scores

    // Boom-independent methods (analyze full signal)
    DynamicRange,    // (max - min) / max - measures "drama" of the signal
    RiseTime,        // peak_frame / total_frames - how quickly action happens
    Smoothness,      // 1 / (1 + mean_abs_second_deriv) - signal quality

    // Simple boom-relative methods (properties around boom)
    PreBoomContrast, // 1 - (avg_before / peak) - contrast before boom
    BoomSteepness,   // derivative_at_boom / max_derivative - sharpness of event

    // Additional signal analysis methods
    BuildupGradient, // Average slope from start to peak - measures dramatic rise
    PeakDominance,   // peak / mean ratio - how much peak stands out
    DecayRate,       // How quickly signal drops after peak

    // Testing
    ConstantScore    // Always returns configured score (for testing)
};

// Parameters for score prediction
struct ScoreParams {
    ScoreMethod method = ScoreMethod::PeakClarity;
    std::string metric_name;  // REQUIRED: Must be set from config, no default

    // For Composite: pairs of (score_name, weight)
    std::vector<std::pair<std::string, double>> weights;

    // For ConstantScore: the score to always return (for testing)
    double constant_score = 0.5;

    // For boom-relative methods: window size around boom (seconds)
    double window_seconds = 1.0;
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
    case FrameDetectionMethod::ConstantFrame:
        return "constant_frame";
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
    if (s == "constant_frame" || s == "constant")
        return FrameDetectionMethod::ConstantFrame;
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
    case ScoreMethod::DynamicRange:
        return "dynamic_range";
    case ScoreMethod::RiseTime:
        return "rise_time";
    case ScoreMethod::Smoothness:
        return "smoothness";
    case ScoreMethod::PreBoomContrast:
        return "pre_boom_contrast";
    case ScoreMethod::BoomSteepness:
        return "boom_steepness";
    case ScoreMethod::BuildupGradient:
        return "buildup_gradient";
    case ScoreMethod::PeakDominance:
        return "peak_dominance";
    case ScoreMethod::DecayRate:
        return "decay_rate";
    case ScoreMethod::ConstantScore:
        return "constant_score";
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
    if (s == "dynamic_range" || s == "range")
        return ScoreMethod::DynamicRange;
    if (s == "rise_time" || s == "rise")
        return ScoreMethod::RiseTime;
    if (s == "smoothness" || s == "smooth")
        return ScoreMethod::Smoothness;
    if (s == "pre_boom_contrast" || s == "contrast")
        return ScoreMethod::PreBoomContrast;
    if (s == "boom_steepness" || s == "steepness")
        return ScoreMethod::BoomSteepness;
    if (s == "buildup_gradient" || s == "buildup")
        return ScoreMethod::BuildupGradient;
    if (s == "peak_dominance" || s == "dominance")
        return ScoreMethod::PeakDominance;
    if (s == "decay_rate" || s == "decay")
        return ScoreMethod::DecayRate;
    if (s == "constant_score" || s == "constant")
        return ScoreMethod::ConstantScore;
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
