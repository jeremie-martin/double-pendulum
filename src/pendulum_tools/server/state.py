"""Thread-safe state management for the watcher server."""

from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum, auto
from pathlib import Path
from threading import Lock
from typing import Any, Optional


class WatcherStatus(Enum):
    """Overall watcher status."""

    STARTING = auto()
    RUNNING = auto()
    PAUSED = auto()
    AUTH_REQUIRED = auto()
    STOPPING = auto()
    STOPPED = auto()


class JobStatus(Enum):
    """Individual job status."""

    PENDING = auto()
    SETTLING = auto()
    PROCESSING = auto()
    ADDING_MUSIC = auto()
    UPLOADING = auto()
    WAITING_DELAY = auto()
    COMPLETED = auto()
    FAILED = auto()
    SKIPPED = auto()


class ErrorCategory(Enum):
    """Error categorization for retry logic."""

    TRANSIENT = auto()  # Rate limit, network - auto-retry eligible
    SYSTEMIC = auto()  # Auth, config - requires intervention
    CONTENT = auto()  # Bad video, missing metadata - skip and continue


@dataclass
class JobInfo:
    """Information about a single video job."""

    dir_name: str
    dir_path: Path
    status: JobStatus = JobStatus.PENDING
    first_seen: datetime = field(default_factory=datetime.now)
    started_at: Optional[datetime] = None
    completed_at: Optional[datetime] = None

    # Progress tracking
    progress_message: str = ""
    progress_percent: float = 0.0

    # Results
    video_id: Optional[str] = None
    video_url: Optional[str] = None
    error: Optional[str] = None
    error_category: Optional[ErrorCategory] = None

    # Metadata (populated after detection)
    boom_seconds: Optional[float] = None
    music_track: Optional[str] = None
    template_used: Optional[str] = None

    def to_dict(self) -> dict[str, Any]:
        """Convert to dictionary for UI serialization."""
        return {
            "dir_name": self.dir_name,
            "dir_path": str(self.dir_path),
            "status": self.status.name,
            "first_seen": self.first_seen.isoformat(),
            "started_at": self.started_at.isoformat() if self.started_at else None,
            "completed_at": self.completed_at.isoformat() if self.completed_at else None,
            "progress_message": self.progress_message,
            "progress_percent": self.progress_percent,
            "video_id": self.video_id,
            "video_url": self.video_url,
            "error": self.error,
            "error_category": self.error_category.name if self.error_category else None,
            "boom_seconds": self.boom_seconds,
            "music_track": self.music_track,
            "template_used": self.template_used,
        }


@dataclass
class RuntimeSettings:
    """Runtime-configurable settings."""

    upload_delay: float = 60.0
    poll_interval: float = 5.0
    settle_time: float = 10.0
    privacy: str = "private"
    playlist_id: Optional[str] = None
    delete_after_upload: bool = False


class WatcherState:
    """Thread-safe watcher state container.

    This class manages all state shared between the watcher thread and the UI.
    All public methods are thread-safe.
    """

    def __init__(
        self,
        batch_dir: Optional[Path] = None,
        history_limit: int = 100,
        failed_limit: int = 50,
    ):
        self._lock = Lock()

        # Core state
        self._status = WatcherStatus.STOPPED
        self._batch_dir = batch_dir

        # Queue management
        self._pending_queue: deque[JobInfo] = deque()
        self._current_job: Optional[JobInfo] = None

        # History (limited size)
        self._completed_history: deque[JobInfo] = deque(maxlen=history_limit)
        self._failed_history: deque[JobInfo] = deque(maxlen=failed_limit)

        # Statistics
        self._total_processed = 0
        self._total_failed = 0
        self._total_skipped = 0
        self._session_start: Optional[datetime] = None

        # Runtime settings
        self._settings = RuntimeSettings()

        # Control flags
        self._paused = False

        # Error state
        self._auth_error: Optional[str] = None
        self._last_error: Optional[str] = None

    # --- Status properties ---

    @property
    def status(self) -> WatcherStatus:
        with self._lock:
            return self._status

    @status.setter
    def status(self, value: WatcherStatus) -> None:
        with self._lock:
            self._status = value

    @property
    def batch_dir(self) -> Optional[Path]:
        with self._lock:
            return self._batch_dir

    @property
    def paused(self) -> bool:
        with self._lock:
            return self._paused

    @paused.setter
    def paused(self, value: bool) -> None:
        with self._lock:
            self._paused = value
            if value:
                self._status = WatcherStatus.PAUSED
            elif self._auth_error:
                self._status = WatcherStatus.AUTH_REQUIRED
            else:
                self._status = WatcherStatus.RUNNING

    @property
    def auth_error(self) -> Optional[str]:
        with self._lock:
            return self._auth_error

    @property
    def settings(self) -> RuntimeSettings:
        with self._lock:
            return RuntimeSettings(
                upload_delay=self._settings.upload_delay,
                poll_interval=self._settings.poll_interval,
                settle_time=self._settings.settle_time,
                privacy=self._settings.privacy,
                playlist_id=self._settings.playlist_id,
                delete_after_upload=self._settings.delete_after_upload,
            )

    # --- Queue operations ---

    def add_pending(self, job: JobInfo) -> None:
        """Add a job to the pending queue."""
        with self._lock:
            # Check if already in queue
            for existing in self._pending_queue:
                if existing.dir_name == job.dir_name:
                    return
            self._pending_queue.append(job)

    def get_next_pending(self) -> Optional[JobInfo]:
        """Get and remove the next pending job."""
        with self._lock:
            if self._pending_queue:
                return self._pending_queue.popleft()
            return None

    def peek_pending(self) -> list[JobInfo]:
        """Get a copy of the pending queue."""
        with self._lock:
            return list(self._pending_queue)

    def pending_count(self) -> int:
        """Get the number of pending jobs."""
        with self._lock:
            return len(self._pending_queue)

    def set_current_job(self, job: Optional[JobInfo]) -> None:
        """Set the current job being processed."""
        with self._lock:
            self._current_job = job

    def get_current_job(self) -> Optional[JobInfo]:
        """Get the current job."""
        with self._lock:
            return self._current_job

    def update_current_progress(
        self,
        status: Optional[JobStatus] = None,
        message: Optional[str] = None,
        percent: Optional[float] = None,
    ) -> None:
        """Update the current job's progress."""
        with self._lock:
            if self._current_job:
                if status is not None:
                    self._current_job.status = status
                if message is not None:
                    self._current_job.progress_message = message
                if percent is not None:
                    self._current_job.progress_percent = percent

    # --- Completion operations ---

    def complete_current(
        self,
        video_id: Optional[str] = None,
        video_url: Optional[str] = None,
        music_track: Optional[str] = None,
        template_used: Optional[str] = None,
    ) -> None:
        """Mark current job as completed and move to history."""
        with self._lock:
            if self._current_job:
                self._current_job.status = JobStatus.COMPLETED
                self._current_job.completed_at = datetime.now()
                self._current_job.video_id = video_id
                self._current_job.video_url = video_url
                self._current_job.music_track = music_track
                self._current_job.template_used = template_used
                self._completed_history.appendleft(self._current_job)
                self._total_processed += 1
                self._current_job = None

    def fail_current(
        self,
        error: str,
        category: ErrorCategory = ErrorCategory.CONTENT,
    ) -> None:
        """Mark current job as failed and move to failed history."""
        with self._lock:
            if self._current_job:
                self._current_job.status = JobStatus.FAILED
                self._current_job.completed_at = datetime.now()
                self._current_job.error = error
                self._current_job.error_category = category
                self._failed_history.appendleft(self._current_job)
                self._total_failed += 1
                self._current_job = None

    def skip_current(self, reason: str) -> None:
        """Mark current job as skipped and move to completed history."""
        with self._lock:
            if self._current_job:
                self._current_job.status = JobStatus.SKIPPED
                self._current_job.completed_at = datetime.now()
                self._current_job.error = reason
                self._completed_history.appendleft(self._current_job)
                self._total_skipped += 1
                self._current_job = None

    # --- Error handling ---

    def set_auth_error(self, error: str) -> None:
        """Set auth error and pause processing."""
        with self._lock:
            self._auth_error = error
            self._paused = True
            self._status = WatcherStatus.AUTH_REQUIRED

    def clear_auth_error(self) -> None:
        """Clear auth error and resume processing."""
        with self._lock:
            self._auth_error = None
            self._paused = False
            self._status = WatcherStatus.RUNNING

    # --- Failed job management ---

    def get_failed_jobs(self) -> list[JobInfo]:
        """Get a copy of the failed jobs."""
        with self._lock:
            return list(self._failed_history)

    def retry_failed(self, dir_name: str) -> bool:
        """Move a failed job back to pending queue.

        Returns True if the job was found and moved.
        """
        with self._lock:
            for i, job in enumerate(self._failed_history):
                if job.dir_name == dir_name:
                    # Reset job state
                    job.status = JobStatus.PENDING
                    job.error = None
                    job.error_category = None
                    job.completed_at = None
                    job.progress_message = ""
                    job.progress_percent = 0.0
                    # Move to pending
                    del self._failed_history[i]
                    self._pending_queue.append(job)
                    self._total_failed -= 1
                    return True
            return False

    def clear_failed(self, dir_name: str) -> bool:
        """Remove a job from failed history.

        Returns True if the job was found and removed.
        """
        with self._lock:
            for i, job in enumerate(self._failed_history):
                if job.dir_name == dir_name:
                    del self._failed_history[i]
                    return True
            return False

    def clear_all_failed(self) -> int:
        """Clear all failed jobs. Returns the count of cleared jobs."""
        with self._lock:
            count = len(self._failed_history)
            self._failed_history.clear()
            return count

    # --- History access ---

    def get_completed_history(self, limit: int = 20) -> list[JobInfo]:
        """Get recent completed jobs."""
        with self._lock:
            return list(self._completed_history)[:limit]

    # --- Settings management ---

    def update_settings(self, **kwargs: Any) -> None:
        """Update runtime settings."""
        with self._lock:
            for key, value in kwargs.items():
                if hasattr(self._settings, key):
                    setattr(self._settings, key, value)

    # --- Session management ---

    def start_session(self, batch_dir: Path) -> None:
        """Start a new session."""
        with self._lock:
            self._batch_dir = batch_dir
            self._session_start = datetime.now()
            self._status = WatcherStatus.STARTING
            self._total_processed = 0
            self._total_failed = 0
            self._total_skipped = 0
            self._pending_queue.clear()
            self._current_job = None
            self._auth_error = None

    def stop_session(self) -> None:
        """Stop the current session."""
        with self._lock:
            self._status = WatcherStatus.STOPPED

    # --- Snapshot for UI ---

    def snapshot(self) -> dict[str, Any]:
        """Get a thread-safe snapshot of state for UI rendering."""
        with self._lock:
            return {
                "status": self._status.name,
                "batch_dir": str(self._batch_dir) if self._batch_dir else None,
                "paused": self._paused,
                "auth_error": self._auth_error,
                "pending_count": len(self._pending_queue),
                "pending_queue": [j.to_dict() for j in self._pending_queue],
                "current_job": self._current_job.to_dict() if self._current_job else None,
                "completed_history": [j.to_dict() for j in list(self._completed_history)[:20]],
                "failed_history": [j.to_dict() for j in self._failed_history],
                "total_processed": self._total_processed,
                "total_failed": self._total_failed,
                "total_skipped": self._total_skipped,
                "session_start": self._session_start.isoformat() if self._session_start else None,
                "settings": {
                    "upload_delay": self._settings.upload_delay,
                    "poll_interval": self._settings.poll_interval,
                    "settle_time": self._settings.settle_time,
                    "privacy": self._settings.privacy,
                    "playlist_id": self._settings.playlist_id,
                    "delete_after_upload": self._settings.delete_after_upload,
                },
            }
