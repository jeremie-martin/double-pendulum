#pragma once

#include <json.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace metrics {

// Forward declarations
class MetricsCollector;
class EventDetector;

// Composite score aggregating all analyzer outputs
struct SimulationScore {
    std::unordered_map<std::string, double> scores;  // analyzer_name → score

    // Get a specific score (returns 0 if not found)
    double get(std::string const& name) const {
        auto it = scores.find(name);
        return it != scores.end() ? it->second : 0.0;
    }

    // Set a score
    void set(std::string const& name, double value) { scores[name] = value; }

    // Compute weighted composite score
    double composite(std::unordered_map<std::string, double> const& weights =
                         {}) const {
        if (scores.empty())
            return 0.0;

        double total = 0.0;
        double weight_sum = 0.0;

        for (auto const& [name, score] : scores) {
            double weight = 1.0;
            auto it = weights.find(name);
            if (it != weights.end()) {
                weight = it->second;
            }
            total += score * weight;
            weight_sum += weight;
        }

        return weight_sum > 0.0 ? total / weight_sum : 0.0;
    }

    // Check if a score exists
    bool has(std::string const& name) const {
        return scores.find(name) != scores.end();
    }

    // Check if any scores exist
    bool empty() const { return scores.empty(); }

    // Get all score names
    std::vector<std::string> names() const {
        std::vector<std::string> result;
        result.reserve(scores.size());
        for (auto const& [name, _] : scores) {
            result.push_back(name);
        }
        return result;
    }
};

// Abstract base class for pluggable quality analyzers
class Analyzer {
public:
    virtual ~Analyzer() = default;

    // Analyzer identification
    virtual std::string name() const = 0;

    // Run analysis on collected metrics
    // The analyzer should store its results internally
    virtual void analyze(MetricsCollector const& collector,
                         EventDetector const& events) = 0;

    // Get the primary score (0.0 - 1.0 normalized preferred)
    virtual double score() const = 0;

    // Get detailed results as JSON
    virtual nlohmann::json toJSON() const = 0;

    // Reset analyzer state
    virtual void reset() = 0;

    // Check if analysis has been performed
    virtual bool hasResults() const = 0;
};

// Score names for standard analyzers
namespace ScoreNames {
constexpr const char* Boom = "boom";
constexpr const char* Causticness = "causticness";
constexpr const char* PeakClarity = "peak_clarity";     // Peak clarity from causticness
constexpr const char* PostBoomSustain = "post_boom_sustain";  // Post-boom area normalized
constexpr const char* Animation = "animation";  // Future
} // namespace ScoreNames

} // namespace metrics
