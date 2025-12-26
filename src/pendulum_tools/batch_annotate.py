#!/usr/bin/env python3
"""
Batch video annotation and processing GUI.

Simple tool for mass production workflow:
- Browse batch directories
- Edit boom_frame with autosave
- Add music with track selection
- Process videos with effects
- Delete unwanted simulations

Entry point: batch-annotate
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from PyQt6.QtCore import Qt, QSettings, QTimer
from PyQt6.QtGui import QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from .config import get_config
from .models import VideoMetadata
from .music import MusicDatabase, MusicManager, MusicTrack


MPV_COMMAND = "mpv"


def get_best_video_path(video_dir: Path) -> Optional[Path]:
    """Get the most processed video available.

    Priority: video_processed_final.mp4 > video_processed.mp4 > video.mp4 > video_raw.mp4
    """
    candidates = [
        "video_processed_final.mp4",
        "video_processed.mp4",
        "video.mp4",
        "video_raw.mp4",
    ]
    for name in candidates:
        path = video_dir / name
        if path.exists():
            return path
    return None


@dataclass
class VideoInfo:
    """Information about a video in a batch."""
    name: str
    path: Path
    metadata_path: Path
    has_metadata: bool
    has_video: bool
    boom_frame: Optional[int]
    boom_seconds: Optional[float]
    video_fps: int
    duration_seconds: float
    best_video_path: Optional[Path]

    # Processing state
    has_processed: bool = False
    has_music: bool = False


def load_video_info(video_dir: Path) -> Optional[VideoInfo]:
    """Load video info from a video directory."""
    if not video_dir.is_dir():
        return None

    name = video_dir.name
    if not name.startswith("video_"):
        return None

    metadata_path = video_dir / "metadata.json"
    has_metadata = metadata_path.exists()
    has_video = (video_dir / "video_raw.mp4").exists()
    best_video = get_best_video_path(video_dir)

    # Check processing state
    has_processed = (video_dir / "video_processed.mp4").exists()
    has_music = (video_dir / "video_processed_final.mp4").exists() or (video_dir / "video.mp4").exists()

    boom_frame = None
    boom_seconds = None
    video_fps = 60
    duration_seconds = 30.0

    if has_metadata:
        try:
            data = json.loads(metadata_path.read_text())
            results = data.get("results", {})
            boom_frame = results.get("boom_frame")
            boom_seconds = results.get("boom_seconds")

            output = data.get("output", {})
            video_fps = output.get("video_fps", 60)
            duration_seconds = output.get("video_duration", 30.0)
        except Exception:
            pass

    return VideoInfo(
        name=name,
        path=video_dir,
        metadata_path=metadata_path,
        has_metadata=has_metadata,
        has_video=has_video,
        boom_frame=boom_frame,
        boom_seconds=boom_seconds,
        video_fps=video_fps,
        duration_seconds=duration_seconds,
        best_video_path=best_video,
        has_processed=has_processed,
        has_music=has_music,
    )


def scan_batch(batch_dir: Path) -> list[VideoInfo]:
    """Scan a batch directory for videos."""
    videos = []

    for item in sorted(batch_dir.iterdir()):
        if item.is_dir() and item.name.startswith("video_"):
            info = load_video_info(item)
            if info:
                videos.append(info)

    return videos


class MainWindow(QMainWindow):
    """Main window for batch annotation."""

    def __init__(self, batch_dir: Optional[Path] = None):
        super().__init__()
        self.setWindowTitle("Batch Video Annotator")
        self.resize(1000, 700)

        self.settings = QSettings("double-pendulum", "batch-annotate")
        self.batch_dir: Optional[Path] = None
        self.videos: list[VideoInfo] = []
        self.current_video: Optional[VideoInfo] = None
        self.music_db: Optional[MusicDatabase] = None
        self.valid_tracks: list[MusicTrack] = []

        # Autosave timer (debounce rapid changes)
        self.autosave_timer = QTimer()
        self.autosave_timer.setSingleShot(True)
        self.autosave_timer.timeout.connect(self._do_autosave)

        self._init_ui()
        self._setup_shortcuts()
        self._load_music_database()

        if batch_dir:
            self._load_batch(batch_dir)
        else:
            last_batch = self.settings.value("last_batch", "", type=str)
            if last_batch and Path(last_batch).exists():
                self._load_batch(Path(last_batch))

    def _load_music_database(self) -> None:
        """Load the music database."""
        try:
            config = get_config()
            music_dir = config.get_music_dir(None)
            self.music_db = MusicDatabase(music_dir)
        except Exception as e:
            print(f"Warning: Could not load music database: {e}")
            self.music_db = None

    def _init_ui(self) -> None:
        """Initialize the UI."""
        central = QWidget()
        self.setCentralWidget(central)

        layout = QHBoxLayout(central)

        # Left side: video list
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)

        # Batch controls
        batch_row = QHBoxLayout()
        self.open_btn = QPushButton("Open Batch...")
        self.open_btn.clicked.connect(self._open_batch_dialog)
        batch_row.addWidget(self.open_btn)

        self.batch_label = QLabel("No batch loaded")
        self.batch_label.setStyleSheet("color: gray;")
        batch_row.addWidget(self.batch_label, 1)
        left_layout.addLayout(batch_row)

        # Video list
        self.video_list = QListWidget()
        self.video_list.currentItemChanged.connect(self._on_video_selected)
        left_layout.addWidget(self.video_list)

        # Navigation
        nav_row = QHBoxLayout()
        self.prev_btn = QPushButton("< Prev (P)")
        self.prev_btn.clicked.connect(self._prev_video)
        nav_row.addWidget(self.prev_btn)

        self.next_btn = QPushButton("Next (N) >")
        self.next_btn.clicked.connect(self._next_video)
        nav_row.addWidget(self.next_btn)
        left_layout.addLayout(nav_row)

        # Right side: video details and controls
        right = QWidget()
        right_layout = QVBoxLayout(right)

        # Video info
        self.info_label = QLabel("Select a video")
        self.info_label.setWordWrap(True)
        self.info_label.setStyleSheet("font-size: 14px; padding: 10px; background: #f0f0f0; border-radius: 5px;")
        right_layout.addWidget(self.info_label)

        # Boom frame editor (with autosave)
        boom_row = QHBoxLayout()
        boom_row.addWidget(QLabel("Boom Frame:"))

        self.boom_spin = QSpinBox()
        self.boom_spin.setRange(0, 100000)
        self.boom_spin.setEnabled(False)
        self.boom_spin.valueChanged.connect(self._on_boom_changed)
        boom_row.addWidget(self.boom_spin)

        self.boom_seconds_label = QLabel("(0.0s)")
        boom_row.addWidget(self.boom_seconds_label)

        self.autosave_label = QLabel("")
        self.autosave_label.setStyleSheet("color: green; font-size: 12px;")
        boom_row.addWidget(self.autosave_label)

        boom_row.addStretch()
        right_layout.addLayout(boom_row)

        # Play buttons
        play_row = QHBoxLayout()

        self.play_btn = QPushButton("Play Video (O)")
        self.play_btn.setEnabled(False)
        self.play_btn.clicked.connect(self._play_video)
        play_row.addWidget(self.play_btn)

        self.play_boom_btn = QPushButton("Play at Boom")
        self.play_boom_btn.setEnabled(False)
        self.play_boom_btn.clicked.connect(self._play_at_boom)
        play_row.addWidget(self.play_boom_btn)

        right_layout.addLayout(play_row)

        # Music section
        right_layout.addWidget(QLabel("Music:"))

        music_row = QHBoxLayout()
        self.music_combo = QComboBox()
        self.music_combo.setEnabled(False)
        self.music_combo.setMinimumWidth(200)
        music_row.addWidget(self.music_combo, 1)

        self.add_music_btn = QPushButton("Add Music")
        self.add_music_btn.setEnabled(False)
        self.add_music_btn.clicked.connect(self._add_music)
        music_row.addWidget(self.add_music_btn)

        right_layout.addLayout(music_row)

        # Processing buttons
        right_layout.addWidget(QLabel("Processing:"))

        process_row = QHBoxLayout()

        self.process_btn = QPushButton("Process Video")
        self.process_btn.setEnabled(False)
        self.process_btn.clicked.connect(self._process_video)
        self.process_btn.setStyleSheet("background: #2196F3; color: white; padding: 8px;")
        process_row.addWidget(self.process_btn)

        self.process_music_btn = QPushButton("Process + Music")
        self.process_music_btn.setEnabled(False)
        self.process_music_btn.clicked.connect(self._process_with_music)
        self.process_music_btn.setStyleSheet("background: #4CAF50; color: white; padding: 8px;")
        process_row.addWidget(self.process_music_btn)

        right_layout.addLayout(process_row)

        # Upload button
        self.upload_btn = QPushButton("Upload to YouTube")
        self.upload_btn.setEnabled(False)
        self.upload_btn.clicked.connect(self._upload_video)
        self.upload_btn.setStyleSheet("background: #FF0000; color: white; padding: 8px;")
        right_layout.addWidget(self.upload_btn)

        # Delete button
        self.delete_btn = QPushButton("Delete Video")
        self.delete_btn.setEnabled(False)
        self.delete_btn.clicked.connect(self._delete_video)
        self.delete_btn.setStyleSheet("background: #f44336; color: white; padding: 8px; margin-top: 20px;")
        right_layout.addWidget(self.delete_btn)

        right_layout.addStretch()

        # Splitter
        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(left)
        splitter.addWidget(right)
        splitter.setStretchFactor(0, 1)
        splitter.setStretchFactor(1, 2)

        layout.addWidget(splitter)

    def _setup_shortcuts(self) -> None:
        """Set up keyboard shortcuts."""
        QShortcut(QKeySequence("N"), self).activated.connect(self._next_video)
        QShortcut(QKeySequence("P"), self).activated.connect(self._prev_video)
        QShortcut(QKeySequence("O"), self).activated.connect(self._play_video)
        QShortcut(QKeySequence("Ctrl+O"), self).activated.connect(self._open_batch_dialog)

    def _open_batch_dialog(self) -> None:
        """Open a file dialog to select a batch directory."""
        start_dir = str(self.batch_dir) if self.batch_dir else str(Path.cwd() / "batch_output")

        dir_path = QFileDialog.getExistingDirectory(
            self,
            "Select Batch Directory",
            start_dir,
        )

        if dir_path:
            self._load_batch(Path(dir_path))

    def _load_batch(self, batch_dir: Path) -> None:
        """Load a batch directory."""
        self.batch_dir = batch_dir
        self.videos = scan_batch(batch_dir)

        self.settings.setValue("last_batch", str(batch_dir))

        self.batch_label.setText(batch_dir.name)
        self.batch_label.setStyleSheet("")
        self.setWindowTitle(f"Batch Annotator - {batch_dir.name}")

        # Populate list
        self.video_list.clear()
        for video in self.videos:
            status = self._get_video_status(video)
            item = QListWidgetItem(f"[{status}] {video.name}")
            item.setData(Qt.ItemDataRole.UserRole, video)
            self.video_list.addItem(item)

        if self.videos:
            self.video_list.setCurrentRow(0)

    def _get_video_status(self, video: VideoInfo) -> str:
        """Get status indicator for video."""
        if video.boom_frame is None:
            return "?"  # No boom set
        elif video.has_music:
            return "M"  # Has music
        elif video.has_processed:
            return "P"  # Processed
        else:
            return "+"  # Has boom set

    def _on_video_selected(self, current: Optional[QListWidgetItem], _previous) -> None:
        """Handle video selection."""
        if not current:
            self.current_video = None
            self._update_ui()
            return

        self.current_video = current.data(Qt.ItemDataRole.UserRole)
        self._update_ui()
        self._update_music_combo()

    def _update_ui(self) -> None:
        """Update UI based on current video."""
        video = self.current_video
        self.autosave_label.setText("")

        if not video:
            self.info_label.setText("Select a video")
            self.boom_spin.setEnabled(False)
            self.play_btn.setEnabled(False)
            self.play_boom_btn.setEnabled(False)
            self.music_combo.setEnabled(False)
            self.add_music_btn.setEnabled(False)
            self.process_btn.setEnabled(False)
            self.process_music_btn.setEnabled(False)
            self.upload_btn.setEnabled(False)
            self.delete_btn.setEnabled(False)
            return

        # Update info
        boom_str = f"{video.boom_seconds:.2f}s" if video.boom_seconds else "N/A"
        video_file = video.best_video_path.name if video.best_video_path else "None"

        info_text = f"""<b>{video.name}</b><br>
FPS: {video.video_fps} | Duration: {video.duration_seconds:.1f}s<br>
Video: {video_file}<br>
Boom Frame: {video.boom_frame if video.boom_frame else 'Not set'}<br>
Boom Time: {boom_str}<br>
Processed: {'Yes' if video.has_processed else 'No'} | Music: {'Yes' if video.has_music else 'No'}"""

        self.info_label.setText(info_text)

        # Update boom spinner
        self.boom_spin.blockSignals(True)
        self.boom_spin.setEnabled(True)
        self.boom_spin.setMaximum(int(video.duration_seconds * video.video_fps))
        self.boom_spin.setValue(video.boom_frame or 0)
        self.boom_spin.blockSignals(False)

        self._update_boom_label()

        # Enable buttons
        has_boom = video.boom_frame is not None and video.boom_frame > 0
        self.play_btn.setEnabled(video.best_video_path is not None)
        self.play_boom_btn.setEnabled(video.best_video_path is not None and has_boom)
        self.music_combo.setEnabled(has_boom and self.music_db is not None)
        self.add_music_btn.setEnabled(has_boom and self.music_db is not None)
        self.process_btn.setEnabled(video.has_video and has_boom)
        self.process_music_btn.setEnabled(video.has_video and has_boom and self.music_db is not None)
        self.upload_btn.setEnabled(video.best_video_path is not None)
        self.delete_btn.setEnabled(True)

    def _update_boom_label(self) -> None:
        """Update the boom seconds label."""
        if not self.current_video:
            return

        frame = self.boom_spin.value()
        seconds = frame / self.current_video.video_fps
        self.boom_seconds_label.setText(f"({seconds:.2f}s)")

    def _update_music_combo(self) -> None:
        """Update music dropdown with valid tracks."""
        self.music_combo.clear()
        self.valid_tracks = []

        if not self.music_db or not self.current_video:
            return

        boom_frame = self.boom_spin.value()
        if boom_frame <= 0:
            return

        boom_seconds = boom_frame / self.current_video.video_fps

        # Get valid tracks (drop time > boom time)
        self.valid_tracks = self.music_db.get_valid_tracks_for_boom(boom_seconds)

        if not self.valid_tracks:
            self.music_combo.addItem("No valid tracks (boom too late)")
            self.music_combo.setEnabled(False)
            self.add_music_btn.setEnabled(False)
            return

        # Add random option first
        self.music_combo.addItem(f"Random ({len(self.valid_tracks)} available)")

        # Add individual tracks
        for track in self.valid_tracks:
            self.music_combo.addItem(f"{track.title} (drop: {track.drop_time_seconds:.1f}s)")

    def _on_boom_changed(self, value: int) -> None:
        """Handle boom frame change - trigger autosave."""
        self._update_boom_label()
        self._update_music_combo()

        if self.current_video and self.current_video.has_metadata:
            # Debounce: wait 500ms after last change before saving
            self.autosave_timer.stop()
            self.autosave_timer.start(500)
            self.autosave_label.setText("...")

    def _do_autosave(self) -> None:
        """Perform the actual autosave."""
        if not self.current_video or not self.current_video.has_metadata:
            return

        try:
            data = json.loads(self.current_video.metadata_path.read_text())

            new_boom_frame = self.boom_spin.value()
            if "results" not in data:
                data["results"] = {}

            data["results"]["boom_frame"] = new_boom_frame
            data["results"]["boom_seconds"] = new_boom_frame / self.current_video.video_fps

            self.current_video.metadata_path.write_text(
                json.dumps(data, indent=2) + "\n"
            )

            self.current_video.boom_frame = new_boom_frame
            self.current_video.boom_seconds = new_boom_frame / self.current_video.video_fps

            # Update list item
            row = self.video_list.currentRow()
            if row >= 0:
                item = self.video_list.item(row)
                status = self._get_video_status(self.current_video)
                item.setText(f"[{status}] {self.current_video.name}")

            self.autosave_label.setText("Saved")
            QTimer.singleShot(2000, lambda: self.autosave_label.setText(""))

        except Exception as e:
            self.autosave_label.setText(f"Error: {e}")

    def _prev_video(self) -> None:
        """Select previous video."""
        row = self.video_list.currentRow()
        if row > 0:
            self.video_list.setCurrentRow(row - 1)

    def _next_video(self) -> None:
        """Select next video."""
        row = self.video_list.currentRow()
        if row < self.video_list.count() - 1:
            self.video_list.setCurrentRow(row + 1)

    def _play_video(self) -> None:
        """Play the best available video."""
        if not self.current_video or not self.current_video.best_video_path:
            return

        if shutil.which(MPV_COMMAND):
            subprocess.Popen([MPV_COMMAND, str(self.current_video.best_video_path)])
        else:
            QMessageBox.warning(self, "mpv not found", "mpv is not installed or not in PATH")

    def _play_at_boom(self) -> None:
        """Play video starting before boom frame."""
        if not self.current_video or not self.current_video.best_video_path:
            return

        frame = self.boom_spin.value()
        if frame <= 0:
            self._play_video()
            return

        seconds = frame / self.current_video.video_fps
        start_seconds = max(0, seconds - 3)

        if shutil.which(MPV_COMMAND):
            subprocess.Popen([MPV_COMMAND, f"--start={start_seconds:.2f}", str(self.current_video.best_video_path)])
        else:
            QMessageBox.warning(self, "mpv not found", "mpv is not installed or not in PATH")

    def _get_selected_track_id(self) -> Optional[str]:
        """Get the track ID from the combo selection, or None for random."""
        idx = self.music_combo.currentIndex()
        if idx <= 0 or idx > len(self.valid_tracks):
            return None  # Random
        return self.valid_tracks[idx - 1].id

    def _add_music(self) -> None:
        """Add music to the current video."""
        if not self.current_video:
            return

        track_id = self._get_selected_track_id()
        self._run_cli_command("music", "add", track_id=track_id)

    def _process_video(self) -> None:
        """Process the current video with effects."""
        if not self.current_video:
            return
        self._run_cli_command("process")

    def _process_with_music(self) -> None:
        """Process video and add music."""
        if not self.current_video:
            return

        track_id = self._get_selected_track_id()
        self._run_cli_command("process", music=True, track_id=track_id)

    def _upload_video(self) -> None:
        """Upload the current video to YouTube."""
        if not self.current_video:
            return
        self._run_cli_command("upload")

    def _run_cli_command(self, command: str, subcommand: str = "", music: bool = False, track_id: Optional[str] = None) -> None:
        """Run a pendulum-tools CLI command."""
        if not self.current_video:
            return

        video_dir = str(self.current_video.path)

        # Build command
        if command == "music":
            cmd = ["uv", "run", "pendulum-tools", "music", subcommand, video_dir]
            if track_id:
                cmd.extend(["--track", track_id])
        elif command == "process":
            cmd = ["uv", "run", "pendulum-tools", "process", video_dir, "--shorts", "--blur-bg", "--force"]
            if music:
                cmd.append("--music")
                if track_id:
                    cmd.extend(["--track", track_id])
        elif command == "upload":
            cmd = ["uv", "run", "pendulum-tools", "upload", video_dir, "--privacy", "public"]
        else:
            return

        # Run in background and show result
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, cwd=str(Path.cwd()))

            if result.returncode == 0:
                QMessageBox.information(self, "Success", f"Command completed successfully:\n{command} {subcommand}")
                # Refresh video info
                new_info = load_video_info(self.current_video.path)
                if new_info:
                    self.current_video = new_info
                    # Update list item
                    row = self.video_list.currentRow()
                    if row >= 0:
                        item = self.video_list.item(row)
                        item.setData(Qt.ItemDataRole.UserRole, new_info)
                        status = self._get_video_status(new_info)
                        item.setText(f"[{status}] {new_info.name}")
                    self._update_ui()
            else:
                QMessageBox.warning(self, "Error", f"Command failed:\n{result.stderr}")
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to run command: {e}")

    def _delete_video(self) -> None:
        """Delete the current video."""
        if not self.current_video:
            return

        reply = QMessageBox.question(
            self,
            "Delete Video",
            f"Are you sure you want to delete {self.current_video.name}?\n\n"
            "This will permanently delete the video directory and all its contents.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )

        if reply != QMessageBox.StandardButton.Yes:
            return

        try:
            shutil.rmtree(self.current_video.path)

            row = self.video_list.currentRow()
            self.video_list.takeItem(row)
            self.videos = [v for v in self.videos if v.name != self.current_video.name]

            # Remove symlink if exists
            if self.batch_dir:
                symlink = self.batch_dir / f"{self.current_video.name}.mp4"
                if symlink.is_symlink():
                    symlink.unlink()

            self.statusBar().showMessage(f"Deleted {self.current_video.name}", 2000)

        except Exception as e:
            QMessageBox.critical(self, "Delete Error", f"Failed to delete: {e}")


def main() -> int:
    """Main entry point."""
    parser = argparse.ArgumentParser(description="Batch video annotation tool")
    parser.add_argument("batch_dir", nargs="?", help="Path to batch directory")
    args = parser.parse_args()

    batch_dir = Path(args.batch_dir) if args.batch_dir else None

    app = QApplication(sys.argv)
    window = MainWindow(batch_dir)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
