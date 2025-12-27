#!/usr/bin/env python3
"""
Batch video production studio.

Refined workflow: Core production actions (1→2→3→4) are always visible;
advanced settings are tucked into a 'Config' tab on the right.

Entry point: batch-annotate
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from PyQt6.QtCore import QSettings, Qt, QTimer
from PyQt6.QtGui import QColor, QFont, QKeySequence, QPalette, QShortcut
from PyQt6.QtWidgets import (
    QApplication,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QTabWidget,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from .config import get_config
from .models import VideoMetadata
from .music import MusicDatabase, MusicTrack
from .processing.templates import TemplateLibrary
from .templates import generate_description, generate_title

MPV_COMMAND = "mpv"


class Separator(QFrame):
    """Horizontal separator line."""

    def __init__(self):
        super().__init__()
        self.setFrameShape(QFrame.Shape.HLine)
        self.setFrameShadow(QFrame.Shadow.Sunken)
        self.setStyleSheet("color: #ddd;")


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
    has_processed: bool = False
    has_music: bool = False
    music_title: Optional[str] = None


def get_best_video_path(video_dir: Path) -> Optional[Path]:
    """Get the most processed video available."""
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


def load_video_info(video_dir: Path) -> Optional[VideoInfo]:
    """Load video info from a video directory."""
    if not video_dir.is_dir() or not video_dir.name.startswith("video_"):
        return None

    metadata_path = video_dir / "metadata.json"
    has_metadata = metadata_path.exists()

    boom_frame = None
    boom_seconds = None
    video_fps = 60
    duration_seconds = 30.0
    music_title = None

    if has_metadata:
        try:
            data = json.loads(metadata_path.read_text())
            results = data.get("results", {})
            boom_frame = results.get("boom_frame")
            boom_seconds = results.get("boom_seconds")

            output = data.get("output", {})
            video_fps = output.get("video_fps", 60)
            duration_seconds = output.get("video_duration", 30.0)

            music = data.get("music", {})
            music_title = music.get("title")
        except Exception:
            pass

    return VideoInfo(
        name=video_dir.name,
        path=video_dir,
        metadata_path=metadata_path,
        has_metadata=has_metadata,
        has_video=(video_dir / "video_raw.mp4").exists(),
        boom_frame=boom_frame,
        boom_seconds=boom_seconds,
        video_fps=video_fps,
        duration_seconds=duration_seconds,
        best_video_path=get_best_video_path(video_dir),
        has_processed=(video_dir / "video_processed.mp4").exists(),
        has_music=(
            (video_dir / "video_processed_final.mp4").exists()
            or (video_dir / "video.mp4").exists()
        ),
        music_title=music_title,
    )


def scan_batch(batch_dir: Path) -> list[VideoInfo]:
    """Scan a batch directory for videos."""
    videos = []
    for item in sorted(batch_dir.iterdir()):
        info = load_video_info(item)
        if info:
            videos.append(info)
    return videos


class MainWindow(QMainWindow):
    """Video Production Studio - streamlined 1→2→3→4 workflow."""

    def __init__(self, batch_dir: Optional[Path] = None):
        super().__init__()
        self.setWindowTitle("Video Production Studio")
        self.resize(1300, 850)

        self.settings = QSettings("double-pendulum", "batch-annotate")
        self.batch_dir: Optional[Path] = None
        self.videos: list[VideoInfo] = []
        self.current_video: Optional[VideoInfo] = None
        self.music_db: Optional[MusicDatabase] = None
        self.valid_tracks: list[MusicTrack] = []
        self.template_lib: Optional[TemplateLibrary] = None

        self.autosave_timer = QTimer()
        self.autosave_timer.setSingleShot(True)
        self.autosave_timer.timeout.connect(self._do_autosave)

        self._init_ui()
        self._setup_shortcuts()

        # Load resources after UI is ready
        QTimer.singleShot(0, self._load_resources)

        self.statusBar().show()

        if batch_dir:
            self._load_batch(batch_dir)
        else:
            last_batch = self.settings.value("last_batch", "", type=str)
            if last_batch and Path(last_batch).exists():
                self._load_batch(Path(last_batch))

    def _load_resources(self) -> None:
        """Load templates and music database."""
        try:
            self.template_lib = TemplateLibrary()
            self.combo_template.addItem("random")
            for name in sorted(self.template_lib.list_templates()):
                tmpl = self.template_lib.get(name)
                self.combo_template.addItem(f"{name} - {tmpl.description}")
            # Default to minimal_science
            for i in range(self.combo_template.count()):
                if self.combo_template.itemText(i).startswith("minimal_science"):
                    self.combo_template.setCurrentIndex(i)
                    break
        except Exception as e:
            print(f"Warning: Could not load templates: {e}")

        try:
            config = get_config()
            self.music_db = MusicDatabase(config.get_music_dir(None))
        except Exception as e:
            print(f"Warning: Could not load music database: {e}")

    def _init_ui(self) -> None:
        """Initialize the production studio layout."""
        central = QWidget()
        self.setCentralWidget(central)

        main_layout = QHBoxLayout(central)
        main_layout.setContentsMargins(10, 10, 10, 10)
        main_layout.setSpacing(15)

        # === SIDEBAR (Left, 280px): Batch Navigation ===
        sidebar = self._create_sidebar()
        sidebar.setFixedWidth(280)
        main_layout.addWidget(sidebar)

        # === PRODUCTION AREA (Center, expanding): The 1→2→3→4 Workflow ===
        production = self._create_production_panel()
        main_layout.addWidget(production, 1)

        # === SETTINGS PANEL (Right, 360px): Metadata + Config tabs ===
        settings_panel = self._create_settings_panel()
        settings_panel.setFixedWidth(360)
        main_layout.addWidget(settings_panel)

    def _create_sidebar(self) -> QWidget:
        """Create the left sidebar with batch navigation."""
        sidebar = QWidget()
        layout = QVBoxLayout(sidebar)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        self.btn_open = QPushButton("Open Batch...")
        self.btn_open.setMinimumHeight(35)
        self.btn_open.clicked.connect(self._open_batch_dialog)
        layout.addWidget(self.btn_open)

        self.lbl_batch = QLabel("No batch loaded")
        self.lbl_batch.setStyleSheet("color: gray; font-style: italic;")
        layout.addWidget(self.lbl_batch)

        layout.addWidget(Separator())

        self.video_list = QListWidget()
        self.video_list.currentItemChanged.connect(self._on_video_selected)
        layout.addWidget(self.video_list, 1)

        nav_row = QHBoxLayout()
        self.btn_prev = QPushButton("< Prev (P)")
        self.btn_next = QPushButton("Next (N) >")
        self.btn_prev.clicked.connect(self._prev_video)
        self.btn_next.clicked.connect(self._next_video)
        nav_row.addWidget(self.btn_prev)
        nav_row.addWidget(self.btn_next)
        layout.addLayout(nav_row)

        return sidebar

    def _create_production_panel(self) -> QWidget:
        """Create the center production panel with 1→2→3→4 workflow."""
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)

        # Video Header
        self.lbl_title = QLabel("Select a video")
        title_font = QFont()
        title_font.setPointSize(18)
        title_font.setBold(True)
        self.lbl_title.setFont(title_font)
        layout.addWidget(self.lbl_title)

        self.lbl_stats = QLabel("FPS: -- | Duration: -- | Status: --")
        self.lbl_stats.setStyleSheet("color: #666;")
        layout.addWidget(self.lbl_stats)

        layout.addWidget(Separator())

        # === 1. ANNOTATION & TIMING ===
        group1 = QGroupBox("1. Annotation & Timing")
        g1_layout = QVBoxLayout(group1)

        boom_row = QHBoxLayout()
        boom_row.addWidget(QLabel("Boom Frame:"))

        self.spin_boom = QSpinBox()
        self.spin_boom.setRange(0, 100000)
        self.spin_boom.setFixedSize(130, 42)
        boom_font = QFont()
        boom_font.setPointSize(16)
        boom_font.setBold(True)
        self.spin_boom.setFont(boom_font)
        self.spin_boom.valueChanged.connect(self._on_boom_changed)
        boom_row.addWidget(self.spin_boom)

        self.lbl_boom_secs = QLabel("(0.00s)")
        self.lbl_boom_secs.setStyleSheet("font-size: 14px; color: #1976d2;")
        boom_row.addWidget(self.lbl_boom_secs)

        boom_row.addStretch()

        self.lbl_autosave = QLabel("")
        self.lbl_autosave.setStyleSheet("font-size: 16px;")
        self.lbl_autosave.setFixedWidth(30)
        boom_row.addWidget(self.lbl_autosave)

        g1_layout.addLayout(boom_row)

        play_row = QHBoxLayout()
        self.btn_play = QPushButton("Play Video (O)")
        self.btn_play.setMinimumHeight(42)
        self.btn_play.clicked.connect(self._play_video)
        self.btn_play.setEnabled(False)
        play_row.addWidget(self.btn_play)

        self.btn_play_boom = QPushButton("Play at Boom")
        self.btn_play_boom.setMinimumHeight(42)
        self.btn_play_boom.clicked.connect(self._play_at_boom)
        self.btn_play_boom.setEnabled(False)
        play_row.addWidget(self.btn_play_boom)

        g1_layout.addLayout(play_row)
        layout.addWidget(group1)

        # === 2. SOUNDTRACK ===
        group2 = QGroupBox("2. Soundtrack")
        g2_layout = QVBoxLayout(group2)

        music_row = QHBoxLayout()
        self.combo_music = QComboBox()
        self.combo_music.setEnabled(False)
        music_row.addWidget(self.combo_music, 1)

        self.btn_add_music = QPushButton("Add Music")
        self.btn_add_music.setFixedWidth(110)
        self.btn_add_music.setMinimumHeight(35)
        self.btn_add_music.clicked.connect(self._add_music)
        self.btn_add_music.setEnabled(False)
        music_row.addWidget(self.btn_add_music)

        g2_layout.addLayout(music_row)

        self.lbl_current_music = QLabel("Current: None")
        self.lbl_current_music.setStyleSheet("color: #666; font-size: 11px;")
        g2_layout.addWidget(self.lbl_current_music)

        layout.addWidget(group2)

        # === 3. RENDERING ===
        group3 = QGroupBox("3. Rendering")
        g3_layout = QHBoxLayout(group3)

        self.btn_process = QPushButton("Process FX")
        self.btn_process.setMinimumHeight(45)
        self.btn_process.setStyleSheet(
            "background: #2196F3; color: white; font-weight: bold;"
        )
        self.btn_process.clicked.connect(self._process_video)
        self.btn_process.setEnabled(False)
        g3_layout.addWidget(self.btn_process)

        self.btn_process_music = QPushButton("Process + Music")
        self.btn_process_music.setMinimumHeight(45)
        self.btn_process_music.setStyleSheet(
            "background: #4CAF50; color: white; font-weight: bold;"
        )
        self.btn_process_music.clicked.connect(self._process_with_music)
        self.btn_process_music.setEnabled(False)
        g3_layout.addWidget(self.btn_process_music)

        layout.addWidget(group3)

        # === 4. PUBLISHING ===
        group4 = QGroupBox("4. Publishing")
        g4_layout = QVBoxLayout(group4)

        # Title row
        title_row = QHBoxLayout()
        title_row.addWidget(QLabel("Title:"))
        self.edit_title = QLineEdit()
        self.edit_title.setReadOnly(True)
        title_row.addWidget(self.edit_title)
        self.btn_regen_title = QPushButton("↻")
        self.btn_regen_title.setFixedWidth(30)
        self.btn_regen_title.setToolTip("Regenerate title")
        self.btn_regen_title.clicked.connect(lambda: self._regenerate("title"))
        title_row.addWidget(self.btn_regen_title)
        self.btn_copy_title = QPushButton("Copy")
        self.btn_copy_title.setFixedWidth(50)
        self.btn_copy_title.clicked.connect(
            lambda: self._copy_to_clipboard(self.edit_title.text())
        )
        title_row.addWidget(self.btn_copy_title)
        g4_layout.addLayout(title_row)

        # Description row
        desc_header = QHBoxLayout()
        desc_header.addWidget(QLabel("Description:"))
        desc_header.addStretch()
        self.btn_regen_desc = QPushButton("↻")
        self.btn_regen_desc.setFixedWidth(30)
        self.btn_regen_desc.setToolTip("Regenerate description")
        self.btn_regen_desc.clicked.connect(lambda: self._regenerate("description"))
        desc_header.addWidget(self.btn_regen_desc)
        self.btn_copy_desc = QPushButton("Copy")
        self.btn_copy_desc.setFixedWidth(50)
        self.btn_copy_desc.clicked.connect(
            lambda: self._copy_to_clipboard(self.edit_desc.toPlainText())
        )
        desc_header.addWidget(self.btn_copy_desc)
        g4_layout.addLayout(desc_header)

        self.edit_desc = QTextEdit()
        self.edit_desc.setReadOnly(True)
        self.edit_desc.setMaximumHeight(70)
        g4_layout.addWidget(self.edit_desc)

        # Path row
        path_row = QHBoxLayout()
        path_row.addWidget(QLabel("Path:"))
        self.edit_path = QLineEdit()
        self.edit_path.setReadOnly(True)
        path_row.addWidget(self.edit_path)
        self.btn_copy_path = QPushButton("Copy")
        self.btn_copy_path.setFixedWidth(50)
        self.btn_copy_path.clicked.connect(
            lambda: self._copy_to_clipboard(self.edit_path.text())
        )
        path_row.addWidget(self.btn_copy_path)
        g4_layout.addLayout(path_row)

        # Upload button
        self.btn_upload = QPushButton("Upload to YouTube")
        self.btn_upload.setMinimumHeight(45)
        self.btn_upload.setStyleSheet(
            "background: #FF0000; color: white; font-weight: bold;"
        )
        self.btn_upload.clicked.connect(self._upload_video)
        self.btn_upload.setEnabled(False)
        g4_layout.addWidget(self.btn_upload)

        layout.addWidget(group4)

        layout.addStretch()

        # Delete button (de-emphasized)
        self.btn_delete = QPushButton("Delete Project")
        self.btn_delete.setStyleSheet("color: #999; border: 1px solid #ddd;")
        self.btn_delete.clicked.connect(self._delete_video)
        self.btn_delete.setEnabled(False)
        layout.addWidget(self.btn_delete)

        return panel

    def _create_settings_panel(self) -> QWidget:
        """Create the right panel with Metadata and Config tabs."""
        tabs = QTabWidget()

        # === METADATA TAB ===
        meta_tab = QWidget()
        meta_layout = QVBoxLayout(meta_tab)

        self.txt_metadata = QTextEdit()
        self.txt_metadata.setReadOnly(True)
        self.txt_metadata.setStyleSheet(
            "font-family: monospace; font-size: 11px; background: #fafafa;"
        )
        meta_layout.addWidget(self.txt_metadata)

        tabs.addTab(meta_tab, "Metadata")

        # === CONFIG TAB (Advanced settings) ===
        config_tab = QWidget()
        config_layout = QVBoxLayout(config_tab)

        # Template group
        template_group = QGroupBox("Template")
        template_layout = QVBoxLayout(template_group)
        self.combo_template = QComboBox()
        self.combo_template.currentIndexChanged.connect(self._on_template_changed)
        template_layout.addWidget(self.combo_template)
        config_layout.addWidget(template_group)

        # Motion group
        motion_group = QGroupBox("Motion (Slow Zoom)")
        motion_form = QFormLayout(motion_group)

        self.spin_zoom_start = QDoubleSpinBox()
        self.spin_zoom_start.setRange(0.5, 2.0)
        self.spin_zoom_start.setSingleStep(0.01)
        self.spin_zoom_start.setDecimals(2)
        self.spin_zoom_start.setValue(1.0)
        motion_form.addRow("Zoom Start:", self.spin_zoom_start)

        self.spin_zoom_end = QDoubleSpinBox()
        self.spin_zoom_end.setRange(0.5, 2.0)
        self.spin_zoom_end.setSingleStep(0.01)
        self.spin_zoom_end.setDecimals(2)
        self.spin_zoom_end.setValue(1.08)
        motion_form.addRow("Zoom End:", self.spin_zoom_end)

        config_layout.addWidget(motion_group)

        # Background group
        bg_group = QGroupBox("Background (Shorts)")
        bg_form = QFormLayout(bg_group)

        self.spin_blur = QSpinBox()
        self.spin_blur.setRange(5, 100)
        self.spin_blur.setValue(50)
        self.spin_blur.setToolTip("Blur strength for Shorts background")
        bg_form.addRow("Blur:", self.spin_blur)

        self.spin_brightness = QDoubleSpinBox()
        self.spin_brightness.setRange(0.0, 1.0)
        self.spin_brightness.setSingleStep(0.05)
        self.spin_brightness.setDecimals(2)
        self.spin_brightness.setValue(1.0)
        self.spin_brightness.setToolTip("Background brightness (0-1)")
        bg_form.addRow("Brightness:", self.spin_brightness)

        config_layout.addWidget(bg_group)

        config_layout.addStretch()
        tabs.addTab(config_tab, "Config")

        return tabs

    def _setup_shortcuts(self) -> None:
        """Set up keyboard shortcuts."""
        QShortcut(QKeySequence("N"), self).activated.connect(self._next_video)
        QShortcut(QKeySequence("P"), self).activated.connect(self._prev_video)
        QShortcut(QKeySequence("O"), self).activated.connect(self._play_video)
        QShortcut(QKeySequence("Ctrl+O"), self).activated.connect(self._open_batch_dialog)

    def _open_batch_dialog(self) -> None:
        """Open a file dialog to select a batch directory."""
        start_dir = str(self.batch_dir) if self.batch_dir else str(Path.cwd() / "batch_output")
        dir_path = QFileDialog.getExistingDirectory(self, "Select Batch Directory", start_dir)
        if dir_path:
            self._load_batch(Path(dir_path))

    def _load_batch(self, batch_dir: Path) -> None:
        """Load a batch directory."""
        self.batch_dir = batch_dir
        self.videos = scan_batch(batch_dir)

        self.settings.setValue("last_batch", str(batch_dir))

        self.lbl_batch.setText(batch_dir.name)
        self.lbl_batch.setStyleSheet("font-weight: bold;")
        self.setWindowTitle(f"Video Production Studio - {batch_dir.name}")

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
            return "?"
        elif video.has_processed:
            return "✓"
        elif video.has_music:
            return "♪"
        else:
            return "+"

    def _on_video_selected(self, current: Optional[QListWidgetItem], _previous) -> None:
        """Handle video selection."""
        if not current:
            self.current_video = None
            self._clear_ui()
            return

        self.current_video = current.data(Qt.ItemDataRole.UserRole)
        self._update_ui()
        self._update_music_combo()

    def _clear_ui(self) -> None:
        """Clear all UI fields."""
        self.lbl_title.setText("Select a video")
        self.lbl_stats.setText("FPS: -- | Duration: -- | Status: --")
        self.spin_boom.setValue(0)
        self.lbl_boom_secs.setText("(0.00s)")
        self.lbl_autosave.setText("")
        self.combo_music.clear()
        self.lbl_current_music.setText("Current: None")
        self.edit_title.clear()
        self.edit_desc.clear()
        self.edit_path.clear()
        self.txt_metadata.clear()

        # Disable buttons
        self.btn_play.setEnabled(False)
        self.btn_play_boom.setEnabled(False)
        self.combo_music.setEnabled(False)
        self.btn_add_music.setEnabled(False)
        self.btn_process.setEnabled(False)
        self.btn_process_music.setEnabled(False)
        self.btn_upload.setEnabled(False)
        self.btn_delete.setEnabled(False)

    def _update_ui(self) -> None:
        """Update UI based on current video."""
        video = self.current_video
        if not video:
            self._clear_ui()
            return

        self.lbl_autosave.setText("")

        # Header
        self.lbl_title.setText(video.name)
        video_file = video.best_video_path.name if video.best_video_path else "None"
        status = "Processed" if video.has_processed else ("Music" if video.has_music else "Raw")
        self.lbl_stats.setText(
            f"FPS: {video.video_fps} | Duration: {video.duration_seconds:.1f}s | "
            f"Video: {video_file} | Status: {status}"
        )

        # Boom spinner
        self.spin_boom.blockSignals(True)
        self.spin_boom.setMaximum(int(video.duration_seconds * video.video_fps))
        self.spin_boom.setValue(video.boom_frame or 0)
        self.spin_boom.blockSignals(False)
        self._update_boom_label()

        # Current music
        self.lbl_current_music.setText(
            f"Current: {video.music_title}" if video.music_title else "Current: None"
        )

        # Publishing info
        if video.best_video_path:
            self.edit_path.setText(str(video.best_video_path.resolve()))
        else:
            self.edit_path.clear()

        try:
            metadata = VideoMetadata.from_file(video.metadata_path)
            self.edit_title.setText(generate_title(metadata))
            self.edit_desc.setText(generate_description(metadata))

            # Metadata tab
            date_str = metadata.created_at.strftime("%Y-%m-%d %H:%M")
            color_preset = f" ({metadata.color.preset_name})" if metadata.color.preset_name else ""
            pp_preset = f" ({metadata.post_process.preset_name})" if metadata.post_process.preset_name else ""
            meta_text = (
                f"=== Simulation ===\n"
                f"Pendulums: {metadata.simulation.pendulum_count:,}\n"
                f"Date: {date_str}\n\n"
                f"=== Color{color_preset} ===\n"
                f"Scheme: {metadata.color.scheme}\n"
                f"Range: {metadata.color.start} - {metadata.color.end}\n\n"
                f"=== Post-Process{pp_preset} ===\n"
                f"Tone Map: {metadata.post_process.tone_map}\n"
                f"Exposure: {metadata.post_process.exposure}\n"
                f"Contrast: {metadata.post_process.contrast}\n"
                f"Gamma: {metadata.post_process.gamma}\n\n"
                f"=== Results ===\n"
                f"Boom Frame: {video.boom_frame}\n"
                f"Boom Time: {video.boom_seconds:.2f}s" if video.boom_seconds else ""
            )
            self.txt_metadata.setText(meta_text)
        except Exception:
            self.edit_title.clear()
            self.edit_desc.clear()
            self.txt_metadata.setText("Metadata not available")

        # Enable buttons based on state
        has_boom = video.boom_frame is not None and video.boom_frame > 0
        self.btn_play.setEnabled(video.best_video_path is not None)
        self.btn_play_boom.setEnabled(video.best_video_path is not None and has_boom)
        self.combo_music.setEnabled(has_boom and self.music_db is not None)
        self.btn_add_music.setEnabled(has_boom and self.music_db is not None)
        self.btn_process.setEnabled(video.has_video and has_boom)
        self.btn_process_music.setEnabled(
            video.has_video and has_boom and self.music_db is not None
        )
        self.btn_upload.setEnabled(video.best_video_path is not None)
        self.btn_delete.setEnabled(True)

    def _update_boom_label(self) -> None:
        """Update the boom seconds label."""
        if not self.current_video:
            return
        frame = self.spin_boom.value()
        seconds = frame / self.current_video.video_fps
        self.lbl_boom_secs.setText(f"({seconds:.2f}s)")

    def _update_music_combo(self) -> None:
        """Update music dropdown with valid tracks."""
        self.combo_music.clear()
        self.valid_tracks = []

        if not self.music_db or not self.current_video:
            return

        boom_frame = self.spin_boom.value()
        if boom_frame <= 0:
            return

        boom_seconds = boom_frame / self.current_video.video_fps
        self.valid_tracks = self.music_db.get_valid_tracks_for_boom(boom_seconds)

        if not self.valid_tracks:
            self.combo_music.addItem("No valid tracks")
            self.combo_music.setEnabled(False)
            self.btn_add_music.setEnabled(False)
            return

        self.combo_music.addItem(f"Random ({len(self.valid_tracks)} tracks)")
        for track in self.valid_tracks:
            self.combo_music.addItem(f"{track.title} (drop: {track.drop_time_seconds:.1f}s)")

    def _on_boom_changed(self, _value: int) -> None:
        """Handle boom frame change - trigger autosave."""
        self._update_boom_label()
        self._update_music_combo()

        if self.current_video and self.current_video.has_metadata:
            self.autosave_timer.stop()
            self.autosave_timer.start(500)
            self.lbl_autosave.setText("...")
            self.lbl_autosave.setStyleSheet("color: #666; font-size: 16px;")

    def _do_autosave(self) -> None:
        """Perform the actual autosave."""
        if not self.current_video or not self.current_video.has_metadata:
            return

        try:
            data = json.loads(self.current_video.metadata_path.read_text())
            new_boom_frame = self.spin_boom.value()

            if "results" not in data:
                data["results"] = {}

            data["results"]["boom_frame"] = new_boom_frame
            data["results"]["boom_seconds"] = new_boom_frame / self.current_video.video_fps

            self.current_video.metadata_path.write_text(json.dumps(data, indent=2) + "\n")

            self.current_video.boom_frame = new_boom_frame
            self.current_video.boom_seconds = new_boom_frame / self.current_video.video_fps

            row = self.video_list.currentRow()
            if row >= 0:
                item = self.video_list.item(row)
                status = self._get_video_status(self.current_video)
                item.setText(f"[{status}] {self.current_video.name}")

            self.lbl_autosave.setText("✓")
            self.lbl_autosave.setStyleSheet("color: #4CAF50; font-size: 16px;")
            QTimer.singleShot(2000, lambda: self.lbl_autosave.setText(""))

        except Exception as e:
            self.lbl_autosave.setText("✗")
            self.lbl_autosave.setStyleSheet("color: #f44336; font-size: 16px;")
            self.statusBar().showMessage(f"Save error: {e}", 3000)

    def _on_template_changed(self, _index: int) -> None:
        """Update zoom spinboxes when template changes."""
        if not self.template_lib:
            return

        text = self.combo_template.currentText()
        if text == "random":
            self.spin_zoom_start.setValue(1.0)
            self.spin_zoom_end.setValue(1.08)
            return

        template_name = text.split(" - ")[0]
        try:
            template = self.template_lib.get(template_name)
            if template.motion and template.motion.slow_zoom:
                self.spin_zoom_start.setValue(template.motion.slow_zoom.start)
                self.spin_zoom_end.setValue(template.motion.slow_zoom.end)
            else:
                self.spin_zoom_start.setValue(1.0)
                self.spin_zoom_end.setValue(1.0)
        except KeyError:
            pass

    def _copy_to_clipboard(self, text: str) -> None:
        """Copy text to clipboard and show confirmation."""
        clipboard = QApplication.clipboard()
        clipboard.setText(text)
        self.statusBar().showMessage("Copied to clipboard", 1500)

    def _regenerate(self, field: str) -> None:
        """Regenerate title or description from templates."""
        if not self.current_video or not self.current_video.has_metadata:
            return
        try:
            metadata = VideoMetadata.from_file(self.current_video.metadata_path)
            if field == "title":
                self.edit_title.setText(generate_title(metadata))
                self.statusBar().showMessage("Title regenerated", 1500)
            elif field == "description":
                self.edit_desc.setText(generate_description(metadata))
                self.statusBar().showMessage("Description regenerated", 1500)
        except Exception as e:
            self.statusBar().showMessage(f"Error: {e}", 3000)

    # === ACTIONS ===

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

        frame = self.spin_boom.value()
        if frame <= 0:
            self._play_video()
            return

        seconds = frame / self.current_video.video_fps
        start_seconds = max(0, seconds - 3)

        if shutil.which(MPV_COMMAND):
            subprocess.Popen([
                MPV_COMMAND,
                f"--start={start_seconds:.2f}",
                str(self.current_video.best_video_path),
            ])
        else:
            QMessageBox.warning(self, "mpv not found", "mpv is not installed or not in PATH")

    def _get_selected_track_id(self) -> Optional[str]:
        """Get the track ID from the combo selection, or None for random."""
        idx = self.combo_music.currentIndex()
        if idx <= 0 or idx > len(self.valid_tracks):
            return None
        return self.valid_tracks[idx - 1].id

    def _get_selected_template(self) -> str:
        """Get the selected template name."""
        text = self.combo_template.currentText()
        if text == "random":
            return "random"
        return text.split(" - ")[0]

    def _run_cli_command(
        self,
        command: str,
        subcommand: str = "",
        music: bool = False,
        track_id: Optional[str] = None,
    ) -> None:
        """Run a pendulum-tools CLI command."""
        if not self.current_video:
            return

        video_dir = str(self.current_video.path)

        if command == "music":
            cmd = ["uv", "run", "pendulum-tools", "music", subcommand, video_dir]
            if track_id:
                cmd.extend(["--track", track_id])
        elif command == "process":
            template = self._get_selected_template()
            zoom_start = self.spin_zoom_start.value()
            zoom_end = self.spin_zoom_end.value()
            blur = self.spin_blur.value()
            brightness = self.spin_brightness.value()
            cmd = [
                "uv", "run", "pendulum-tools", "process", video_dir,
                "--shorts", "--blur-bg", "--force",
                "--template", template,
                "--zoom-start", str(zoom_start),
                "--zoom-end", str(zoom_end),
                "--blur-strength", str(blur),
                "--bg-brightness", str(brightness),
            ]
            if music:
                cmd.append("--music")
                if track_id:
                    cmd.extend(["--track", track_id])
        elif command == "upload":
            cmd = ["uv", "run", "pendulum-tools", "upload", video_dir, "--privacy", "public"]
        else:
            return

        print(f"\n>>> Running: {' '.join(cmd)}")
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, cwd=str(Path.cwd()))

            if result.stdout:
                print(result.stdout)
            if result.stderr:
                print(result.stderr)

            if result.returncode == 0:
                action = command if not subcommand else f"{command} {subcommand}"
                self.statusBar().showMessage(f"Done: {action}", 3000)

                # Refresh current video info
                new_info = load_video_info(self.current_video.path)
                if new_info:
                    self.current_video = new_info
                    row = self.video_list.currentRow()
                    if row >= 0:
                        item = self.video_list.item(row)
                        item.setData(Qt.ItemDataRole.UserRole, new_info)
                        status = self._get_video_status(new_info)
                        item.setText(f"[{status}] {new_info.name}")
                    self._update_ui()
            else:
                error_line = result.stderr.strip().split("\n")[-1] if result.stderr else "Unknown error"
                self.statusBar().showMessage(f"Failed: {error_line[:80]}", 5000)
        except Exception as e:
            print(f"Error: {e}")
            self.statusBar().showMessage(f"Error: {e}", 5000)

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

    def _delete_video(self) -> None:
        """Delete the current video project."""
        if not self.current_video:
            return

        reply = QMessageBox.question(
            self,
            "Delete Project",
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

            if self.batch_dir:
                symlink = self.batch_dir / f"{self.current_video.name}.mp4"
                if symlink.is_symlink():
                    symlink.unlink()

            self.statusBar().showMessage(f"Deleted {self.current_video.name}", 2000)
        except Exception as e:
            QMessageBox.critical(self, "Delete Error", f"Failed to delete: {e}")


def main() -> int:
    """Main entry point."""
    parser = argparse.ArgumentParser(description="Video production studio")
    parser.add_argument("batch_dir", nargs="?", help="Path to batch directory")
    args = parser.parse_args()

    batch_dir = Path(args.batch_dir) if args.batch_dir else None

    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    # Light palette
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window, QColor(248, 248, 248))
    app.setPalette(palette)

    window = MainWindow(batch_dir)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
