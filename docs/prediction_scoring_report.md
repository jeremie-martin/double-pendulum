# Prediction/Scoring Codebase Report (Concise)

## Current Pipeline
- Frame targets are generic: `[targets.X]` in config -> `TargetConfig` -> `PredictionTarget` -> `FrameDetector` over any metric series (`src/config.cpp`, `include/optimize/target_evaluator.h`, `include/optimize/frame_detector.h`).
- Boom detection is a thin wrapper around `FrameDetector`, then forces an event for analyzers (`include/metrics/boom_detection.h`, `include/metrics/metrics_init.h`).
- Score targets are not generic: `ScorePredictor` ignores `ScoreParams.metric_name` and only uses `CausticnessAnalyzer` (`include/optimize/score_predictor.h`, `include/metrics/causticness_analyzer.h`).
- `CausticnessAnalyzer` is hard-wired to `MetricNames::AngularCausticness` for its input series (`src/metrics/causticness_analyzer.cpp`).

## Where It Is Used
- Simulation run + probe compute boom via `FrameDetector`, then run `CausticnessAnalyzer`, then evaluate targets (`src/simulation.cpp`, `src/metrics/probe_pipeline.cpp`).
- Filters evaluate `PredictionResult` for target-based constraints (frame or score) (`include/metrics/probe_filter.h`, `src/metrics/probe_filter.cpp`).
- GUI exposes target config (including score type and metric selector), even though score metric is unused (`src/main_gui.cpp`).
- Optimization tool only supports frame targets; score targets are explicitly skipped (`src/main_optimize.cpp`).

## Mismatches / Legacy
- Score metric ignored: `ScoreParams.metric_name` is parsed and editable in GUI, but unused; all scores come from `CausticnessAnalyzer` on `angular_causticness` (`include/optimize/score_predictor.h`, `src/metrics/causticness_analyzer.cpp`).
- Two boom references inside scoring: `CausticnessAnalyzer` uses forced boom event for some stats, but `post_boom_area_normalized` anchors to the peak frame of angular_causticness, not the configured boom (`src/metrics/causticness_analyzer.cpp`).
- Naming drift: config comments mention `post_boom_area` and "composite not yet implemented," but parser expects `post_boom_sustain` and composite is implemented (`config/default.toml`, `include/optimize/prediction_target.h`, `include/optimize/score_predictor.h`).
- Legacy boom path: EventDetector boom criteria is deprecated; boom is now injected from frame detection (`include/metrics/event_detector.h`, `include/metrics/boom_detection.h`).
- `boom_metric` legacy: only used to prefill GUI defaults; does not auto-create targets, so boom detection can be disabled unless `[targets.boom]` exists (`include/config.h`, `src/main_gui.cpp`).
- Python models still reference BoomAnalyzer which is not present in code (`src/pendulum_tools/models.py`).

## "Peak post clarity" Note
- No such symbol exists. Closest are `peak_clarity_score` and `post_boom_area_normalized` from `CausticnessAnalyzer` (`include/metrics/causticness_analyzer.h`, `src/metrics/causticness_analyzer.cpp`).

## Implications vs Metrics Registry
- Frame prediction already behaves like the registry pattern: add a metric once, and any frame detector can use it.
- Score prediction does not: it is hard-coded to one analyzer and one metric, so the registry flexibility does not apply.
