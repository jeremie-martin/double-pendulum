"""Constants and default values for pendulum-tools.

Centralizes magic numbers and default values for easier configuration
and maintenance.
"""

from pathlib import Path

# =============================================================================
# Timing Constants
# =============================================================================

# Pre-boom offset for thumbnail extraction (seconds before boom)
PRE_BOOM_OFFSET_SECONDS = 0.5

# Fallback boom time when not detected (seconds)
# Used when metadata lacks boom detection
FALLBACK_BOOM_SECONDS = 10.0

# =============================================================================
# Video Processing Constants
# =============================================================================

# Blur effect settings
DEFAULT_BLUR_STRENGTH = 50
BLUR_SCALE_FACTOR = 4  # Scale down before blur for performance
MIN_BLUR_SIGMA = 5

# Background brightness for Shorts mode (0.0-1.0)
DEFAULT_BACKGROUND_BRIGHTNESS = 1.0

# =============================================================================
# Encoding Quality Constants
# =============================================================================

# libx264 CRF (0-51, lower = better, 17 = high quality for YouTube re-encoding)
DEFAULT_CRF_QUALITY = 17

# NVENC CQ (0-51, lower = better, 19 = high quality for YouTube re-encoding)
DEFAULT_NVENC_CQ = 19

# Encoding preset
DEFAULT_ENCODING_PRESET = "medium"

# =============================================================================
# Thumbnail Quality
# =============================================================================

# JPEG quality (1-31, lower = better)
DEFAULT_THUMBNAIL_QUALITY = 2

# =============================================================================
# Default Paths (relative to project root)
# =============================================================================

DEFAULT_MUSIC_DIR = "music"
DEFAULT_CREDENTIALS_DIR = "credentials"
DEFAULT_CONFIG_DIR = "config"

# User config file locations (in order of precedence)
USER_CONFIG_PATHS = [
    Path.home() / ".config" / "pendulum-tools" / "config.toml",
    Path.home() / ".pendulum-tools.toml",
]

# Environment variable names
ENV_MUSIC_DIR = "PENDULUM_MUSIC_DIR"
ENV_CREDENTIALS_DIR = "PENDULUM_CREDENTIALS_DIR"
ENV_CONFIG_DIR = "PENDULUM_CONFIG_DIR"

# =============================================================================
# File Size Validation
# =============================================================================

# Minimum expected output video size (bytes)
# Videos smaller than this are likely corrupted
MIN_OUTPUT_VIDEO_SIZE = 100_000  # 100KB

# =============================================================================
# Batch Processing
# =============================================================================

# Default log file name pattern
DEFAULT_LOG_FILE_PATTERN = "batch_{timestamp}.log"

# =============================================================================
# Logging (Loguru)
# =============================================================================

# Default log directory
DEFAULT_LOG_DIR = Path.home() / ".local" / "share" / "pendulum-tools" / "logs"

# Log file date format
LOG_DATE_FORMAT = "%Y-%m-%d"

# =============================================================================
# Watch Mode Constants
# =============================================================================

# Polling interval for new directories (seconds)
WATCH_POLL_INTERVAL = 5.0

# Minimum time to wait after detecting a new directory before processing
# (ensures video generation is complete)
WATCH_SETTLE_TIME = 10.0
