#pragma once

#include "optimize/prediction_target.h"
#include "metrics/causticness_analyzer.h"
#include "metrics/metrics_collector.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace optimize {

// Result of score prediction
struct ScorePrediction {
    double score = 0.0;
    ScoreMethod method_used = ScoreMethod::PeakClarity;

    bool valid() const { return score >= 0.0 && score <= 1.0; }
};

// Generic score predictor that works with analyzers
class ScorePredictor {
public:
    ScorePredictor() = default;
    explicit ScorePredictor(ScoreParams const& params) : params_(params) {}

    void setParams(ScoreParams const& params) { params_ = params; }
    ScoreParams const& getParams() const { return params_; }

    // Main prediction entry point
    // Note: CausticnessAnalyzer must have already been run with analyze()
    ScorePrediction predict(metrics::CausticnessAnalyzer const& analyzer) const {
        ScorePrediction result;
        result.method_used = params_.method;

        if (!analyzer.hasResults()) {
            result.score = 0.0;
            return result;
        }

        switch (params_.method) {
        case ScoreMethod::PeakClarity:
            result.score = predictPeakClarity(analyzer);
            break;
        case ScoreMethod::PostBoomSustain:
            result.score = predictPostBoomSustain(analyzer);
            break;
        case ScoreMethod::Composite:
            result.score = predictComposite(analyzer);
            break;
        default:
            result.score = predictPeakClarity(analyzer);
            break;
        }

        // Clamp to valid range
        result.score = std::clamp(result.score, 0.0, 1.0);
        return result;
    }

private:
    ScoreParams params_;

    // Method 1: Peak clarity score
    // Higher = cleaner single peak, lower = competing peaks
    double predictPeakClarity(metrics::CausticnessAnalyzer const& analyzer) const {
        return analyzer.peakClarityScore();
    }

    // Method 2: Post-boom sustain
    // Higher = causticness remains high after boom
    double predictPostBoomSustain(metrics::CausticnessAnalyzer const& analyzer) const {
        return analyzer.postBoomAreaNormalized();
    }

    // Method 3: Composite weighted score
    // Uses weights from params, or defaults to analyzer's qualityScore()
    double predictComposite(metrics::CausticnessAnalyzer const& analyzer) const {
        if (params_.weights.empty()) {
            // Default: use analyzer's built-in quality score
            return analyzer.score();
        }

        // Custom weighted combination
        double total = 0.0;
        double weight_sum = 0.0;

        for (auto const& [name, weight] : params_.weights) {
            double value = 0.0;
            if (name == "peak_clarity") {
                value = analyzer.peakClarityScore();
            } else if (name == "post_boom_sustain") {
                value = analyzer.postBoomAreaNormalized();
            } else if (name == "peak_causticness") {
                value = std::min(1.0, analyzer.getMetrics().peak_causticness);
            }
            total += value * weight;
            weight_sum += weight;
        }

        return weight_sum > 0 ? total / weight_sum : 0.0;
    }
};

// Convenience function: predict score using params
inline ScorePrediction predictScore(metrics::CausticnessAnalyzer const& analyzer,
                                     ScoreParams const& params) {
    ScorePredictor predictor(params);
    return predictor.predict(analyzer);
}

// Convert ScorePrediction to PredictionResult
inline PredictionResult toPredictionResult(std::string const& target_name,
                                            ScorePrediction const& prediction) {
    PredictionResult result;
    result.target_name = target_name;
    result.type = PredictionType::Score;
    result.predicted_score = prediction.score;
    return result;
}

}  // namespace optimize
