"""Custom exceptions for pendulum-tools with detailed error information."""

from __future__ import annotations

from pathlib import Path
from typing import Optional


class PendulumToolsError(Exception):
    """Base exception for pendulum-tools errors."""

    pass


class FFmpegError(PendulumToolsError):
    """Exception for FFmpeg-related errors with detailed output capture."""

    def __init__(
        self,
        message: str,
        command: Optional[list[str]] = None,
        returncode: Optional[int] = None,
        stdout: Optional[str] = None,
        stderr: Optional[str] = None,
    ):
        self.command = command
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr

        # Build detailed error message
        parts = [message]

        if returncode is not None:
            parts.append(f"Exit code: {returncode}")

        if stderr:
            # Extract the most relevant error lines from FFmpeg stderr
            error_lines = self._extract_error_lines(stderr)
            if error_lines:
                parts.append(f"FFmpeg error: {error_lines}")

        super().__init__("\n".join(parts))

    @staticmethod
    def _extract_error_lines(stderr: str) -> str:
        """Extract the most relevant error lines from FFmpeg stderr.

        FFmpeg outputs a lot of verbose info. This extracts just the error.
        """
        lines = stderr.strip().split("\n")

        # Look for lines containing common error indicators
        error_indicators = [
            "Error",
            "error",
            "Invalid",
            "invalid",
            "No such file",
            "not found",
            "Unable to",
            "Cannot",
            "failed",
            "Discarding",
        ]

        error_lines = []
        for line in lines:
            if any(indicator in line for indicator in error_indicators):
                # Clean up the line
                line = line.strip()
                if line and line not in error_lines:
                    error_lines.append(line)

        # If no specific errors found, return last few lines
        if not error_lines and lines:
            error_lines = [l.strip() for l in lines[-3:] if l.strip()]

        return " | ".join(error_lines[:3])  # Limit to 3 most relevant

    @property
    def command_string(self) -> str:
        """Get the command as a string for display."""
        if self.command:
            import shlex
            return shlex.join(self.command)
        return ""


class VideoValidationError(PendulumToolsError):
    """Exception for video validation failures."""

    def __init__(
        self,
        message: str,
        video_path: Optional[Path] = None,
        expected: Optional[str] = None,
        actual: Optional[str] = None,
    ):
        self.video_path = video_path
        self.expected = expected
        self.actual = actual

        parts = [message]
        if video_path:
            parts.append(f"Video: {video_path}")
        if expected and actual:
            parts.append(f"Expected: {expected}, Got: {actual}")

        super().__init__(" | ".join(parts))


class MetadataError(PendulumToolsError):
    """Exception for metadata-related errors."""

    def __init__(
        self,
        message: str,
        metadata_path: Optional[Path] = None,
        missing_fields: Optional[list[str]] = None,
    ):
        self.metadata_path = metadata_path
        self.missing_fields = missing_fields

        parts = [message]
        if metadata_path:
            parts.append(f"File: {metadata_path}")
        if missing_fields:
            parts.append(f"Missing fields: {', '.join(missing_fields)}")

        super().__init__(" | ".join(parts))


class MusicSyncError(PendulumToolsError):
    """Exception for music synchronization issues."""

    def __init__(
        self,
        message: str,
        boom_seconds: Optional[float] = None,
        available_tracks: int = 0,
    ):
        self.boom_seconds = boom_seconds
        self.available_tracks = available_tracks

        parts = [message]
        if boom_seconds is not None:
            parts.append(f"Boom at: {boom_seconds:.2f}s")
        parts.append(f"Available tracks: {available_tracks}")

        super().__init__(" | ".join(parts))


class UploadError(PendulumToolsError):
    """Exception for YouTube upload failures."""

    def __init__(
        self,
        message: str,
        video_path: Optional[Path] = None,
        is_rate_limit: bool = False,
        retry_after: Optional[int] = None,
    ):
        self.video_path = video_path
        self.is_rate_limit = is_rate_limit
        self.retry_after = retry_after

        parts = [message]
        if video_path:
            parts.append(f"Video: {video_path}")
        if is_rate_limit:
            parts.append("Rate limit exceeded")
        if retry_after:
            parts.append(f"Retry after: {retry_after}s")

        super().__init__(" | ".join(parts))


class RateLimitError(UploadError):
    """Specific exception for YouTube API rate limit errors."""

    def __init__(
        self,
        video_path: Optional[Path] = None,
        retry_after: Optional[int] = None,
    ):
        super().__init__(
            "YouTube API rate limit exceeded",
            video_path=video_path,
            is_rate_limit=True,
            retry_after=retry_after,
        )
