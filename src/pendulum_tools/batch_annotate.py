#!/usr/bin/env python3
"""
Batch video annotation and processing GUI.

Three-Pane Studio layout:
- Navigator (Left): Batch selection and video list
- Stage (Center): Annotation and playback controls
- Property Panel (Right): Processing, Music, and Publishing tabs

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
from PyQt6.QtGui import QFont, QKeySequence, QShortcut
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
    QSizePolicy,
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

# Layout constants
LEFT_PANEL_WIDTH = 280
RIGHT_PANEL_WIDTH = 380
BUTTON_HEIGHT = 40
SPACING = 10


def create_separator() -> QFrame:
    """Create a horizontal separator line."""
    line = QFrame()
    line.setFrameShape(QFrame.Shape.HLine)
    line.setFrameShadow(QFrame.Shadow.Sunken)
    line.setStyleSheet("background-color: #c0c0c0;")
    line.setFixedHeight(1)
    return line


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

    has_processed = (video_dir / "video_processed.mp4").exists()
    has_music = (video_dir / "video_processed_final.mp4").exists() or (
        video_dir / "video.mp4"
    ).exists()

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
        music_title=music_title,
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
    """Main window for batch annotation - Three-Pane Studio layout."""

    def __init__(self, batch_dir: Optional[Path] = None):
        super().__init__()
        self.setWindowTitle("Batch Video Annotator")
        self.resize(1200, 800)

        self.settings = QSettings("double-pendulum", "batch-annotate")
        self.batch_dir: Optional[Path] = None
        self.videos: list[VideoInfo] = []
        self.current_video: Optional[VideoInfo] = None
        self.music_db: Optional[MusicDatabase] = None
        self.valid_tracks: list[MusicTrack] = []
        self.template_lib: Optional[TemplateLibrary] = None

        # Autosave timer
        self.autosave_timer = QTimer()
        self.autosave_timer.setSingleShot(True)
        self.autosave_timer.timeout.connect(self._do_autosave)

        self._init_ui()
        self._setup_shortcuts()
        self._load_music_database()
        self._load_templates()

        self.statusBar().show()

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

    def _load_templates(self) -> None:
        """Load templates and populate the combo."""
        try:
            self.template_lib = TemplateLibrary()
            templates = self.template_lib.list_templates()
            self.template_combo.addItem("random")
            for name in sorted(templates):
                template = self.template_lib.get(name)
                self.template_combo.addItem(f"{name} - {template.description}")
            for i in range(self.template_combo.count()):
                if self.template_combo.itemText(i).startswith("minimal_science"):
                    self.template_combo.setCurrentIndex(i)
                    break
        except Exception as e:
            print(f"Warning: Could not load templates: {e}")
            self.template_lib = None

    def _init_ui(self) -> None:
        """Initialize the Three-Pane Studio layout."""
        central = QWidget()
        self.setCentralWidget(central)

        main_layout = QHBoxLayout(central)
        main_layout.setSpacing(SPACING)
        main_layout.setContentsMargins(SPACING, SPACING, SPACING, SPACING)

        # === ZONE A: Navigator (Left) ===
        left_panel = self._create_navigator_panel()
        left_panel.setFixedWidth(LEFT_PANEL_WIDTH)
        main_layout.addWidget(left_panel)

        # === ZONE B: Stage (Center) ===
        center_panel = self._create_stage_panel()
        center_panel.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        main_layout.addWidget(center_panel, 1)

        # === ZONE C: Property Panel (Right) ===
        right_panel = self._create_property_panel()
        right_panel.setFixedWidth(RIGHT_PANEL_WIDTH)
        main_layout.addWidget(right_panel)

    def _create_navigator_panel(self) -> QWidget:
        """Create Zone A: Navigator panel."""
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setSpacing(SPACING)
        layout.setContentsMargins(0, 0, 0, 0)

        # Batch controls
        self.open_btn = QPushButton("Open Batch...")
        self.open_btn.clicked.connect(self._open_batch_dialog)
        layout.addWidget(self.open_btn)

        self.batch_label = QLabel("No batch loaded")
        self.batch_label.setStyleSheet("color: gray; font-style: italic;")
        self.batch_label.setWordWrap(True)
        layout.addWidget(self.batch_label)

        layout.addWidget(create_separator())

        # Video list
        self.video_list = QListWidget()
        self.video_list.currentItemChanged.connect(self._on_video_selected)
        layout.addWidget(self.video_list, 1)

        # Navigation
        nav_layout = QHBoxLayout()
        self.prev_btn = QPushButton("< Prev (P)")
        self.prev_btn.clicked.connect(self._prev_video)
        nav_layout.addWidget(self.prev_btn)

        self.next_btn = QPushButton("Next (N) >")
        self.next_btn.clicked.connect(self._next_video)
        nav_layout.addWidget(self.next_btn)
        layout.addLayout(nav_layout)

        return panel

    def _create_stage_panel(self) -> QWidget:
        """Create Zone B: Stage panel with hero controls."""
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setSpacing(SPACING)
        layout.setContentsMargins(0, 0, 0, 0)

        # Video title (large)
        self.video_title = QLabel("Select a video")
        title_font = QFont()
        title_font.setPointSize(16)
        title_font.setBold(True)
        self.video_title.setFont(title_font)
        layout.addWidget(self.video_title)

        # Video stats
        self.video_stats = QLabel("")
        self.video_stats.setStyleSheet("color: #666;")
        layout.addWidget(self.video_stats)

        layout.addWidget(create_separator())

        # === Hero Timing Control ===
        timing_group = QGroupBox("Timing")
        timing_layout = QVBoxLayout(timing_group)

        # Boom frame row with large spinbox
        boom_row = QHBoxLayout()

        boom_label = QLabel("Boom Frame:")
        boom_label.setStyleSheet("font-size: 14px;")
        boom_row.addWidget(boom_label)

        self.boom_spin = QSpinBox()
        self.boom_spin.setRange(0, 100000)
        self.boom_spin.setEnabled(False)
        self.boom_spin.valueChanged.connect(self._on_boom_changed)
        boom_font = QFont()
        boom_font.setPointSize(18)
        self.boom_spin.setFont(boom_font)
        self.boom_spin.setMinimumWidth(120)
        self.boom_spin.setMinimumHeight(40)
        boom_row.addWidget(self.boom_spin)

        self.boom_seconds_label = QLabel("(0.0s)")
        self.boom_seconds_label.setStyleSheet("font-size: 14px; color: #666;")
        boom_row.addWidget(self.boom_seconds_label)

        # Save status indicator
        self.save_status = QLabel("")
        self.save_status.setStyleSheet("font-size: 16px;")
        self.save_status.setFixedWidth(30)
        boom_row.addWidget(self.save_status)

        boom_row.addStretch()
        timing_layout.addLayout(boom_row)

        layout.addWidget(timing_group)

        layout.addWidget(create_separator())

        # Playback controls
        playback_group = QGroupBox("Playback")
        playback_layout = QHBoxLayout(playback_group)

        self.play_btn = QPushButton("Play Video (O)")
        self.play_btn.setEnabled(False)
        self.play_btn.clicked.connect(self._play_video)
        self.play_btn.setMinimumHeight(BUTTON_HEIGHT)
        playback_layout.addWidget(self.play_btn)

        self.play_boom_btn = QPushButton("Play at Boom")
        self.play_boom_btn.setEnabled(False)
        self.play_boom_btn.clicked.connect(self._play_at_boom)
        self.play_boom_btn.setMinimumHeight(BUTTON_HEIGHT)
        playback_layout.addWidget(self.play_boom_btn)

        layout.addWidget(playback_group)

        layout.addWidget(create_separator())

        # Metadata display
        metadata_group = QGroupBox("Metadata")
        self.metadata_label = QLabel("")
        self.metadata_label.setWordWrap(True)
        self.metadata_label.setStyleSheet("font-size: 11px;")
        metadata_layout = QVBoxLayout(metadata_group)
        metadata_layout.addWidget(self.metadata_label)
        layout.addWidget(metadata_group)

        layout.addStretch()

        return panel

    def _create_property_panel(self) -> QWidget:
        """Create Zone C: Property panel with tabs."""
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setSpacing(SPACING)
        layout.setContentsMargins(0, 0, 0, 0)

        # Tab widget
        self.tabs = QTabWidget()
        layout.addWidget(self.tabs)

        # Processing tab
        self.tabs.addTab(self._create_processing_tab(), "Processing")

        # Music tab
        self.tabs.addTab(self._create_music_tab(), "Music")

        # Publishing tab
        self.tabs.addTab(self._create_publishing_tab(), "Publishing")

        # Delete button at bottom (outside tabs)
        layout.addWidget(create_separator())

        self.delete_btn = QPushButton("Delete Video")
        self.delete_btn.setEnabled(False)
        self.delete_btn.clicked.connect(self._delete_video)
        self.delete_btn.setStyleSheet(
            "background: #f44336; color: white; padding: 8px;"
        )
        self.delete_btn.setMinimumHeight(BUTTON_HEIGHT)
        layout.addWidget(self.delete_btn)

        return panel

    def _create_processing_tab(self) -> QWidget:
        """Create the Processing tab content."""
        tab = QWidget()
        layout = QVBoxLayout(tab)
        layout.setSpacing(SPACING)

        # Template selection
        template_group = QGroupBox("Template")
        template_layout = QVBoxLayout(template_group)
        self.template_combo = QComboBox()
        self.template_combo.currentIndexChanged.connect(self._on_template_changed)
        template_layout.addWidget(self.template_combo)
        layout.addWidget(template_group)

        # Motion settings
        motion_group = QGroupBox("Motion")
        motion_form = QFormLayout(motion_group)
        motion_form.setSpacing(8)

        zoom_row = QHBoxLayout()
        self.zoom_start_spin = QDoubleSpinBox()
        self.zoom_start_spin.setRange(0.5, 2.0)
        self.zoom_start_spin.setSingleStep(0.01)
        self.zoom_start_spin.setDecimals(2)
        self.zoom_start_spin.setValue(1.0)
        zoom_row.addWidget(QLabel("Start:"))
        zoom_row.addWidget(self.zoom_start_spin)
        zoom_row.addWidget(QLabel("End:"))
        self.zoom_end_spin = QDoubleSpinBox()
        self.zoom_end_spin.setRange(0.5, 2.0)
        self.zoom_end_spin.setSingleStep(0.01)
        self.zoom_end_spin.setDecimals(2)
        self.zoom_end_spin.setValue(1.08)
        zoom_row.addWidget(self.zoom_end_spin)
        motion_form.addRow("Zoom:", zoom_row)

        layout.addWidget(motion_group)

        # Background settings
        bg_group = QGroupBox("Background (Shorts)")
        bg_form = QFormLayout(bg_group)
        bg_form.setSpacing(8)

        self.blur_spin = QSpinBox()
        self.blur_spin.setRange(5, 100)
        self.blur_spin.setValue(50)
        self.blur_spin.setToolTip("Blur strength for Shorts background")
        bg_form.addRow("Blur:", self.blur_spin)

        self.brightness_spin = QDoubleSpinBox()
        self.brightness_spin.setRange(0.0, 1.0)
        self.brightness_spin.setSingleStep(0.05)
        self.brightness_spin.setDecimals(2)
        self.brightness_spin.setValue(1.0)
        self.brightness_spin.setToolTip("Background brightness (0-1)")
        bg_form.addRow("Brightness:", self.brightness_spin)

        layout.addWidget(bg_group)

        layout.addWidget(create_separator())

        # Process buttons
        self.process_btn = QPushButton("Process Video")
        self.process_btn.setEnabled(False)
        self.process_btn.clicked.connect(self._process_video)
        self.process_btn.setStyleSheet(
            "background: #2196F3; color: white; font-weight: bold;"
        )
        self.process_btn.setMinimumHeight(BUTTON_HEIGHT)
        layout.addWidget(self.process_btn)

        self.process_music_btn = QPushButton("Process + Music")
        self.process_music_btn.setEnabled(False)
        self.process_music_btn.clicked.connect(self._process_with_music)
        self.process_music_btn.setStyleSheet(
            "background: #4CAF50; color: white; font-weight: bold;"
        )
        self.process_music_btn.setMinimumHeight(BUTTON_HEIGHT)
        layout.addWidget(self.process_music_btn)

        layout.addStretch()
        return tab

    def _create_music_tab(self) -> QWidget:
        """Create the Music tab content."""
        tab = QWidget()
        layout = QVBoxLayout(tab)
        layout.setSpacing(SPACING)

        # Track selection
        track_group = QGroupBox("Track Selection")
        track_layout = QVBoxLayout(track_group)

        self.music_combo = QComboBox()
        self.music_combo.setEnabled(False)
        track_layout.addWidget(self.music_combo)

        self.add_music_btn = QPushButton("Add Music Only")
        self.add_music_btn.setEnabled(False)
        self.add_music_btn.clicked.connect(self._add_music)
        self.add_music_btn.setMinimumHeight(BUTTON_HEIGHT)
        track_layout.addWidget(self.add_music_btn)

        layout.addWidget(track_group)

        # Current music info
        info_group = QGroupBox("Current Track")
        info_layout = QVBoxLayout(info_group)
        self.current_music_label = QLabel("None")
        self.current_music_label.setStyleSheet("color: #666;")
        info_layout.addWidget(self.current_music_label)
        layout.addWidget(info_group)

        layout.addStretch()
        return tab

    def _create_publishing_tab(self) -> QWidget:
        """Create the Publishing tab content."""
        tab = QWidget()
        layout = QVBoxLayout(tab)
        layout.setSpacing(SPACING)

        # Upload button
        self.upload_btn = QPushButton("Upload to YouTube")
        self.upload_btn.setEnabled(False)
        self.upload_btn.clicked.connect(self._upload_video)
        self.upload_btn.setStyleSheet(
            "background: #FF0000; color: white; font-weight: bold;"
        )
        self.upload_btn.setMinimumHeight(BUTTON_HEIGHT)
        layout.addWidget(self.upload_btn)

        layout.addWidget(create_separator())

        # Manual upload info
        manual_group = QGroupBox("Manual Upload Info")
        manual_layout = QVBoxLayout(manual_group)

        # Title
        title_row = QHBoxLayout()
        self.title_edit = QLineEdit()
        self.title_edit.setReadOnly(True)
        title_row.addWidget(self.title_edit)
        self.regen_title_btn = QPushButton("↻")
        self.regen_title_btn.setFixedWidth(30)
        self.regen_title_btn.setToolTip("Regenerate title")
        self.regen_title_btn.clicked.connect(lambda: self._regenerate("title"))
        title_row.addWidget(self.regen_title_btn)
        self.copy_title_btn = QPushButton("Copy")
        self.copy_title_btn.setFixedWidth(50)
        self.copy_title_btn.clicked.connect(
            lambda: self._copy_to_clipboard(self.title_edit.text())
        )
        title_row.addWidget(self.copy_title_btn)
        manual_layout.addWidget(QLabel("Title:"))
        manual_layout.addLayout(title_row)

        # Description
        desc_header = QHBoxLayout()
        desc_header.addWidget(QLabel("Description:"))
        desc_header.addStretch()
        self.regen_desc_btn = QPushButton("↻")
        self.regen_desc_btn.setFixedWidth(30)
        self.regen_desc_btn.setToolTip("Regenerate description")
        self.regen_desc_btn.clicked.connect(lambda: self._regenerate("description"))
        desc_header.addWidget(self.regen_desc_btn)
        self.copy_desc_btn = QPushButton("Copy")
        self.copy_desc_btn.setFixedWidth(50)
        self.copy_desc_btn.clicked.connect(
            lambda: self._copy_to_clipboard(self.desc_edit.toPlainText())
        )
        desc_header.addWidget(self.copy_desc_btn)
        manual_layout.addLayout(desc_header)

        self.desc_edit = QTextEdit()
        self.desc_edit.setReadOnly(True)
        self.desc_edit.setMaximumHeight(80)
        manual_layout.addWidget(self.desc_edit)

        # Paths
        path_form = QFormLayout()
        path_form.setSpacing(4)

        rel_row = QHBoxLayout()
        self.path_rel_edit = QLineEdit()
        self.path_rel_edit.setReadOnly(True)
        rel_row.addWidget(self.path_rel_edit)
        self.copy_rel_btn = QPushButton("Copy")
        self.copy_rel_btn.setFixedWidth(50)
        self.copy_rel_btn.clicked.connect(
            lambda: self._copy_to_clipboard(self.path_rel_edit.text())
        )
        rel_row.addWidget(self.copy_rel_btn)
        path_form.addRow("Path (rel):", rel_row)

        abs_row = QHBoxLayout()
        self.path_abs_edit = QLineEdit()
        self.path_abs_edit.setReadOnly(True)
        abs_row.addWidget(self.path_abs_edit)
        self.copy_abs_btn = QPushButton("Copy")
        self.copy_abs_btn.setFixedWidth(50)
        self.copy_abs_btn.clicked.connect(
            lambda: self._copy_to_clipboard(self.path_abs_edit.text())
        )
        abs_row.addWidget(self.copy_abs_btn)
        path_form.addRow("Path (abs):", abs_row)

        manual_layout.addLayout(path_form)
        layout.addWidget(manual_group)

        layout.addStretch()
        return tab

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

        self.batch_label.setText(batch_dir.name)
        self.batch_label.setStyleSheet("font-weight: bold;")
        self.setWindowTitle(f"Batch Annotator - {batch_dir.name}")

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
            return "P"
        elif video.has_music:
            return "M"
        else:
            return "+"

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
        self.save_status.setText("")

        if not video:
            self.video_title.setText("Select a video")
            self.video_stats.setText("")
            self.metadata_label.setText("")
            self.boom_spin.setEnabled(False)
            self.play_btn.setEnabled(False)
            self.play_boom_btn.setEnabled(False)
            self.music_combo.setEnabled(False)
            self.add_music_btn.setEnabled(False)
            self.process_btn.setEnabled(False)
            self.process_music_btn.setEnabled(False)
            self.upload_btn.setEnabled(False)
            self.delete_btn.setEnabled(False)
            self.current_music_label.setText("None")
            self.title_edit.clear()
            self.desc_edit.clear()
            self.path_rel_edit.clear()
            self.path_abs_edit.clear()
            return

        # Update title and stats
        self.video_title.setText(video.name)
        video_file = video.best_video_path.name if video.best_video_path else "None"
        boom_str = f"{video.boom_seconds:.2f}s" if video.boom_seconds else "N/A"
        self.video_stats.setText(
            f"FPS: {video.video_fps} | Duration: {video.duration_seconds:.1f}s | "
            f"Video: {video_file} | Boom: {boom_str}"
        )

        # Update current music
        self.current_music_label.setText(video.music_title if video.music_title else "None")

        # Load full metadata
        try:
            meta = VideoMetadata.from_file(video.metadata_path)
            date_str = meta.created_at.strftime("%Y-%m-%d %H:%M")
            color_preset = f" ({meta.color.preset_name})" if meta.color.preset_name else ""
            pp_preset = f" ({meta.post_process.preset_name})" if meta.post_process.preset_name else ""
            self.metadata_label.setText(
                f"<b>Simulation</b><br>"
                f"Pendulums: {meta.simulation.pendulum_count:,}<br>"
                f"Date: {date_str}<br><br>"
                f"<b>Color</b>{color_preset}<br>"
                f"Scheme: {meta.color.scheme} | Range: {meta.color.start}-{meta.color.end}<br><br>"
                f"<b>Post-Process</b>{pp_preset}<br>"
                f"Tone: {meta.post_process.tone_map} | "
                f"Exp: {meta.post_process.exposure} | "
                f"Contrast: {meta.post_process.contrast} | "
                f"Gamma: {meta.post_process.gamma}"
            )
        except Exception:
            self.metadata_label.setText("")

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

        # Update manual upload info
        if video.best_video_path:
            self.path_abs_edit.setText(str(video.best_video_path.resolve()))
            if self.batch_dir:
                try:
                    self.path_rel_edit.setText(
                        str(video.best_video_path.relative_to(self.batch_dir.parent))
                    )
                except ValueError:
                    self.path_rel_edit.setText(str(video.best_video_path))
            else:
                self.path_rel_edit.setText(str(video.best_video_path))
        else:
            self.path_rel_edit.setText("")
            self.path_abs_edit.setText("")

        try:
            metadata = VideoMetadata.from_file(video.metadata_path)
            self.title_edit.setText(generate_title(metadata))
            self.desc_edit.setText(generate_description(metadata))
        except Exception:
            self.title_edit.setText("")
            self.desc_edit.setText("")

    def _update_boom_label(self) -> None:
        """Update the boom seconds label."""
        if not self.current_video:
            return
        frame = self.boom_spin.value()
        seconds = frame / self.current_video.video_fps
        self.boom_seconds_label.setText(f"({seconds:.2f}s)")

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
                self.title_edit.setText(generate_title(metadata))
                self.statusBar().showMessage("Title regenerated", 1500)
            elif field == "description":
                self.desc_edit.setText(generate_description(metadata))
                self.statusBar().showMessage("Description regenerated", 1500)
        except Exception as e:
            self.statusBar().showMessage(f"Error: {e}", 3000)

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
        self.valid_tracks = self.music_db.get_valid_tracks_for_boom(boom_seconds)

        if not self.valid_tracks:
            self.music_combo.addItem("No valid tracks (boom too late)")
            self.music_combo.setEnabled(False)
            self.add_music_btn.setEnabled(False)
            return

        self.music_combo.addItem(f"Random ({len(self.valid_tracks)} available)")
        for track in self.valid_tracks:
            self.music_combo.addItem(f"{track.title} (drop: {track.drop_time_seconds:.1f}s)")

    def _on_boom_changed(self, _value: int) -> None:
        """Handle boom frame change - trigger autosave."""
        self._update_boom_label()
        self._update_music_combo()

        if self.current_video and self.current_video.has_metadata:
            self.autosave_timer.stop()
            self.autosave_timer.start(500)
            self.save_status.setText("...")
            self.save_status.setStyleSheet("color: #666; font-size: 16px;")

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

            self.current_video.metadata_path.write_text(json.dumps(data, indent=2) + "\n")

            self.current_video.boom_frame = new_boom_frame
            self.current_video.boom_seconds = new_boom_frame / self.current_video.video_fps

            row = self.video_list.currentRow()
            if row >= 0:
                item = self.video_list.item(row)
                status = self._get_video_status(self.current_video)
                item.setText(f"[{status}] {self.current_video.name}")

            self.save_status.setText("✓")
            self.save_status.setStyleSheet("color: #4CAF50; font-size: 16px;")
            QTimer.singleShot(2000, lambda: self.save_status.setText(""))

        except Exception as e:
            self.save_status.setText("✗")
            self.save_status.setStyleSheet("color: #f44336; font-size: 16px;")
            self.statusBar().showMessage(f"Save error: {e}", 3000)

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
            subprocess.Popen([MPV_COMMAND, f"--start={start_seconds:.2f}",
                              str(self.current_video.best_video_path)])
        else:
            QMessageBox.warning(self, "mpv not found", "mpv is not installed or not in PATH")

    def _get_selected_track_id(self) -> Optional[str]:
        """Get the track ID from the combo selection, or None for random."""
        idx = self.music_combo.currentIndex()
        if idx <= 0 or idx > len(self.valid_tracks):
            return None
        return self.valid_tracks[idx - 1].id

    def _get_selected_template(self) -> str:
        """Get the selected template name."""
        text = self.template_combo.currentText()
        if text == "random":
            return "random"
        return text.split(" - ")[0]

    def _on_template_changed(self, _index: int) -> None:
        """Update zoom spinboxes when template changes."""
        if not self.template_lib:
            return

        template_name = self._get_selected_template()
        if template_name == "random":
            self.zoom_start_spin.setValue(1.0)
            self.zoom_end_spin.setValue(1.08)
            return

        try:
            template = self.template_lib.get(template_name)
            if template.motion and template.motion.slow_zoom:
                self.zoom_start_spin.setValue(template.motion.slow_zoom.start)
                self.zoom_end_spin.setValue(template.motion.slow_zoom.end)
            else:
                self.zoom_start_spin.setValue(1.0)
                self.zoom_end_spin.setValue(1.0)
        except KeyError:
            pass

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
            zoom_start = self.zoom_start_spin.value()
            zoom_end = self.zoom_end_spin.value()
            blur = self.blur_spin.value()
            brightness = self.brightness_spin.value()
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
    app.setStyle("Fusion")  # Modern, consistent look

    window = MainWindow(batch_dir)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
