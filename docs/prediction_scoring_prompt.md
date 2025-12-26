# Improved Prompt (Rewritten)

I recently implemented a new **metric registry** that turned out beautifully — clean, flexible, and fully decoupled.
Adding a new metric now requires only a single registry entry + implementation, and the entire system (config, GUI, optimizers, analyzers, etc.) picks it up automatically.
This is exactly the design quality I want everywhere.

However, the **prediction / scoring subsystem** is still based on old, legacy logic.
Before refactoring anything, I need a deep, precise analysis of how it currently works, what is conceptually broken, and how we should redesign it.

---

# What We Currently Have (My Understanding)

There are essentially **two types of predictions** in the system:

1. **Frame predictions**

   - Predict an index (frame number) from a metric series.
   - The current `FrameDetector` is generic and works on any metric.
   - This part is actually well-designed.

2. **Score predictions**

   - Predict a real number in **[0, 1]**.
   - The implementation is inflexible, tied to legacy logic, and conceptually unclear.

You will find large inconsistencies:

- Score predictors are not generic: `ScorePredictor` ignores the metric name and hard-codes `CausticnessAnalyzer`.
- `ScoreParams.metric_name` is parsed (and required) but unused.
- `CausticnessAnalyzer` is hard-wired to `MetricNames::AngularCausticness` (we definitely DON'T want to use only one metric, just like the frame dectection we need this to work with any metrics)
- The GUI exposes score metric selection, but it does nothing.
- Optimization explicitly skips score targets.
- Naming is inconsistent (`peak_clarity`, `post_boom_area`, `post_boom_sustain`, etc.).
- Legacy boom logic is half-removed and half-alive.
- Post-boom sustain uses the *peak* frame (angular_causticness), not the configured boom frame.
- Python models reference analyzers that no longer exist in C++.
- Some features (like peak clarity) exist and are defined in code, but are not clearly surfaced in documentation or UI.

Below is the summary of my own investigation, which you should verify:

## Current Architecture Summary (to be validated)

- Frame targets -> fully generic -> good architecture.
- Score targets -> tied to causticness logic -> legacy, inconsistent.
- Boom detection -> generic detection + forced legacy event path.
- GUI and config support a generic abstraction, but the backend implementation is not generic.

---

# What I Need You To Do (Step 1): Deep Analysis

First, I need you to switch into **plan mode**.

Before proposing any solution, I need:

## A complete, accurate, up-to-date explanation of:

1. How predictions (frame + score) currently work, end-to-end.
2. How each component interacts with config, metrics, GUI, optimization, etc.
3. What assumptions, concepts, and abstractions exist today — both intended and accidental.
4. Which parts are sound, and which are fundamentally flawed or need redesign.
5. How the scoring system is actually used in the codebase:

   - Where values get consumed
   - Where prediction results influence logic
   - Where the system expects generic behavior but gets legacy behavior

6. Identify redundancy, dead layers, outdated naming, and non-generic design.

**Do not rely on comments or documentation — they are outdated. Base your analysis on code only.**

---

# Step 2: Conceptual Redesign

Design a future-proof system (and a plan for implementing it).

## High-level goal

Rebuild the scoring/prediction subsystem with the **same design quality** as the metric registry:

- Simple
- Generic
- Extensible
- Zero hard-coded assumptions
- Automatically integrated through a central registry

## Key questions to resolve

1. Should “frame prediction” and “score prediction” remain separate abstractions?
   Or are they two variants of the same concept (predict X from metrics)?

2. What is the correct terminology?
   (Predictor, scorer, analyzer, etc.)

3. What is the right abstraction boundary?

   - A prediction method takes metric time-series -> outputs value(s).
   - No method should be tied to one specific metric.

4. What should a prediction registry look like?
   Ideally parallel to the metric registry.

5. How will config files reference predictors?
   Can the existing `[targets.*]` structure remain unchanged?

6. How will frame predictors and score predictors integrate with:

   - optimization
   - simulation
   - GUI
   - probe filters
   - analyzers
   - event systems

7. Which legacy components should be deleted or merged?
   (This should be decided based on the analysis, not assumed.)

8. How to ensure the new design is robust, minimal, consistent, and flexible?

---

# Step 3: Implementation Plan

Finally, once the conceptual redesign is clear, produce a **full, self-contained implementation plan**, similar to what we did for the metric registry refactoring.

The plan must specify:

- Exact classes/files to modify, remove, or create
- Required registry design
- Naming conventions
- How config parsing should work
- How the GUI should adapt
- How optimization pipelines incorporate the new generic predictors
- How to update existing tools that depend on prediction
- What tests must be added/updated
- Migration notes (if any)

The plan must be:

- Precise (no vagueness)
- Actionable (a developer could follow it step-by-step)
- Self-contained (no references to this conversation)
- Concise but complete

---

# Additional Notes

- You can assume we will **not preserve backward compatibility** for legacy scoring.
- Feel free to propose renaming concepts that are confusing or misleading.
- Please think deeply and systematically — this redesign is important.
- The end state should feel as elegant as the metric registry.
- For validation: after the redesign, add dummy prediction implementations (one frame, one score) that always return a constant value, and ensure they can be used in the optimization tool (run `./build/pendulum-optimize --grid-steps 2 output/eval2/annotations.json`).

If you’d like, I can also add an example of what a predictor registry entry might look like, but for now I prefer you think without being biased by my ideas.

This is a complex task, but I trust you.
