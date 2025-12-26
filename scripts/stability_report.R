#!/usr/bin/env Rscript
# Metric Stability Analysis Report Generator
#
# Usage:
#   Rscript stability_report.R <stability_data.csv> [output_dir]
#
# Generates comprehensive visualizations and analysis of metric stability
# across different pendulum counts.

suppressPackageStartupMessages({
  library(dplyr)
  library(ggplot2)
  library(scales)
})

# =============================================================================
# CONFIGURATION
# =============================================================================

THRESHOLDS <- list(
  excellent = 0.01,
  good = 0.05,
  acceptable = 0.10,
  marginal = 0.20,
  poor = 0.50
)

GRADE_COLORS <- c(
  "A+" = "#2ecc71",
  "A"  = "#27ae60",
  "B"  = "#f1c40f",
  "C"  = "#e67e22",
  "D"  = "#e74c3c",
  "F"  = "#8e44ad"
)

# =============================================================================
# HELPER FUNCTIONS
# =============================================================================

get_grade <- function(cv) {
  ifelse(cv < THRESHOLDS$excellent, "A+",
  ifelse(cv < THRESHOLDS$good, "A",
  ifelse(cv < THRESHOLDS$acceptable, "B",
  ifelse(cv < THRESHOLDS$marginal, "C",
  ifelse(cv < THRESHOLDS$poor, "D", "F")))))
}

theme_stability <- function() {
  theme_minimal() +
    theme(
      plot.title = element_text(size = 14, face = "bold", hjust = 0.5),
      plot.subtitle = element_text(size = 10, hjust = 0.5, color = "gray40"),
      axis.title = element_text(size = 10),
      axis.text = element_text(size = 9),
      legend.position = "right",
      panel.grid.minor = element_blank(),
      strip.text = element_text(face = "bold")
    )
}

# =============================================================================
# DATA LOADING
# =============================================================================

load_and_process_data <- function(csv_path) {
  cat("Loading data from:", csv_path, "\n")

  raw_data <- read.csv(csv_path, stringsAsFactors = FALSE)
  col_names <- names(raw_data)

  metric_cols <- col_names[grepl("_N[0-9]+$", col_names)]
  metrics <- unique(gsub("_N[0-9]+$", "", metric_cols))

  n_matches <- regmatches(metric_cols, regexpr("N[0-9]+$", metric_cols))
  n_values <- unique(as.integer(gsub("N", "", n_matches)))
  n_values <- sort(n_values)

  cat("Found", length(metrics), "metrics\n")
  cat("Pendulum counts:", paste(n_values, collapse = ", "), "\n")
  cat("Frames:", nrow(raw_data), "\n")

  # Reshape to long format
  long_list <- list()
  idx <- 1

  for (col in col_names) {
    if (col == "frame") next
    values <- raw_data[[col]]
    valid_idx <- !is.na(values)
    if (sum(valid_idx) == 0) next

    if (grepl("_mean$", col)) {
      metric <- gsub("_mean$", "", col)
      stat_type <- "mean"
      N <- NA_integer_
    } else if (grepl("_cv$", col)) {
      metric <- gsub("_cv$", "", col)
      stat_type <- "cv"
      N <- NA_integer_
    } else if (grepl("_N[0-9]+$", col)) {
      metric <- gsub("_N[0-9]+$", "", col)
      stat_type <- "value"
      N <- as.integer(gsub(".*_N", "", col))
    } else {
      next
    }

    for (i in which(valid_idx)) {
      long_list[[idx]] <- data.frame(
        frame = raw_data$frame[i],
        value = values[i],
        metric = metric,
        stat_type = stat_type,
        N = N,
        stringsAsFactors = FALSE
      )
      idx <- idx + 1
    }
  }

  long_data <- do.call(rbind, long_list)

  list(
    raw = raw_data,
    long = long_data,
    metrics = metrics,
    n_values = n_values,
    n_frames = nrow(raw_data),
    max_n = max(n_values)
  )
}

# =============================================================================
# ANALYSIS
# =============================================================================

compute_metric_summary <- function(data) {
  cv_data <- data$long %>%
    filter(stat_type == "cv") %>%
    group_by(metric) %>%
    summarize(
      mean_cv = mean(value, na.rm = TRUE),
      median_cv = median(value, na.rm = TRUE),
      max_cv = max(value, na.rm = TRUE),
      min_cv = min(value, na.rm = TRUE),
      unstable_pct = mean(value > 0.10, na.rm = TRUE) * 100,
      .groups = "drop"
    ) %>%
    mutate(grade = get_grade(mean_cv)) %>%
    arrange(mean_cv)

  cv_data
}

# Load the summary CSV with absolute value analysis
load_summary_data <- function(csv_path) {
  # Try to find the summary file
  summary_path <- gsub("\\.csv$", "_summary.csv", csv_path)
  if (!file.exists(summary_path)) {
    return(NULL)
  }

  cat("Loading summary data from:", summary_path, "\n")
  summary_data <- read.csv(summary_path, stringsAsFactors = FALSE)
  return(summary_data)
}

# Extract per-N statistics from the detailed CSV
extract_per_n_stats <- function(data) {
  # Get value data at specific frames (boom frame approximation: around 40-60% of simulation)
  value_data <- data$long %>%
    filter(stat_type == "value", !is.na(N))

  # Compute per-metric, per-N statistics
  stats <- value_data %>%
    group_by(metric, N) %>%
    summarize(
      mean_value = mean(value, na.rm = TRUE),
      max_value = max(value, na.rm = TRUE),
      sd_value = sd(value, na.rm = TRUE),
      .groups = "drop"
    )

  # Add reference values (highest N)
  max_n <- max(stats$N)
  ref_stats <- stats %>%
    filter(N == max_n) %>%
    select(metric, ref_mean = mean_value, ref_max = max_value)

  stats <- stats %>%
    left_join(ref_stats, by = "metric") %>%
    mutate(
      rel_deviation = ifelse(abs(ref_mean) > 1e-10,
                             abs(mean_value - ref_mean) / abs(ref_mean) * 100,
                             0)
    )

  stats
}

# =============================================================================
# PLOTTING FUNCTIONS
# =============================================================================

plot_stability_overview <- function(summary_data) {
  summary_data$metric <- factor(summary_data$metric, levels = summary_data$metric)

  ggplot(summary_data, aes(x = metric, y = mean_cv * 100, fill = grade)) +
    geom_bar(stat = "identity", width = 0.7) +
    geom_hline(yintercept = c(1, 5, 10), linetype = "dashed",
               color = c("green", "orange", "red"), alpha = 0.7) +
    scale_fill_manual(values = GRADE_COLORS, name = "Grade") +
    scale_y_continuous(labels = function(x) paste0(x, "%")) +
    coord_flip() +
    labs(
      title = "Metric Stability Overview",
      subtitle = "Mean coefficient of variation across pendulum counts",
      x = NULL,
      y = "Mean CV (%)"
    ) +
    theme_stability()
}

plot_cv_timeseries <- function(data, metrics_to_plot = NULL, title_suffix = "") {
  cv_data <- data$long %>% filter(stat_type == "cv")

  if (!is.null(metrics_to_plot)) {
    cv_data <- cv_data %>% filter(metric %in% metrics_to_plot)
  }

  if (nrow(cv_data) == 0) return(NULL)

  frame_duration <- 11.0 / data$n_frames
  cv_data$time <- cv_data$frame * frame_duration

  ggplot(cv_data, aes(x = time, y = value * 100, color = metric)) +
    geom_line(alpha = 0.8, linewidth = 0.5) +
    scale_color_viridis_d(name = "Metric") +
    scale_y_continuous(labels = function(x) paste0(x, "%")) +
    geom_hline(yintercept = 10, linetype = "dashed", color = "red", alpha = 0.5) +
    labs(
      title = paste("Coefficient of Variation Over Time", title_suffix),
      subtitle = "How stability changes throughout the simulation",
      x = "Time (seconds)",
      y = "CV (%)"
    ) +
    theme_stability()
}

plot_heatmap <- function(data, summary_data) {
  cv_data <- data$long %>%
    filter(stat_type == "cv") %>%
    mutate(time_bin = cut(frame, breaks = 10, labels = FALSE)) %>%
    group_by(metric, time_bin) %>%
    summarize(mean_cv = mean(value, na.rm = TRUE), .groups = "drop")

  metric_order <- summary_data$metric
  cv_data$metric <- factor(cv_data$metric, levels = rev(metric_order))

  ggplot(cv_data, aes(x = time_bin, y = metric, fill = pmin(mean_cv * 100, 50))) +
    geom_tile() +
    scale_fill_viridis_c(name = "CV (%)", option = "plasma") +
    scale_x_continuous(breaks = 1:10, labels = paste0(seq(10, 100, 10), "%")) +
    labs(
      title = "Stability Heatmap",
      subtitle = "CV across simulation progress and metrics",
      x = "Simulation Progress",
      y = NULL
    ) +
    theme_stability()
}

# Plot metric values across N (the key comparison plot)
plot_metric_comparison <- function(data, metric_name) {
  value_data <- data$long %>%
    filter(stat_type == "value", !is.na(N), metric == metric_name)

  if (nrow(value_data) == 0) return(NULL)

  frame_duration <- 11.0 / data$n_frames
  value_data$time <- value_data$frame * frame_duration
  value_data$N_label <- factor(
    paste0("N=", format(value_data$N, big.mark = ",")),
    levels = paste0("N=", format(sort(unique(value_data$N)), big.mark = ","))
  )

  ggplot(value_data, aes(x = time, y = value, color = N_label)) +
    geom_line(alpha = 0.8, linewidth = 0.6) +
    scale_color_viridis_d(name = "Pendulums") +
    labs(
      title = paste("Metric:", metric_name),
      subtitle = "Value comparison across pendulum counts",
      x = "Time (seconds)",
      y = "Metric Value"
    ) +
    theme_stability()
}

# Plot deviation from highest-N reference
plot_metric_deviation <- function(data, metric_name) {
  value_data <- data$long %>%
    filter(stat_type == "value", !is.na(N), metric == metric_name)

  if (nrow(value_data) == 0) return(NULL)

  max_n <- data$max_n
  frame_duration <- 11.0 / data$n_frames

  # Get reference values (highest N)
  ref_data <- value_data %>%
    filter(N == max_n) %>%
    select(frame, ref_value = value)

  # Compute deviation from reference
  dev_data <- value_data %>%
    filter(N != max_n) %>%
    left_join(ref_data, by = "frame") %>%
    mutate(
      deviation = value - ref_value,
      rel_deviation = ifelse(abs(ref_value) > 1e-10,
                             (value - ref_value) / abs(ref_value) * 100,
                             0),
      time = frame * frame_duration,
      N_label = factor(
        paste0("N=", format(N, big.mark = ",")),
        levels = paste0("N=", format(sort(unique(N)), big.mark = ","))
      )
    )

  # Plot relative deviation
  ggplot(dev_data, aes(x = time, y = rel_deviation, color = N_label)) +
    geom_line(alpha = 0.8, linewidth = 0.6) +
    geom_hline(yintercept = 0, linetype = "solid", color = "black", alpha = 0.3) +
    geom_hline(yintercept = c(-10, 10), linetype = "dashed", color = "red", alpha = 0.3) +
    scale_color_viridis_d(name = "Pendulums") +
    labs(
      title = paste("Metric:", metric_name, "- Deviation from N =", format(max_n, big.mark = ",")),
      subtitle = "Relative difference from highest pendulum count (%)",
      x = "Time (seconds)",
      y = "Relative Deviation (%)"
    ) +
    theme_stability()
}

# Comparison plot (values only)
plot_metric_analysis <- function(data, metric_name, report_dir) {
  p1 <- plot_metric_comparison(data, metric_name)

  if (is.null(p1)) return(FALSE)

  filename_base <- gsub("_", "-", metric_name)

  ggsave(file.path(report_dir, paste0("metric_", filename_base, "_values.png")),
         p1, width = 10, height = 5, dpi = 300)

  return(TRUE)
}

# =============================================================================
# ABSOLUTE VALUE ANALYSIS PLOTS
# =============================================================================

# Plot scale sensitivity (how much metric changes per doubling of N)
plot_scale_sensitivity <- function(abs_summary) {
  if (is.null(abs_summary) || !"scale_sensitivity" %in% names(abs_summary)) {
    return(NULL)
  }

  # Filter out extreme outliers and metrics without sensitivity data
  plot_data <- abs_summary %>%
    filter(!is.na(scale_sensitivity), abs(scale_sensitivity) < 200) %>%
    mutate(
      sensitivity_type = case_when(
        abs(scale_sensitivity) < 1 ~ "N-Independent",
        abs(scale_sensitivity) < 5 ~ "Low",
        abs(scale_sensitivity) < 20 ~ "Moderate",
        TRUE ~ "High"
      ),
      sensitivity_type = factor(sensitivity_type,
                                levels = c("N-Independent", "Low", "Moderate", "High"))
    ) %>%
    arrange(abs(scale_sensitivity))

  plot_data$metric <- factor(plot_data$metric, levels = plot_data$metric)

  ggplot(plot_data, aes(x = metric, y = scale_sensitivity, fill = sensitivity_type)) +
    geom_bar(stat = "identity", width = 0.7) +
    geom_hline(yintercept = 0, linetype = "solid", color = "black", alpha = 0.5) +
    geom_hline(yintercept = c(-5, 5), linetype = "dashed", color = "orange", alpha = 0.5) +
    scale_fill_manual(
      values = c("N-Independent" = "#2ecc71", "Low" = "#3498db",
                 "Moderate" = "#f1c40f", "High" = "#e74c3c"),
      name = "Sensitivity"
    ) +
    coord_flip() +
    labs(
      title = "Scale Sensitivity Analysis",
      subtitle = "Percent change in metric value per doubling of pendulum count",
      x = NULL,
      y = "% Change per 2x N"
    ) +
    theme_stability()
}

# Plot convergence analysis (minimum N needed for <5% deviation)
plot_convergence_analysis <- function(abs_summary) {
  if (is.null(abs_summary) || !"convergence_N" %in% names(abs_summary)) {
    return(NULL)
  }

  plot_data <- abs_summary %>%
    filter(!is.na(convergence_N), convergence_N > 0) %>%
    arrange(convergence_N)

  if (nrow(plot_data) == 0) return(NULL)

  plot_data$metric <- factor(plot_data$metric, levels = plot_data$metric)

  # Color by convergence category
  plot_data <- plot_data %>%
    mutate(
      conv_category = case_when(
        convergence_N <= 1000 ~ "Fast (<1K)",
        convergence_N <= 5000 ~ "Medium (1-5K)",
        convergence_N <= 20000 ~ "Slow (5-20K)",
        TRUE ~ "Very Slow (>20K)"
      ),
      conv_category = factor(conv_category,
                             levels = c("Fast (<1K)", "Medium (1-5K)",
                                        "Slow (5-20K)", "Very Slow (>20K)"))
    )

  ggplot(plot_data, aes(x = metric, y = convergence_N, fill = conv_category)) +
    geom_bar(stat = "identity", width = 0.7) +
    scale_y_log10(labels = scales::comma) +
    scale_fill_manual(
      values = c("Fast (<1K)" = "#2ecc71", "Medium (1-5K)" = "#3498db",
                 "Slow (5-20K)" = "#f1c40f", "Very Slow (>20K)" = "#e74c3c"),
      name = "Convergence"
    ) +
    coord_flip() +
    labs(
      title = "Convergence Analysis",
      subtitle = "Minimum N required for <5% deviation from reference",
      x = NULL,
      y = "Minimum N (log scale)"
    ) +
    theme_stability()
}

# Plot value scaling with N for selected metrics
plot_value_vs_n <- function(per_n_stats, metrics_to_plot = NULL) {
  if (is.null(per_n_stats)) return(NULL)

  plot_data <- per_n_stats

  if (!is.null(metrics_to_plot)) {
    plot_data <- plot_data %>% filter(metric %in% metrics_to_plot)
  }

  if (nrow(plot_data) == 0) return(NULL)

  # Normalize values within each metric for comparison
  plot_data <- plot_data %>%
    group_by(metric) %>%
    mutate(
      normalized_value = mean_value / max(abs(mean_value), na.rm = TRUE)
    ) %>%
    ungroup()

  ggplot(plot_data, aes(x = N, y = normalized_value, color = metric, group = metric)) +
    geom_line(linewidth = 1) +
    geom_point(size = 2) +
    scale_x_log10(labels = scales::comma) +
    labs(
      title = "Value Scaling with Pendulum Count",
      subtitle = "Normalized mean values across different N (1.0 = max value for each metric)",
      x = "Pendulum Count (N, log scale)",
      y = "Normalized Value",
      color = "Metric"
    ) +
    theme_stability() +
    theme(legend.position = "right")
}

# Plot relative deviation from reference for all metrics at each N
plot_deviation_heatmap <- function(per_n_stats) {
  if (is.null(per_n_stats)) return(NULL)

  # Create heatmap data
  heat_data <- per_n_stats %>%
    filter(N != max(N)) %>%  # Exclude reference N
    mutate(
      N_label = factor(format(N, big.mark = ","),
                       levels = format(sort(unique(N)), big.mark = ",")),
      capped_dev = pmin(rel_deviation, 50)  # Cap at 50% for visualization
    )

  # Order metrics by mean deviation
  metric_order <- heat_data %>%
    group_by(metric) %>%
    summarize(mean_dev = mean(rel_deviation, na.rm = TRUE)) %>%
    arrange(mean_dev) %>%
    pull(metric)

  heat_data$metric <- factor(heat_data$metric, levels = metric_order)

  ggplot(heat_data, aes(x = N_label, y = metric, fill = capped_dev)) +
    geom_tile() +
    geom_text(aes(label = sprintf("%.1f%%", rel_deviation)),
              size = 2.5, color = "white") +
    scale_fill_viridis_c(
      name = "Deviation (%)",
      option = "plasma",
      limits = c(0, 50)
    ) +
    labs(
      title = "Deviation from Reference (Highest N)",
      subtitle = "Relative deviation of mean values from reference",
      x = "Pendulum Count",
      y = NULL
    ) +
    theme_stability() +
    theme(axis.text.x = element_text(angle = 45, hjust = 1))
}

# Plot absolute values at a specific frame (e.g., boom frame proxy)
plot_absolute_at_frame <- function(data, target_pct = 0.5) {
  # Get frame at target percentage of simulation
  target_frame <- round(data$n_frames * target_pct)

  value_data <- data$long %>%
    filter(stat_type == "value", !is.na(N), frame == target_frame)

  if (nrow(value_data) == 0) {
    # Try nearest frame
    all_frames <- unique(data$long$frame[data$long$stat_type == "value"])
    target_frame <- all_frames[which.min(abs(all_frames - target_frame))]
    value_data <- data$long %>%
      filter(stat_type == "value", !is.na(N), frame == target_frame)
  }

  if (nrow(value_data) == 0) return(NULL)

  value_data <- value_data %>%
    mutate(N_label = factor(format(N, big.mark = ","),
                            levels = format(sort(unique(N)), big.mark = ",")))

  # Facet by metric with free scales
  ggplot(value_data, aes(x = N_label, y = value, fill = N_label)) +
    geom_bar(stat = "identity", width = 0.7) +
    facet_wrap(~ metric, scales = "free_y", ncol = 4) +
    scale_fill_viridis_d(guide = "none") +
    labs(
      title = sprintf("Absolute Values at Frame %d (%.0f%% of simulation)",
                      target_frame, target_pct * 100),
      subtitle = "Comparing raw metric values across pendulum counts",
      x = "Pendulum Count",
      y = "Value"
    ) +
    theme_stability() +
    theme(
      axis.text.x = element_text(angle = 45, hjust = 1, size = 7),
      strip.text = element_text(size = 8)
    )
}

# =============================================================================
# REPORT GENERATION
# =============================================================================

generate_report <- function(csv_path, output_dir = NULL) {
  if (is.null(output_dir)) {
    output_dir <- dirname(csv_path)
  }

  report_dir <- file.path(output_dir, "stability_report")
  dir.create(report_dir, showWarnings = FALSE, recursive = TRUE)

  # Create subdirectory for individual metric plots
  metrics_dir <- file.path(report_dir, "metrics")
  dir.create(metrics_dir, showWarnings = FALSE, recursive = TRUE)

  cat("\n", paste(rep("=", 70), collapse = ""), "\n", sep = "")
  cat("METRIC STABILITY REPORT\n")
  cat(paste(rep("=", 70), collapse = ""), "\n\n", sep = "")

  # Load data
  data <- load_and_process_data(csv_path)
  summary_data <- compute_metric_summary(data)

  # Load absolute value summary if available
  abs_summary <- load_summary_data(csv_path)

  # Extract per-N statistics for additional analysis
  per_n_stats <- extract_per_n_stats(data)

  # Print text summary
  cat("\n--- STABILITY GRADES ---\n\n")
  for (grade in c("A+", "A", "B", "C", "D", "F")) {
    metrics_with_grade <- summary_data$metric[summary_data$grade == grade]
    if (length(metrics_with_grade) > 0) {
      cat(sprintf("%s: %s\n", grade, paste(metrics_with_grade, collapse = ", ")))
    }
  }

  cat("\n--- SUMMARY STATISTICS ---\n\n")
  print_data <- summary_data
  print_data$mean_cv <- sprintf("%.2f%%", print_data$mean_cv * 100)
  print_data$max_cv <- sprintf("%.2f%%", print_data$max_cv * 100)
  print_data$unstable_pct <- sprintf("%.1f%%", print_data$unstable_pct)
  print(print_data[, c("metric", "mean_cv", "max_cv", "unstable_pct", "grade")])

  # Print absolute value analysis if available
  if (!is.null(abs_summary)) {
    cat("\n--- ABSOLUTE VALUE ANALYSIS ---\n\n")
    abs_print <- abs_summary %>%
      select(metric, scale_sensitivity, scale_correlation, convergence_N) %>%
      mutate(
        scale_sensitivity = sprintf("%.2f%%", scale_sensitivity),
        scale_correlation = sprintf("%.3f", scale_correlation),
        convergence_N = ifelse(is.na(convergence_N) | convergence_N <= 0,
                               "N/A", format(convergence_N, big.mark = ","))
      )
    print(abs_print)
  }

  cat("\n--- GENERATING PLOTS ---\n\n")

  plot_num <- 1

  # 1. Overview
  cat(sprintf("%d. Stability overview...\n", plot_num))
  p1 <- plot_stability_overview(summary_data)
  ggsave(file.path(report_dir, sprintf("%02d_stability_overview.png", plot_num)), p1,
         width = 10, height = 8, dpi = 300)
  plot_num <- plot_num + 1

  # 2. Heatmap
  cat(sprintf("%d. Stability heatmap...\n", plot_num))
  p2 <- plot_heatmap(data, summary_data)
  ggsave(file.path(report_dir, sprintf("%02d_stability_heatmap.png", plot_num)), p2,
         width = 12, height = 8, dpi = 300)
  plot_num <- plot_num + 1

  # 3. CV time series - stable metrics
  cat(sprintf("%d. CV time series (stable metrics)...\n", plot_num))
  stable_metrics <- summary_data$metric[summary_data$mean_cv < 0.05]
  if (length(stable_metrics) > 0) {
    p3 <- plot_cv_timeseries(data, stable_metrics, "(Stable)")
    if (!is.null(p3)) {
      ggsave(file.path(report_dir, sprintf("%02d_cv_timeseries_stable.png", plot_num)), p3,
             width = 12, height = 6, dpi = 300)
    }
  }
  plot_num <- plot_num + 1

  # 4. CV time series - unstable metrics
  cat(sprintf("%d. CV time series (unstable metrics)...\n", plot_num))
  unstable_metrics <- summary_data$metric[summary_data$mean_cv >= 0.05 &
                                           summary_data$mean_cv < 1.0]
  if (length(unstable_metrics) > 0) {
    p4 <- plot_cv_timeseries(data, unstable_metrics, "(Unstable)")
    if (!is.null(p4)) {
      ggsave(file.path(report_dir, sprintf("%02d_cv_timeseries_unstable.png", plot_num)), p4,
             width = 12, height = 6, dpi = 300)
    }
  }
  plot_num <- plot_num + 1

  # === ABSOLUTE VALUE ANALYSIS PLOTS ===
  cat(sprintf("%d. Scale sensitivity analysis...\n", plot_num))
  if (!is.null(abs_summary)) {
    p_sens <- plot_scale_sensitivity(abs_summary)
    if (!is.null(p_sens)) {
      ggsave(file.path(report_dir, sprintf("%02d_scale_sensitivity.png", plot_num)), p_sens,
             width = 10, height = 8, dpi = 300)
    }
  }
  plot_num <- plot_num + 1

  cat(sprintf("%d. Convergence analysis...\n", plot_num))
  if (!is.null(abs_summary)) {
    p_conv <- plot_convergence_analysis(abs_summary)
    if (!is.null(p_conv)) {
      ggsave(file.path(report_dir, sprintf("%02d_convergence_analysis.png", plot_num)), p_conv,
             width = 10, height = 8, dpi = 300)
    }
  }
  plot_num <- plot_num + 1

  cat(sprintf("%d. Deviation heatmap...\n", plot_num))
  p_dev <- plot_deviation_heatmap(per_n_stats)
  if (!is.null(p_dev)) {
    ggsave(file.path(report_dir, sprintf("%02d_deviation_heatmap.png", plot_num)), p_dev,
           width = 12, height = 10, dpi = 300)
  }
  plot_num <- plot_num + 1

  cat(sprintf("%d. Value scaling (causticness metrics)...\n", plot_num))
  # Dynamically find causticness metrics from the data
  causticness_metrics <- data$metrics[grepl("caustic|concentration", data$metrics)]
  p_scale <- plot_value_vs_n(per_n_stats, causticness_metrics)
  if (!is.null(p_scale)) {
    ggsave(file.path(report_dir, sprintf("%02d_value_scaling_causticness.png", plot_num)), p_scale,
           width = 12, height = 6, dpi = 300)
  }
  plot_num <- plot_num + 1

  cat(sprintf("%d. Absolute values at mid-simulation...\n", plot_num))
  p_abs <- plot_absolute_at_frame(data, 0.5)
  if (!is.null(p_abs)) {
    ggsave(file.path(report_dir, sprintf("%02d_absolute_values_midpoint.png", plot_num)), p_abs,
           width = 16, height = 12, dpi = 300)
  }
  plot_num <- plot_num + 1

  # Individual metric analysis (ALL METRICS)
  cat(sprintf("%d. Individual metric analysis (all %d metrics)...\n", plot_num, length(data$metrics)))
  for (m in data$metrics) {
    cat("   -", m, "\n")
    plot_metric_analysis(data, m, metrics_dir)
  }
  plot_num <- plot_num + 1

  # Save summary table
  cat(sprintf("%d. Saving summary table...\n", plot_num))
  write.csv(summary_data, file.path(report_dir, "stability_summary.csv"),
            row.names = FALSE)
  plot_num <- plot_num + 1

  # Generate index
  cat(sprintf("%d. Generating index...\n", plot_num))
  generate_index(report_dir, metrics_dir, data$metrics, summary_data, abs_summary)

  cat("\n", paste(rep("=", 70), collapse = ""), "\n", sep = "")
  cat("Report generated in:", report_dir, "\n")
  cat("Individual metric plots in:", metrics_dir, "\n")
  cat(paste(rep("=", 70), collapse = ""), "\n", sep = "")

  invisible(list(data = data, summary = summary_data, abs_summary = abs_summary,
                 report_dir = report_dir))
}

# Generate a simple HTML index for easy browsing
generate_index <- function(report_dir, metrics_dir, metrics, summary_data, abs_summary = NULL) {
  html <- '<!DOCTYPE html>
<html>
<head>
  <title>Metric Stability Report</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; max-width: 1400px; }
    h1 { color: #333; }
    h2 { color: #666; border-bottom: 1px solid #ddd; padding-bottom: 10px; margin-top: 30px; }
    img { max-width: 100%; margin: 10px 0; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
    .metric-section { margin: 20px 0; padding: 15px; background: #f9f9f9; border-radius: 5px; }
    .grade-A\\+ { color: #2ecc71; font-weight: bold; }
    .grade-A { color: #27ae60; font-weight: bold; }
    .grade-B { color: #f1c40f; font-weight: bold; }
    .grade-C { color: #e67e22; font-weight: bold; }
    .grade-D { color: #e74c3c; font-weight: bold; }
    .grade-F { color: #8e44ad; font-weight: bold; }
    table { border-collapse: collapse; width: 100%; margin: 15px 0; }
    th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
    th { background: #f4f4f4; }
    .plot-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(500px, 1fr)); gap: 20px; }
    .plot-container { background: white; padding: 10px; border-radius: 5px; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }
    .sensitivity-low { background-color: #d4edda; }
    .sensitivity-moderate { background-color: #fff3cd; }
    .sensitivity-high { background-color: #f8d7da; }
  </style>
</head>
<body>
  <h1>Metric Stability Analysis Report</h1>

  <h2>CV Stability Analysis</h2>
  <p>These plots show how the Coefficient of Variation (relative variability) changes across pendulum counts and simulation time.</p>
  <div class="plot-grid">
    <div class="plot-container"><img src="01_stability_overview.png" alt="Stability Overview"></div>
    <div class="plot-container"><img src="02_stability_heatmap.png" alt="Stability Heatmap"></div>
    <div class="plot-container"><img src="03_cv_timeseries_stable.png" alt="CV Timeseries (Stable)"></div>
    <div class="plot-container"><img src="04_cv_timeseries_unstable.png" alt="CV Timeseries (Unstable)"></div>
  </div>

  <h2>Absolute Value Analysis</h2>
  <p>These plots show how the <strong>actual metric values</strong> (not just relative stability) change with pendulum count.
  Critical for threshold-based detection methods that depend on absolute values.</p>
  <div class="plot-grid">
    <div class="plot-container"><img src="05_scale_sensitivity.png" alt="Scale Sensitivity"></div>
    <div class="plot-container"><img src="06_convergence_analysis.png" alt="Convergence Analysis"></div>
    <div class="plot-container"><img src="07_deviation_heatmap.png" alt="Deviation Heatmap"></div>
    <div class="plot-container"><img src="08_value_scaling_causticness.png" alt="Value Scaling (Causticness)"></div>
    <div class="plot-container"><img src="09_absolute_values_midpoint.png" alt="Absolute Values at Midpoint"></div>
  </div>
'

  # Add absolute value summary table if available
  if (!is.null(abs_summary) && nrow(abs_summary) > 0) {
    html <- paste0(html, '
  <h2>Absolute Value Summary Table</h2>
  <p><strong>Scale Sensitivity:</strong> % change per doubling of N. <strong>Convergence N:</strong> Minimum N for &lt;5% deviation from reference.</p>
  <table>
    <tr>
      <th>Metric</th>
      <th>Scale Sensitivity</th>
      <th>Scale Correlation</th>
      <th>Convergence N</th>
      <th>Reference Value</th>
    </tr>
')

    for (i in seq_len(nrow(abs_summary))) {
      row <- abs_summary[i, ]
      sens_class <- if (!is.na(row$scale_sensitivity)) {
        if (abs(row$scale_sensitivity) < 5) "sensitivity-low"
        else if (abs(row$scale_sensitivity) < 20) "sensitivity-moderate"
        else "sensitivity-high"
      } else ""

      conv_str <- if (is.na(row$convergence_N) || row$convergence_N <= 0) "N/A"
                  else format(row$convergence_N, big.mark = ",")
      ref_str <- if ("ref_value_at_boom" %in% names(row) && !is.na(row$ref_value_at_boom))
                   sprintf("%.4f", row$ref_value_at_boom)
                 else if ("ref_mean" %in% names(row) && !is.na(row$ref_mean))
                   sprintf("%.4f", row$ref_mean)
                 else "N/A"

      html <- paste0(html, sprintf(
        '    <tr>
      <td>%s</td>
      <td class="%s">%.2f%%</td>
      <td>%.3f</td>
      <td>%s</td>
      <td>%s</td>
    </tr>
',
        row$metric,
        sens_class,
        ifelse(is.na(row$scale_sensitivity), 0, row$scale_sensitivity),
        ifelse(is.na(row$scale_correlation), 0, row$scale_correlation),
        conv_str,
        ref_str
      ))
    }
    html <- paste0(html, '  </table>\n')
  }

  html <- paste0(html, '
  <h2>Individual Metrics</h2>
  <p>Detailed time series comparison plots for each metric:</p>
')

  # Add links to each metric
  for (m in metrics) {
    grade <- summary_data$grade[summary_data$metric == m]
    if (length(grade) == 0) grade <- "?"
    grade_class <- paste0("grade-", gsub("\\+", "\\\\+", grade))
    filename_base <- gsub("_", "-", m)

    html <- paste0(html, sprintf(
      '  <div class="metric-section">
    <h3>%s <span class="%s">[%s]</span></h3>
    <div class="plot-grid">
      <div class="plot-container"><img src="metrics/metric_%s_values.png" alt="%s values"></div>
    </div>
  </div>
', m, grade_class, grade, filename_base, m))
  }

  html <- paste0(html, '
</body>
</html>')

  writeLines(html, file.path(report_dir, "index.html"))
}

# =============================================================================
# MAIN
# =============================================================================

main <- function() {
  args <- commandArgs(trailingOnly = TRUE)

  if (length(args) < 1) {
    cat("Usage: Rscript stability_report.R <stability_data.csv> [output_dir]\n")
    cat("\nGenerates comprehensive stability analysis report with:\n")
    cat("  - Overview plots (heatmap, CV timeseries)\n")
    cat("  - Individual metric comparison and deviation plots\n")
    cat("  - HTML index for easy browsing\n")
    quit(status = 1)
  }

  csv_path <- args[1]
  output_dir <- if (length(args) >= 2) args[2] else NULL

  if (!file.exists(csv_path)) {
    cat("Error: File not found:", csv_path, "\n")
    quit(status = 1)
  }

  generate_report(csv_path, output_dir)
}

if (!interactive()) {
  main()
}
