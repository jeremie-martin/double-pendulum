#pragma once

// ============================================================================
// DEPRECATED: This file is provided for backward compatibility only.
// Use signal_analyzer.h instead.
//
// The CausticnessAnalyzer class has been renamed to SignalAnalyzer because
// its analysis (peak detection, clarity scoring, post-reference area) is
// generic and works with any metric series, not just causticness.
// ============================================================================

#include "metrics/signal_analyzer.h"

// All types are already aliased in signal_analyzer.h:
//   using CausticnessAnalyzer = SignalAnalyzer;
//   using CausticnessMetrics = SignalMetrics;
//   using CausticnessPeak = SignalPeak;
