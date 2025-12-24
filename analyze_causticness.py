#!/usr/bin/env python3
"""
Causticness peak analysis - Python port of C++ CausticnessAnalyzer.
Exactly replicates findPeaks() and peak clarity metrics.
"""

import sys
from dataclasses import dataclass, field
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np


@dataclass
class CausticnessPeak:
    frame: int = -1
    value: float = 0.0
    seconds: float = 0.0
    prominence: float = 0.0  # How much peak rises above surrounding terrain


@dataclass
class CausticnessMetrics:
    # Basic metrics
    peak_causticness: float = 0.0
    peak_frame: int = -1
    peak_seconds: float = 0.0
    average_causticness: float = 0.0
    time_above_threshold: float = 0.0
    frames_above_threshold: int = 0
    total_causticness: float = 0.0

    # Peak clarity analysis
    peak_clarity_score: float = 1.0
    competing_peaks_count: int = 0
    max_competitor_ratio: float = 0.0
    nearest_competitor_seconds: float = 0.0

    # Post-boom sustain
    post_boom_area: float = 0.0
    post_boom_area_normalized: float = 0.0
    post_boom_duration: float = 0.0

    def quality_score(self) -> float:
        """Normalized quality score (0-1) matching C++ implementation."""
        # Peak causticness (0-1 range, saturates at 1.0)
        peak_score = min(1.0, self.peak_causticness)
        # Post-boom sustain
        sustain_score = self.post_boom_area_normalized
        # Peak clarity
        clarity_score = self.peak_clarity_score
        # Weight: clarity most important, then peak, then sustain
        return clarity_score * 0.4 + peak_score * 0.35 + sustain_score * 0.25


@dataclass
class CausticnessAnalyzer:
    # Configuration (matching C++ defaults)
    quality_threshold: float = 0.25
    post_boom_window_seconds: float = 10.0
    min_peak_separation: float = 0.3
    min_peak_height_fraction: float = 0.1
    min_prominence_fraction: float = 0.05  # NEW: min prominence as fraction of global max
    frame_duration: float = 1.0 / 60.0

    # Results
    metrics: CausticnessMetrics = field(default_factory=CausticnessMetrics)
    detected_peaks: list = field(default_factory=list)

    def compute_prominence(self, values: np.ndarray, peak_idx: int) -> float:
        """
        Compute prominence of a peak.

        Prominence = peak_value - max(left_base, right_base)
        where left_base is the minimum between the peak and the nearest higher peak to the left
        (or edge), and right_base is the same for the right side.

        This is a standard signal processing metric for peak significance.
        """
        peak_val = values[peak_idx]
        n = len(values)

        # Find left base: go left until we hit a higher value or edge
        left_min = peak_val
        for i in range(peak_idx - 1, -1, -1):
            if values[i] > peak_val:
                # Found a higher point, stop
                break
            if values[i] < left_min:
                left_min = values[i]

        # Find right base: go right until we hit a higher value or edge
        right_min = peak_val
        for i in range(peak_idx + 1, n):
            if values[i] > peak_val:
                # Found a higher point, stop
                break
            if values[i] < right_min:
                right_min = values[i]

        # Prominence is height above the higher of the two bases
        base = max(left_min, right_min)
        return peak_val - base

    def find_peaks(self, values: np.ndarray) -> list[CausticnessPeak]:
        """
        Find peaks in causticness curve with prominence filtering.

        Algorithm:
        1. Find all local maxima (point higher than both neighbors)
        2. Filter by minimum height (fraction of global max)
        3. Filter by minimum prominence (fraction of global max)
        4. Enforce minimum separation (keep higher peak if too close)
        """
        if len(values) < 3:
            return []

        # Calculate thresholds
        min_sep_frames = max(1, int(self.min_peak_separation / self.frame_duration))
        global_max = np.max(values)
        min_height = global_max * self.min_peak_height_fraction
        min_prominence = global_max * self.min_prominence_fraction

        # Step 1: Find all local maxima above minimum height
        candidates = []
        for i in range(1, len(values) - 1):
            if (values[i] > values[i - 1] and
                values[i] > values[i + 1] and
                values[i] >= min_height):
                candidates.append(i)

        # Step 2: Filter by prominence
        prominent_peaks = []
        for idx in candidates:
            prom = self.compute_prominence(values, idx)
            if prom >= min_prominence:
                prominent_peaks.append((idx, values[idx], prom))

        # Step 3: Enforce minimum separation (keep higher peak)
        peaks = []
        for idx, val, prom in prominent_peaks:
            if not peaks or (idx - peaks[-1].frame) >= min_sep_frames:
                peak = CausticnessPeak(
                    frame=idx,
                    value=val,
                    seconds=idx * self.frame_duration
                )
                peak.prominence = prom  # Store for debugging
                peaks.append(peak)
            elif val > peaks[-1].value:
                # Replace previous peak if this one is higher and within separation
                peaks[-1].frame = idx
                peaks[-1].value = val
                peaks[-1].seconds = idx * self.frame_duration
                peaks[-1].prominence = prom

        return peaks

    def compute_peak_clarity(self, values: np.ndarray):
        """
        Compute peak clarity metrics.
        Exact port of C++ CausticnessAnalyzer::computePeakClarity().
        """
        self.detected_peaks = self.find_peaks(values)

        if not self.detected_peaks:
            self.metrics.peak_clarity_score = 1.0
            self.metrics.competing_peaks_count = 0
            self.metrics.max_competitor_ratio = 0.0
            self.metrics.nearest_competitor_seconds = 0.0
            return

        # Find the main peak (highest)
        main_peak = max(self.detected_peaks, key=lambda p: p.value)
        main_value = main_peak.value
        main_frame = main_peak.frame

        # Find all peaks before the main peak
        max_preceding = 0.0
        nearest_distance = float('inf')
        competing_count = 0

        for peak in self.detected_peaks:
            if peak.frame < main_frame:
                competing_count += 1
                if peak.value > max_preceding:
                    max_preceding = peak.value
                distance = (main_frame - peak.frame) * self.frame_duration
                if distance < nearest_distance:
                    nearest_distance = distance

        self.metrics.competing_peaks_count = competing_count

        if max_preceding == 0.0:
            # No preceding peaks - perfect clarity
            self.metrics.peak_clarity_score = 1.0
            self.metrics.max_competitor_ratio = 0.0
            self.metrics.nearest_competitor_seconds = 0.0
        else:
            # Score: main / (main + competitor) gives 0.5-1.0 range
            self.metrics.peak_clarity_score = main_value / (main_value + max_preceding)
            self.metrics.max_competitor_ratio = max_preceding / main_value
            self.metrics.nearest_competitor_seconds = nearest_distance

    def compute_post_boom_area(self, values: np.ndarray):
        """
        Compute post-boom area metrics.
        Exact port of C++ CausticnessAnalyzer::computePostBoomArea().
        """
        if self.metrics.peak_frame < 0 or len(values) == 0:
            self.metrics.post_boom_area = 0.0
            self.metrics.post_boom_area_normalized = 0.0
            self.metrics.post_boom_duration = 0.0
            return

        boom_frame = self.metrics.peak_frame
        remaining_seconds = (len(values) - boom_frame) * self.frame_duration
        window_seconds = min(self.post_boom_window_seconds, remaining_seconds)
        window_frames = int(window_seconds / self.frame_duration)

        if window_frames <= 0:
            self.metrics.post_boom_area = 0.0
            self.metrics.post_boom_area_normalized = 0.0
            self.metrics.post_boom_duration = 0.0
            return

        end_frame = min(boom_frame + window_frames, len(values))
        area = np.sum(values[boom_frame:end_frame])

        self.metrics.post_boom_area = area * self.frame_duration
        self.metrics.post_boom_duration = window_seconds

        # Normalize: area / (window * peak)
        max_possible_area = window_frames * self.metrics.peak_causticness
        if max_possible_area > 0.0:
            self.metrics.post_boom_area_normalized = min(1.0, area / max_possible_area)
        else:
            self.metrics.post_boom_area_normalized = 0.0

    def analyze(self, values: np.ndarray):
        """
        Full analysis of causticness curve.
        Exact port of C++ CausticnessAnalyzer::analyze().
        """
        self.metrics = CausticnessMetrics()
        self.detected_peaks = []

        if len(values) < 3:
            return

        # Find overall peak and statistics
        total = 0.0
        max_val = 0.0
        max_frame = 0
        frames_above = 0

        for i, val in enumerate(values):
            total += val
            if val > max_val:
                max_val = val
                max_frame = i
            if val >= self.quality_threshold:
                frames_above += 1

        self.metrics.peak_causticness = max_val
        self.metrics.peak_frame = max_frame
        self.metrics.peak_seconds = max_frame * self.frame_duration
        self.metrics.average_causticness = total / len(values)
        self.metrics.frames_above_threshold = frames_above
        self.metrics.time_above_threshold = frames_above * self.frame_duration
        self.metrics.total_causticness = total

        # Peak clarity analysis
        self.compute_peak_clarity(values)

        # Post-boom area calculation
        self.compute_post_boom_area(values)


def main():
    if len(sys.argv) < 2:
        csv_path = "output/long_test/run_20251224_092802/metrics.csv"
    else:
        csv_path = sys.argv[1]

    # Load data
    df = pd.read_csv(csv_path)
    values = df['angular_causticness'].values
    frames = df['frame'].values

    # Auto-detect frame duration from frame count and typical simulation
    # Assume 60fps unless we can infer otherwise
    frame_duration = 1.0 / 60.0

    # Run analysis
    analyzer = CausticnessAnalyzer(frame_duration=frame_duration)
    analyzer.analyze(values)
    m = analyzer.metrics
    peaks = analyzer.detected_peaks

    # Report
    print("=" * 60)
    print("CAUSTICNESS ANALYSIS REPORT")
    print("=" * 60)
    print(f"CSV: {csv_path}")
    print(f"Total frames: {len(values)}")
    print(f"Frame duration: {frame_duration:.4f}s ({1/frame_duration:.1f} fps)")
    print(f"Total duration: {len(values) * frame_duration:.2f}s")
    print()

    print("BASIC METRICS")
    print("-" * 40)
    print(f"  Peak causticness:     {m.peak_causticness:.6f}")
    print(f"  Peak frame:           {m.peak_frame} ({m.peak_seconds:.2f}s)")
    print(f"  Average causticness:  {m.average_causticness:.6f}")
    print(f"  Total (area):         {m.total_causticness:.4f}")
    print(f"  Frames above {analyzer.quality_threshold}: {m.frames_above_threshold}")
    print(f"  Time above threshold: {m.time_above_threshold:.2f}s")
    print()

    print("PEAK DETECTION")
    print("-" * 40)
    print(f"  Min separation:       {analyzer.min_peak_separation}s ({int(analyzer.min_peak_separation/frame_duration)} frames)")
    print(f"  Min height fraction:  {analyzer.min_peak_height_fraction} (threshold: {m.peak_causticness * analyzer.min_peak_height_fraction:.6f})")
    print(f"  Min prominence frac:  {analyzer.min_prominence_fraction} (threshold: {m.peak_causticness * analyzer.min_prominence_fraction:.6f})")
    print(f"  Peaks detected:       {len(peaks)}")
    if peaks:
        print()
        print("  Detected peaks (with prominence):")
        for i, p in enumerate(peaks):
            marker = " <-- MAIN" if p.frame == m.peak_frame else ""
            print(f"    [{i+1}] frame {p.frame:4d} ({p.seconds:6.2f}s): {p.value:.6f} [prom: {p.prominence:.6f}]{marker}")
    print()

    print("PEAK CLARITY METRICS")
    print("-" * 40)
    print(f"  Peak clarity score:   {m.peak_clarity_score:.4f} (0.5=equal, 1.0=no competition)")
    print(f"  Competing peaks:      {m.competing_peaks_count}")
    print(f"  Max competitor ratio: {m.max_competitor_ratio:.4f}")
    if m.nearest_competitor_seconds > 0:
        print(f"  Nearest competitor:   {m.nearest_competitor_seconds:.2f}s before main")
    print()

    print("POST-BOOM SUSTAIN")
    print("-" * 40)
    print(f"  Window duration:      {m.post_boom_duration:.2f}s")
    print(f"  Post-boom area:       {m.post_boom_area:.4f}")
    print(f"  Area normalized:      {m.post_boom_area_normalized:.4f} (0-1)")
    print()

    print("QUALITY SCORE")
    print("-" * 40)
    # Breakdown (new formula: clarity * 0.4 + peak * 0.35 + sustain * 0.25)
    peak_score = min(1.0, m.peak_causticness)
    sustain_score = m.post_boom_area_normalized
    clarity_score = m.peak_clarity_score
    print(f"  Clarity component:    {clarity_score:.4f} * 0.40 = {clarity_score * 0.4:.4f}")
    print(f"  Peak component:       {peak_score:.4f} * 0.35 = {peak_score * 0.35:.4f}")
    print(f"  Sustain component:    {sustain_score:.4f} * 0.25 = {sustain_score * 0.25:.4f}")
    print(f"  TOTAL QUALITY SCORE:  {m.quality_score():.4f}")
    print("=" * 60)

    # Plot
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True)

    time = frames * frame_duration

    # Main causticness plot
    ax1.plot(time, values, 'b-', linewidth=0.8, alpha=0.8, label='Angular Causticness')
    ax1.axhline(y=analyzer.quality_threshold, color='r', linestyle='--', alpha=0.5,
                label=f'Threshold ({analyzer.quality_threshold})')

    # Mark detected peaks
    if peaks:
        peak_times = [p.seconds for p in peaks]
        peak_vals = [p.value for p in peaks]
        ax1.scatter(peak_times, peak_vals, c='orange', s=50, zorder=5,
                    edgecolors='black', linewidths=0.5, label='Detected Peaks')

        # Highlight main peak
        ax1.scatter([m.peak_seconds], [m.peak_causticness], c='red', s=120,
                    zorder=6, marker='*', edgecolors='black', linewidths=0.5,
                    label=f'Main Peak ({m.peak_seconds:.2f}s)')

    ax1.set_ylabel('Causticness')
    ax1.set_title(f'Angular Causticness Analysis\n'
                  f'Peak Clarity: {m.peak_clarity_score:.3f} | '
                  f'Competing Peaks: {m.competing_peaks_count} | '
                  f'Quality Score: {m.quality_score():.3f}')
    ax1.legend(loc='upper right')
    ax1.grid(True, alpha=0.3)

    # Zoomed view around peak
    if m.peak_frame >= 0:
        zoom_start = max(0, m.peak_seconds - 5)
        zoom_end = min(time[-1], m.peak_seconds + 10)
        mask = (time >= zoom_start) & (time <= zoom_end)

        ax2.plot(time[mask], values[mask], 'b-', linewidth=1.2, label='Angular Causticness')

        # Mark peaks in zoom window
        for p in peaks:
            if zoom_start <= p.seconds <= zoom_end:
                color = 'red' if p.frame == m.peak_frame else 'orange'
                size = 120 if p.frame == m.peak_frame else 50
                ax2.scatter([p.seconds], [p.value], c=color, s=size, zorder=5,
                           edgecolors='black', linewidths=0.5)
                ax2.annotate(f'{p.value:.4f}', (p.seconds, p.value),
                            textcoords="offset points", xytext=(0, 10),
                            ha='center', fontsize=8)

        ax2.axvline(x=m.peak_seconds, color='red', linestyle=':', alpha=0.5)
        ax2.set_xlim(zoom_start, zoom_end)

    ax2.set_xlabel('Time (seconds)')
    ax2.set_ylabel('Causticness')
    ax2.set_title(f'Zoomed View Around Main Peak ({m.peak_seconds:.2f}s)')
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()

    # Save plot
    output_path = csv_path.replace('.csv', '_causticness_analysis.png')
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"\nPlot saved: {output_path}")

    plt.show()


if __name__ == "__main__":
    main()
