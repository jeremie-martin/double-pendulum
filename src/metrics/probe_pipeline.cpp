#include "metrics/probe_pipeline.h"
#include "optimize/frame_detector.h"
#include "optimize/score_predictor.h"
#include "optimize/target_evaluator.h"
#include "pendulum.h"  // For PendulumState

namespace metrics {

ProbePipeline::ProbePipeline()
    : signal_analyzer_(std::make_unique<SignalAnalyzer>()) {

    // Default phase 1 config (physics-only)
    phase1_config_.enabled = true;
    phase1_config_.pendulum_count = 1000;
    phase1_config_.has_rendering = false;

    // Default phase 2 config (low-res render, disabled by default)
    phase2_config_.enabled = false;
    phase2_config_.pendulum_count = 5000;
    phase2_config_.has_rendering = true;
    phase2_config_.render_width = 270;
    phase2_config_.render_height = 270;

    // Register standard metrics
    collector_.registerStandardMetrics();
}

ProbePipeline::~ProbePipeline() = default;

void ProbePipeline::setPhase1Config(ProbePhaseConfig const& config) {
    phase1_config_ = config;
}

void ProbePipeline::setPhase2Config(ProbePhaseConfig const& config) {
    phase2_config_ = config;
}

void ProbePipeline::setPhase1Filter(ProbeFilter const& filter) {
    phase1_filter_ = filter;
}

void ProbePipeline::setPhase2Filter(ProbeFilter const& filter) {
    phase2_filter_ = filter;
}

void ProbePipeline::setChaosThreshold(double threshold) {
    chaos_threshold_ = threshold;
}

void ProbePipeline::setChaosConfirmation(int frames) {
    chaos_confirmation_ = frames;
}

void ProbePipeline::setBoomParams(optimize::FrameDetectionParams const& params) {
    boom_params_ = params;
}

void ProbePipeline::enableSignalAnalyzer(bool enable) {
    signal_analyzer_enabled_ = enable;
}

void ProbePipeline::setProgressCallback(ProgressCallback callback) {
    progress_callback_ = std::move(callback);
}

void ProbePipeline::setTerminationCheck(TerminationCheck callback) {
    termination_check_ = std::move(callback);
}

void ProbePipeline::reset() {
    collector_.reset();
    event_detector_.reset();
    signal_analyzer_->reset();
    current_phase_ = 0;
    current_frame_ = 0;
}

void ProbePipeline::beginPhase1() {
    reset();
    current_phase_ = 1;
    setupEventDetector();
}

void ProbePipeline::beginPhase2() {
    // Don't reset - we want to compare with Phase 1 or reuse settings
    // But do reset the collector for fresh metrics
    collector_.reset();
    event_detector_.reset();
    signal_analyzer_->reset();

    // Register GPU metrics for Phase 2
    collector_.registerGPUMetrics();

    current_phase_ = 2;
    current_frame_ = 0;
    setupEventDetector();
}

void ProbePipeline::setupEventDetector() {
    event_detector_.clearCriteria();
    // Note: Boom is detected via max causticness, not threshold crossing
    // Only chaos uses the EventDetector threshold mechanism
    event_detector_.addChaosCriteria(chaos_threshold_, chaos_confirmation_,
                                      MetricNames::Variance);
}

void ProbePipeline::feedPhysicsFrame(std::vector<PendulumState> const& states,
                                      double total_energy) {
    collector_.beginFrame(current_frame_);
    // Use updateFromStates for full metrics including spatial_concentration
    collector_.updateFromStates(states);

    if (total_energy > 0.0) {
        collector_.setMetric(MetricNames::TotalEnergy, total_energy);
    }

    // Update event detection
    event_detector_.update(collector_, frame_duration_);

    collector_.endFrame();
    current_frame_++;

    // Progress callback
    if (progress_callback_) {
        // Note: total frames not known here, caller should track
        progress_callback_(current_frame_, 0);
    }
}

void ProbePipeline::feedPhysicsFrame(std::vector<double> const& angle1s,
                                      std::vector<double> const& angle2s,
                                      double total_energy) {
    collector_.beginFrame(current_frame_);
    // Legacy angle-only version - does not compute position-based metrics
    collector_.updateFromAngles(angle1s, angle2s);

    if (total_energy > 0.0) {
        collector_.setMetric(MetricNames::TotalEnergy, total_energy);
    }

    // Update event detection
    event_detector_.update(collector_, frame_duration_);

    collector_.endFrame();
    current_frame_++;

    // Progress callback
    if (progress_callback_) {
        // Note: total frames not known here, caller should track
        progress_callback_(current_frame_, 0);
    }
}

void ProbePipeline::feedGPUFrame(GPUMetricsBundle const& gpu_metrics) {
    // GPU metrics are set for the current frame
    // This should be called after feedPhysicsFrame
    collector_.setGPUMetrics(gpu_metrics);
}

void ProbePipeline::runAnalyzers() {
    if (signal_analyzer_enabled_ && !boom_params_.metric_name.empty()) {
        // Set frame duration if available
        if (frame_duration_ > 0.0) {
            signal_analyzer_->setFrameDuration(frame_duration_);
        }
        // Use boom_params_.metric_name for signal analysis
        signal_analyzer_->setMetricName(boom_params_.metric_name);
        signal_analyzer_->analyze(collector_, event_detector_);
    }
}

SimulationScore ProbePipeline::getScores() const {
    SimulationScore scores;

    if (signal_analyzer_enabled_ && signal_analyzer_->hasResults()) {
        scores.set(ScoreNames::Causticness, signal_analyzer_->score());
        // Add peak clarity and post-boom sustain scores for filtering
        scores.set(ScoreNames::PeakClarity, signal_analyzer_->peakClarityScore());
        scores.set(ScoreNames::PostBoomSustain,
                   signal_analyzer_->postBoomAreaNormalized());
    }

    return scores;
}

ProbePhaseResults ProbePipeline::finalizePhase() {
    // Detect boom using configured params (required before running analyzers)
    if (frame_duration_ > 0.0) {
        auto boom = findBoomFrame(collector_, frame_duration_, boom_params_);
        if (boom.frame >= 0) {
            // Get variance at boom for the event
            double variance_at_boom = 0.0;
            if (auto const* var_series = collector_.getMetric(MetricNames::Variance)) {
                if (boom.frame < static_cast<int>(var_series->size())) {
                    variance_at_boom = var_series->at(boom.frame);
                }
            }
            forceBoomEvent(event_detector_, boom, variance_at_boom);
        }
    }

    // Run analyzers
    runAnalyzers();

    // Build results
    ProbeFilter const& filter =
        (current_phase_ == 2) ? phase2_filter_ : phase1_filter_;

    return buildResults(filter);
}

ProbePhaseResults ProbePipeline::buildResults(ProbeFilter const& filter) {
    ProbePhaseResults results;
    results.completed = true;
    results.frames_completed = current_frame_;

    // Get events
    auto boom_event = event_detector_.getEvent(EventNames::Boom);
    if (boom_event && boom_event->detected()) {
        results.boom_frame = boom_event->frame;
        results.boom_seconds = boom_event->seconds;
    }

    auto chaos_event = event_detector_.getEvent(EventNames::Chaos);
    if (chaos_event && chaos_event->detected()) {
        results.chaos_frame = chaos_event->frame;
        results.chaos_seconds = chaos_event->seconds;
    }

    // Get final metrics
    auto* variance_series = collector_.getMetric(MetricNames::Variance);
    if (variance_series && !variance_series->empty()) {
        results.final_variance = variance_series->current();
    }

    results.final_uniformity = collector_.getUniformity();

    // Get scores
    results.score = getScores();

    // Populate predictions BEFORE filter evaluation (filter needs predictions)
    if (!prediction_targets_.empty()) {
        // Use explicit targets with new generic API
        optimize::TargetEvaluator evaluator;
        for (auto const& target : prediction_targets_) {
            evaluator.addTarget(target);
        }
        results.predictions = evaluator.evaluate(collector_, frame_duration_);
    } else {
        // Default predictions based on events and scores
        // Boom prediction from event
        if (results.boom_frame.has_value()) {
            optimize::PredictionResult boom_pred;
            boom_pred.target_name = "boom";
            boom_pred.type = optimize::PredictionType::Frame;
            boom_pred.predicted_frame = *results.boom_frame;
            boom_pred.predicted_seconds = results.boom_seconds;
            // Get causticness at boom frame if available
            if (auto* caustic = collector_.getMetric(MetricNames::AngularCausticness)) {
                if (*results.boom_frame < static_cast<int>(caustic->size())) {
                    boom_pred.predicted_score = caustic->at(*results.boom_frame);
                }
            }
            results.predictions.push_back(boom_pred);
        }

        // Chaos prediction from event
        if (results.chaos_frame.has_value()) {
            optimize::PredictionResult chaos_pred;
            chaos_pred.target_name = "chaos";
            chaos_pred.type = optimize::PredictionType::Frame;
            chaos_pred.predicted_frame = *results.chaos_frame;
            chaos_pred.predicted_seconds = results.chaos_seconds;
            if (variance_series && *results.chaos_frame < static_cast<int>(variance_series->size())) {
                chaos_pred.predicted_score = variance_series->at(*results.chaos_frame);
            }
            results.predictions.push_back(chaos_pred);
        }

        // Boom quality prediction from causticness score
        if (results.score.has(ScoreNames::Causticness)) {
            optimize::PredictionResult quality_pred;
            quality_pred.target_name = "boom_quality";
            quality_pred.type = optimize::PredictionType::Score;
            quality_pred.predicted_score = results.score.get(ScoreNames::Causticness);
            results.predictions.push_back(quality_pred);
        }
    }

    // Evaluate filter with predictions
    FilterResult filter_result =
        filter.evaluate(collector_, event_detector_, results.score, results.predictions);
    results.passed_filter = filter_result.passed;
    results.rejection_reason = filter_result.reason;

    return results;
}

ProbePhaseResults ProbePipeline::run() {
    // This is a placeholder - actual simulation would be done externally
    // The pipeline is designed to be fed frame-by-frame from external simulation

    // For now, just return empty results
    ProbePhaseResults results;
    results.completed = false;
    results.rejection_reason = "run() not implemented - use feedPhysicsFrame()";
    return results;
}

} // namespace metrics
