"""Logging configuration using Loguru.

Provides daily-rotating log files at ~/.local/share/pendulum-tools/logs/
and stderr output for CLI visibility.
"""

from __future__ import annotations

import sys
from datetime import datetime
from pathlib import Path
from typing import Optional

from loguru import logger

from .constants import DEFAULT_LOG_DIR, LOG_DATE_FORMAT

# Remove default handler to avoid duplicate console output
logger.remove()

_configured = False


def setup_logging(verbose: bool = False, log_dir: Optional[Path] = None) -> None:
    """Configure Loguru logging.

    Sets up:
    - Daily rotating log file at ~/.local/share/pendulum-tools/logs/YYYY-MM-DD.log
    - stderr output with appropriate level

    Args:
        verbose: If True, show DEBUG level on stderr
        log_dir: Override log directory (default: ~/.local/share/pendulum-tools/logs/)
    """
    global _configured
    if _configured:
        return

    level = "DEBUG" if verbose else "INFO"
    log_path = log_dir or DEFAULT_LOG_DIR
    log_path.mkdir(parents=True, exist_ok=True)

    # Daily log file
    log_file = log_path / f"{datetime.now().strftime(LOG_DATE_FORMAT)}.log"

    # Add stderr handler (compact format for CLI)
    logger.add(
        sys.stderr,
        level=level,
        format="<level>{level: <8}</level> | <cyan>{extra[name]}</cyan> - <level>{message}</level>",
        colorize=True,
        filter=lambda record: "name" in record["extra"],
    )

    # Add file handler (detailed format for debugging)
    logger.add(
        log_file,
        level="DEBUG",
        format="{time:YYYY-MM-DD HH:mm:ss} | {level: <8} | {extra[name]} - {message}",
        rotation="00:00",  # New file at midnight
        retention="30 days",
        filter=lambda record: "name" in record["extra"],
    )

    _configured = True
    logger.bind(name="logging").info(f"Logging initialized: {log_file}")


def get_logger(name: str):
    """Get a logger bound to a specific name.

    Args:
        name: Logger name (typically module or component name)

    Returns:
        Logger instance bound to the given name
    """
    return logger.bind(name=name)
