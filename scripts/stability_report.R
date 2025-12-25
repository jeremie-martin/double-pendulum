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

# Stability thresholds (as fractions, not percentages)
THRESHOLDS <- list(
  excellent = 0.01,
  good = 0.05,
  acceptable = 0.10,
  marginal = 0.20,
  poor = 0.50
)

# Color palette for grades
GRADE_COLORS <- c(
  "A+" = "#2ecc71",
  "A"  = "#27ae60",
  "B"  = "#f1c40f",
  "C"  = "#e67e22",
  "D"  = "#e74c3c",
  "F"  = "#8e44ad"
)

# Metric categories for grouping
METRIC_CATEGORIES <- list(
  "Basic Statistics" = c("variance", "circular_spread", "spread_ratio", "angular_range"),
  "Causticness (Sector)" = c("angular_causticness", "tip_causticness", "cv_causticness",
                              "organization_causticness", "r1_concentration",
                              "r2_concentration", "joint_concentration"),
  "Causticness (Spatial)" = c("spatial_concentration", "fold_causticness"),
  "Local Coherence" = c("trajectory_smoothness", "curvature", "true_folds", "local_coherence"),
  "Other" = c("total_energy")
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

get_category <- function(metric) {
  for (cat_name in names(METRIC_CATEGORIES)) {
    if (metric %in% METRIC_CATEGORIES[[cat_name]]) {
      return(cat_name)
    }
  }
  return("Other")
}

# Theme for plots
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
# DATA LOADING AND PREPROCESSING
# =============================================================================

load_and_process_data <- function(csv_path) {
  cat("Loading data from:", csv_path, "\n")

  raw_data <- read.csv(csv_path, stringsAsFactors = FALSE)

  # Extract metric names and N values from column names
  col_names <- names(raw_data)

  # Find all unique metrics (columns ending in _N<digits>)
  metric_cols <- col_names[grepl("_N[0-9]+$", col_names)]
  metrics <- unique(gsub("_N[0-9]+$", "", metric_cols))

  # Find all N values
  n_matches <- regmatches(metric_cols, regexpr("N[0-9]+$", metric_cols))
  n_values <- unique(as.integer(gsub("N", "", n_matches)))
  n_values <- sort(n_values)

  cat("Found", length(metrics), "metrics\n")
  cat("Pendulum counts:", paste(n_values, collapse = ", "), "\n")
  cat("Frames:", nrow(raw_data), "\n")

  # Reshape data to long format manually (avoiding tidyr)
  long_list <- list()
  idx <- 1

  for (col in col_names) {
    if (col == "frame") next

    values <- raw_data[[col]]
    valid_idx <- !is.na(values)

    if (sum(valid_idx) == 0) next

    # Parse column name
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
        column = col,
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
    n_frames = nrow(raw_data)
  )
}

# =============================================================================
# ANALYSIS FUNCTIONS
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
    mutate(
      grade = get_grade(mean_cv),
      category = sapply(metric, get_category)
    ) %>%
    arrange(mean_cv)

  cv_data
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

plot_cv_timeseries <- function(data, metrics_to_plot = NULL) {
  cv_data <- data$long %>%
    filter(stat_type == "cv")

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
      title = "Coefficient of Variation Over Time",
      subtitle = "How stability changes throughout the simulation",
      x = "Time (seconds)",
      y = "CV (%)"
    ) +
    theme_stability()
}

plot_convergence <- function(data, metrics_to_plot = NULL) {
  value_data <- data$long %>%
    filter(stat_type == "value", !is.na(N))

  if (!is.null(metrics_to_plot)) {
    value_data <- value_data %>% filter(metric %in% metrics_to_plot)
  }

  if (nrow(value_data) == 0) return(NULL)

  avg_data <- value_data %>%
    group_by(metric, N) %>%
    summarize(
      mean_value = mean(value, na.rm = TRUE),
      .groups = "drop"
    )

  ggplot(avg_data, aes(x = N, y = mean_value, color = metric)) +
    geom_line(linewidth = 1) +
    geom_point(size = 2) +
    scale_x_log10(labels = comma) +
    facet_wrap(~metric, scales = "free_y", ncol = 3) +
    labs(
      title = "Metric Convergence by Pendulum Count",
      subtitle = "How metric values stabilize as N increases",
      x = "Pendulum Count (log scale)",
      y = "Mean Value"
    ) +
    theme_stability() +
    theme(legend.position = "none")
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

plot_category_comparison <- function(summary_data) {
  ggplot(summary_data, aes(x = category, y = mean_cv * 100, fill = category)) +
    geom_boxplot(alpha = 0.7) +
    geom_jitter(aes(color = grade), width = 0.2, size = 3) +
    scale_color_manual(values = GRADE_COLORS, name = "Grade") +
    scale_fill_viridis_d(guide = "none") +
    scale_y_log10(labels = function(x) paste0(x, "%")) +
    labs(
      title = "Stability by Metric Category",
      subtitle = "Comparing different types of metrics",
      x = NULL,
      y = "Mean CV (%, log scale)"
    ) +
    theme_stability() +
    theme(axis.text.x = element_text(angle = 30, hjust = 1))
}

plot_n_comparison <- function(data, metric_name) {
  value_data <- data$long %>%
    filter(stat_type == "value", !is.na(N), metric == metric_name)

  if (nrow(value_data) == 0) return(NULL)

  frame_duration <- 11.0 / data$n_frames
  value_data$time <- value_data$frame * frame_duration
  value_data$N_label <- factor(paste0("N=", format(value_data$N, big.mark = ",")))

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

# =============================================================================
# REPORT GENERATION
# =============================================================================

generate_report <- function(csv_path, output_dir = NULL) {
  if (is.null(output_dir)) {
    output_dir <- dirname(csv_path)
  }

  report_dir <- file.path(output_dir, "stability_report")
  dir.create(report_dir, showWarnings = FALSE, recursive = TRUE)

  cat("\n", paste(rep("=", 60), collapse = ""), "\n", sep = "")
  cat("METRIC STABILITY REPORT\n")
  cat(paste(rep("=", 60), collapse = ""), "\n\n", sep = "")

  # Load data
  data <- load_and_process_data(csv_path)

  # Compute summary
  summary_data <- compute_metric_summary(data)

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
  print(print_data[, c("metric", "mean_cv", "max_cv", "unstable_pct", "grade", "category")])

  # Generate plots
  cat("\n--- GENERATING PLOTS ---\n\n")

  # 1. Overview
  cat("1. Stability overview...\n")
  p1 <- plot_stability_overview(summary_data)
  ggsave(file.path(report_dir, "01_stability_overview.png"), p1,
         width = 10, height = 8, dpi = 150)

  # 2. Heatmap
  cat("2. Stability heatmap...\n")
  p2 <- plot_heatmap(data, summary_data)
  ggsave(file.path(report_dir, "02_stability_heatmap.png"), p2,
         width = 12, height = 8, dpi = 150)

  # 3. CV time series - stable metrics
  cat("3. CV time series (stable metrics)...\n")
  stable_metrics <- summary_data$metric[summary_data$mean_cv < 0.05]
  if (length(stable_metrics) > 0) {
    p3 <- plot_cv_timeseries(data, stable_metrics)
    if (!is.null(p3)) {
      ggsave(file.path(report_dir, "03_cv_timeseries_stable.png"), p3,
             width = 12, height = 6, dpi = 150)
    }
  }

  # 4. CV time series - unstable metrics
  cat("4. CV time series (unstable metrics)...\n")
  unstable_metrics <- summary_data$metric[summary_data$mean_cv >= 0.05 &
                                           summary_data$mean_cv < 1.0]
  if (length(unstable_metrics) > 0) {
    p4 <- plot_cv_timeseries(data, unstable_metrics)
    if (!is.null(p4)) {
      ggsave(file.path(report_dir, "04_cv_timeseries_unstable.png"), p4,
             width = 12, height = 6, dpi = 150)
    }
  }

  # 5. Convergence plots
  cat("5. Convergence analysis...\n")
  p5 <- plot_convergence(data, stable_metrics)
  if (!is.null(p5)) {
    ggsave(file.path(report_dir, "05_convergence_stable.png"), p5,
           width = 12, height = 8, dpi = 150)
  }

  # 6. Category comparison
  cat("6. Category comparison...\n")
  p6 <- plot_category_comparison(summary_data)
  ggsave(file.path(report_dir, "06_category_comparison.png"), p6,
         width = 10, height = 6, dpi = 150)

  # 7. Individual metric comparisons
  cat("7. Individual metric comparisons...\n")
  key_metrics <- c("organization_causticness", "variance", "circular_spread",
                   "trajectory_smoothness")
  for (m in key_metrics) {
    if (m %in% data$metrics) {
      p <- plot_n_comparison(data, m)
      if (!is.null(p)) {
        filename <- sprintf("07_comparison_%s.png", gsub("_", "-", m))
        ggsave(file.path(report_dir, filename), p,
               width = 10, height = 5, dpi = 150)
      }
    }
  }

  # Save summary table
  cat("8. Saving summary table...\n")
  write.csv(summary_data, file.path(report_dir, "stability_summary.csv"),
            row.names = FALSE)

  cat("\n", paste(rep("=", 60), collapse = ""), "\n", sep = "")
  cat("Report generated in:", report_dir, "\n")
  cat(paste(rep("=", 60), collapse = ""), "\n", sep = "")

  invisible(list(
    data = data,
    summary = summary_data,
    report_dir = report_dir
  ))
}

# =============================================================================
# MAIN
# =============================================================================

main <- function() {
  args <- commandArgs(trailingOnly = TRUE)

  if (length(args) < 1) {
    cat("Usage: Rscript stability_report.R <stability_data.csv> [output_dir]\n")
    cat("\nGenerates comprehensive stability analysis report.\n")
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
