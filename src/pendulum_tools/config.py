"""User configuration file support for pendulum-tools.

Supports loading defaults from:
1. Environment variables (highest priority)
2. User config file (~/.config/pendulum-tools/config.toml)
3. Built-in defaults (lowest priority)

Example config file (~/.config/pendulum-tools/config.toml):

    [paths]
    music_dir = "/path/to/music"
    credentials_dir = "/path/to/credentials"

    [encoding]
    use_nvenc = true
    nvenc_cq = 19
    crf_quality = 17

    [processing]
    default_template = "minimal_science"
    shorts = true
    blur_bg = true
    force = true

    [youtube]
    playlist_id = "PLxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    delete_after_upload = false
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from .constants import (
    DEFAULT_CREDENTIALS_DIR,
    DEFAULT_CRF_QUALITY,
    DEFAULT_MUSIC_DIR,
    DEFAULT_NVENC_CQ,
    ENV_CREDENTIALS_DIR,
    ENV_MUSIC_DIR,
    USER_CONFIG_PATHS,
)


@dataclass
class ProcessingDefaults:
    """Default processing settings from config file.

    These can be overridden by CLI options.
    """

    shorts: bool = False
    blur_bg: bool = False
    force: bool = False

# Try to import tomllib (Python 3.11+) or tomli as fallback
try:
    import tomllib
except ImportError:
    try:
        import tomli as tomllib  # type: ignore
    except ImportError:
        tomllib = None  # type: ignore


@dataclass
class UserConfig:
    """User configuration loaded from config file and environment."""

    # Paths
    music_dir: Optional[Path] = None
    credentials_dir: Optional[Path] = None

    # Encoding
    use_nvenc: bool = True
    nvenc_cq: int = DEFAULT_NVENC_CQ
    crf_quality: int = DEFAULT_CRF_QUALITY

    # Processing
    default_template: str = "minimal_science"

    # Processing defaults (for auto/watch commands)
    processing: ProcessingDefaults = field(default_factory=ProcessingDefaults)

    # YouTube settings
    playlist_id: Optional[str] = None  # Playlist to add uploaded videos to
    delete_after_upload: bool = False  # Delete video directory after successful upload

    # Internal: track where config was loaded from
    _config_source: Optional[Path] = field(default=None, repr=False)

    @classmethod
    def load(cls) -> "UserConfig":
        """Load configuration from file and environment.

        Priority (highest to lowest):
        1. Environment variables
        2. User config file
        3. Built-in defaults
        """
        config = cls()

        # Try to load from config file
        config._load_from_file()

        # Override with environment variables
        config._load_from_env()

        return config

    def _load_from_file(self) -> None:
        """Load configuration from TOML file if it exists."""
        if tomllib is None:
            return

        for config_path in USER_CONFIG_PATHS:
            if config_path.exists():
                try:
                    with open(config_path, "rb") as f:
                        data = tomllib.load(f)
                    self._apply_config_data(data)
                    self._config_source = config_path
                    return
                except Exception:
                    # Silently ignore malformed config files
                    pass

    def _apply_config_data(self, data: dict[str, Any]) -> None:
        """Apply configuration data from parsed TOML."""
        # Paths section
        paths = data.get("paths", {})
        if "music_dir" in paths:
            self.music_dir = Path(paths["music_dir"])
        if "credentials_dir" in paths:
            self.credentials_dir = Path(paths["credentials_dir"])

        # Encoding section
        encoding = data.get("encoding", {})
        if "use_nvenc" in encoding:
            self.use_nvenc = bool(encoding["use_nvenc"])
        if "nvenc_cq" in encoding:
            self.nvenc_cq = int(encoding["nvenc_cq"])
        if "crf_quality" in encoding:
            self.crf_quality = int(encoding["crf_quality"])

        # Processing section
        processing = data.get("processing", {})
        if "default_template" in processing:
            self.default_template = str(processing["default_template"])
        if "shorts" in processing:
            self.processing.shorts = bool(processing["shorts"])
        if "blur_bg" in processing:
            self.processing.blur_bg = bool(processing["blur_bg"])
        if "force" in processing:
            self.processing.force = bool(processing["force"])

        # YouTube section
        youtube = data.get("youtube", {})
        if "playlist_id" in youtube:
            self.playlist_id = str(youtube["playlist_id"])
        if "delete_after_upload" in youtube:
            self.delete_after_upload = bool(youtube["delete_after_upload"])

    def _load_from_env(self) -> None:
        """Override configuration from environment variables."""
        if ENV_MUSIC_DIR in os.environ:
            self.music_dir = Path(os.environ[ENV_MUSIC_DIR])
        if ENV_CREDENTIALS_DIR in os.environ:
            self.credentials_dir = Path(os.environ[ENV_CREDENTIALS_DIR])

    def get_music_dir(self, cli_override: Optional[Path] = None) -> Path:
        """Get music directory with CLI override support.

        Priority: CLI > env/config > project-relative default
        """
        if cli_override is not None:
            return cli_override
        if self.music_dir is not None:
            return self.music_dir
        return _find_project_path(DEFAULT_MUSIC_DIR)

    def get_credentials_dir(self, cli_override: Optional[Path] = None) -> Path:
        """Get credentials directory with CLI override support.

        Priority: CLI > env/config > project-relative default
        """
        if cli_override is not None:
            return cli_override
        if self.credentials_dir is not None:
            return self.credentials_dir
        return _find_project_path(DEFAULT_CREDENTIALS_DIR)


def _find_project_path(relative_path: str) -> Path:
    """Find a path relative to the project root.

    Searches upward from CWD for the project root (contains pyproject.toml
    or .git), then returns the relative path from there.

    Falls back to CWD-relative if project root not found.
    """
    cwd = Path.cwd()

    # Search upward for project root indicators
    for parent in [cwd] + list(cwd.parents):
        if (parent / "pyproject.toml").exists() or (parent / ".git").exists():
            candidate = parent / relative_path
            if candidate.exists():
                return candidate

    # Fallback to CWD-relative
    return cwd / relative_path


# Global config instance (lazily loaded)
_config: Optional[UserConfig] = None


def get_config() -> UserConfig:
    """Get the global user configuration (loads on first access)."""
    global _config
    if _config is None:
        _config = UserConfig.load()
    return _config


def reset_config() -> None:
    """Reset the global config (useful for testing)."""
    global _config
    _config = None
