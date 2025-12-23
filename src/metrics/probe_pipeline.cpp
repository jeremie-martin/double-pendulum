#include "metrics/probe_pipeline.h"

namespace metrics {

ProbePipeline::ProbePipeline()
    : boom_analyzer_(std::make_unique<BoomAnalyzer>()),
      causticness_analyzer_(std::make_unique<CausticnessAnalyzer>()) {

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

void ProbePipeline::setBoomThreshold(double threshold) {
    boom_threshold_ = threshold;
}

void ProbePipeline::setBoomConfirmation(int frames) {
    boom_confirmation_ = frames;
}

void ProbePipeline::setChaosThreshold(double threshold) {
    chaos_threshold_ = threshold;
}

void ProbePipeline::setChaosConfirmation(int frames) {
    chaos_confirmation_ = frames;
}

void ProbePipeline::enableBoomAnalyzer(bool enable) {
    boom_analyzer_enabled_ = enable;
}

void ProbePipeline::enableCausticnessAnalyzer(bool enable) {
    causticness_analyzer_enabled_ = enable;
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
    boom_analyzer_->reset();
    causticness_analyzer_->reset();
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
    boom_analyzer_->reset();
    causticness_analyzer_->reset();

    // Register GPU metrics for Phase 2
    collector_.registerGPUMetrics();

    current_phase_ = 2;
    current_frame_ = 0;
    setupEventDetector();
}

void ProbePipeline::setupEventDetector() {
    event_detector_.clearCriteria();
    event_detector_.addBoomCriteria(boom_threshold_, boom_confirmation_,
                                     MetricNames::Variance);
    event_detector_.addChaosCriteria(chaos_threshold_, chaos_confirmation_,
                                      MetricNames::Variance);
}

void ProbePipeline::feedPhysicsFrame(std::vector<double> const& angle1s,
                                      std::vector<double> const& angle2s,
                                      double total_energy) {
    collector_.beginFrame(current_frame_);
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
    if (boom_analyzer_enabled_) {
        boom_analyzer_->analyze(collector_, event_detector_);
    }
    if (causticness_analyzer_enabled_) {
        causticness_analyzer_->analyze(collector_, event_detector_);
    }
}

SimulationScore ProbePipeline::getScores() const {
    SimulationScore scores;

    if (boom_analyzer_enabled_ && boom_analyzer_->hasResults()) {
        scores.set(ScoreNames::Boom, boom_analyzer_->score());
    }
    if (causticness_analyzer_enabled_ && causticness_analyzer_->hasResults()) {
        scores.set(ScoreNames::Causticness, causticness_analyzer_->score());
    }

    return scores;
}

ProbePhaseResults ProbePipeline::finalizePhase() {
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
    results.scores = getScores();

    // Get boom quality
    if (boom_analyzer_enabled_ && boom_analyzer_->hasResults()) {
        results.boom_quality = boom_analyzer_->getQuality();
    }

    // Evaluate filter
    FilterResult filter_result =
        filter.evaluate(collector_, event_detector_, results.scores);
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
