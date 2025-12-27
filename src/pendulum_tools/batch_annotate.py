#!/usr/bin/env python3
"""
Batch video production studio - Main Stage architecture.

Two-pane splitter layout:
- Sidebar (Left): Video list with search filter
- Main Stage (Right): Info-bar + Dual-column workflow (Preparation | Execution)

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
from PyQt6.QtGui import QColor, QFont, QKeySequence, QPalette, QShortcut, QIcon
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
    QSplitter,
    QStyle,
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
SIDEBAR_MIN_WIDTH = 220
SIDEBAR_DEFAULT_WIDTH = 260
BUTTON_HEIGHT = 38
ICON_BUTTON_SIZE = 28
MAIN_MARGINS = 15


class Separator(QFrame):
    """Horizontal separator line."""

    def __init__(self):
        super().__init__()
        self.setFrameShape(QFrame.Shape.HLine)
        self.setFrameShadow(QFrame.Shadow.Sunken)
        self.setStyleSheet("color: #ddd;")


class InfoBadge(QFrame):
    """A styled read-only badge showing key-value info."""

    def __init__(self, label: str, value: str = "—"):
        super().__init__()
        self.setStyleSheet(
            "background: #f0f0f0; border-radius: 4px;"
        )
        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 6, 10, 6)
        layout.setSpacing(6)

        self.lbl_key = QLabel(label)
        self.lbl_key.setStyleSheet("color: #888; font-size: 10px;")
        layout.addWidget(self.lbl_key)

        self.lbl_value = QLabel(value)
        self.lbl_value.setStyleSheet("color: #333; font-weight: bold; font-size: 11px;")
        layout.addWidget(self.lbl_value)

    def set_value(self, value: str) -> None:
        self.lbl_value.setText(value)


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
    pendulum_count: int = 0
    color_scheme: str = ""
    created_at: str = ""
    has_processed: bool = False
    has_music: bool = False
    music_title: Optional[str] = None
    # Processing params (from previous processing)
    processed_template: Optional[str] = None
    processed_zoom_start: Optional[float] = None
    processed_zoom_end: Optional[float] = None
    processed_blur: Optional[int] = None
    processed_brightness: Optional[float] = None


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
    pendulum_count = 0
    color_scheme = ""
    created_at = ""
    processed_template = None
    processed_zoom_start = None
    processed_zoom_end = None
    processed_blur = None
    processed_brightness = None

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

            simulation = data.get("simulation", {})
            pendulum_count = simulation.get("pendulum_count", 0)

            color = data.get("color", {})
            color_scheme = color.get("scheme", "")

            created_at = data.get("created_at", "")[:10]  # YYYY-MM-DD

            # Load processing params if video was processed
            processing = data.get("processing", {})
            processed_template = processing.get("template")
            processed_zoom_start = processing.get("zoom_start")
            processed_zoom_end = processing.get("zoom_end")
            processed_blur = processing.get("blur_strength")
            processed_brightness = processing.get("background_brightness")
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
        pendulum_count=pendulum_count,
        color_scheme=color_scheme,
        created_at=created_at,
        has_processed=(video_dir / "video_processed.mp4").exists(),
        has_music=(
            (video_dir / "video_processed_final.mp4").exists()
            or (video_dir / "video.mp4").exists()
        ),
        music_title=music_title,
        processed_template=processed_template,
        processed_zoom_start=processed_zoom_start,
        processed_zoom_end=processed_zoom_end,
        processed_blur=processed_blur,
        processed_brightness=processed_brightness,
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
    """Video Production Studio - Main Stage architecture."""

    def __init__(self, batch_dir: Optional[Path] = None):
        super().__init__()
        self.setWindowTitle("Video Production Studio")
        self.resize(1280, 820)

        self.settings = QSettings("double-pendulum", "batch-annotate")
        self.batch_dir: Optional[Path] = None
        self.videos: list[VideoInfo] = []
        self.filtered_videos: list[VideoInfo] = []
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
            # Default to random (index 0)
            self.combo_template.setCurrentIndex(0)
        except Exception as e:
            print(f"Warning: Could not load templates: {e}")

        try:
            config = get_config()
            self.music_db = MusicDatabase(config.get_music_dir(None))
        except Exception as e:
            print(f"Warning: Could not load music database: {e}")

    def _init_ui(self) -> None:
        """Initialize the Main Stage layout with QSplitter."""
        central = QWidget()
        self.setCentralWidget(central)

        main_layout = QHBoxLayout(central)
        main_layout.setContentsMargins(MAIN_MARGINS, MAIN_MARGINS, MAIN_MARGINS, MAIN_MARGINS)
        main_layout.setSpacing(0)

        # === QSplitter: Sidebar | Main Stage ===
        self.splitter = QSplitter(Qt.Orientation.Horizontal)
        self.splitter.setHandleWidth(6)

        # Left: Sidebar
        sidebar = self._create_sidebar()
        sidebar.setMinimumWidth(SIDEBAR_MIN_WIDTH)
        self.splitter.addWidget(sidebar)

        # Right: Main Stage
        main_stage = self._create_main_stage()
        self.splitter.addWidget(main_stage)

        # Set initial sizes (sidebar, main stage)
        self.splitter.setSizes([SIDEBAR_DEFAULT_WIDTH, 1000])
        self.splitter.setStretchFactor(0, 0)  # Sidebar doesn't stretch
        self.splitter.setStretchFactor(1, 1)  # Main stage stretches

        main_layout.addWidget(self.splitter)

    def _create_sidebar(self) -> QWidget:
        """Create the left sidebar with search and video list."""
        sidebar = QWidget()
        layout = QVBoxLayout(sidebar)
        layout.setContentsMargins(0, 0, 10, 0)
        layout.setSpacing(8)

        # Open batch button
        self.btn_open = QPushButton("Open Batch...")
        self.btn_open.setMinimumHeight(BUTTON_HEIGHT)
        self.btn_open.clicked.connect(self._open_batch_dialog)
        layout.addWidget(self.btn_open)

        self.lbl_batch = QLabel("No batch loaded")
        self.lbl_batch.setStyleSheet("color: gray; font-style: italic;")
        layout.addWidget(self.lbl_batch)

        layout.addWidget(Separator())

        # Search/filter input
        self.txt_filter = QLineEdit()
        self.txt_filter.setPlaceholderText("Filter videos...")
        self.txt_filter.setClearButtonEnabled(True)
        self.txt_filter.textChanged.connect(self._on_filter_changed)
        layout.addWidget(self.txt_filter)

        # Video list
        self.video_list = QListWidget()
        self.video_list.currentItemChanged.connect(self._on_video_selected)
        layout.addWidget(self.video_list, 1)

        # Navigation row
        nav_row = QHBoxLayout()
        self.btn_prev = QPushButton("◀ Prev")
        self.btn_next = QPushButton("Next ▶")
        self.btn_prev.setMinimumHeight(32)
        self.btn_next.setMinimumHeight(32)
        self.btn_prev.clicked.connect(self._prev_video)
        self.btn_next.clicked.connect(self._next_video)
        nav_row.addWidget(self.btn_prev)
        nav_row.addWidget(self.btn_next)
        layout.addLayout(nav_row)

        return sidebar

    def _create_main_stage(self) -> QWidget:
        """Create the main stage with header, info-bar, and dual-column workflow."""
        stage = QWidget()
        layout = QVBoxLayout(stage)
        layout.setContentsMargins(10, 0, 0, 0)
        layout.setSpacing(10)

        # === HEADER STRIP ===
        self.lbl_title = QLabel("Select a video")
        title_font = QFont()
        title_font.setPointSize(20)
        title_font.setBold(True)
        self.lbl_title.setFont(title_font)
        layout.addWidget(self.lbl_title)

        # === INFO-BAR (Status badges) ===
        info_bar = self._create_info_bar()
        layout.addWidget(info_bar)

        layout.addWidget(Separator())

        # === DUAL-COLUMN WORKFLOW ===
        columns_widget = self._create_dual_columns()
        layout.addWidget(columns_widget, 1)

        return stage

    def _create_info_bar(self) -> QFrame:
        """Create the horizontal info-bar with status badges."""
        bar = QFrame()
        bar.setStyleSheet("background: #f5f5f5; border-radius: 6px;")
        layout = QHBoxLayout(bar)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(8)

        # Use short labels to avoid cramping
        self.badge_pendulums = InfoBadge("N")
        self.badge_pendulums.setToolTip("Pendulum count")
        layout.addWidget(self.badge_pendulums)

        self.badge_scheme = InfoBadge("Color")
        layout.addWidget(self.badge_scheme)

        self.badge_date = InfoBadge("Date")
        layout.addWidget(self.badge_date)

        self.badge_fps = InfoBadge("FPS")
        layout.addWidget(self.badge_fps)

        self.badge_duration = InfoBadge("Len")
        self.badge_duration.setToolTip("Video duration")
        layout.addWidget(self.badge_duration)

        self.badge_status = InfoBadge("State")
        layout.addWidget(self.badge_status)

        layout.addStretch()

        # Autosave indicator
        self.lbl_autosave = QLabel("")
        self.lbl_autosave.setStyleSheet("font-size: 16px;")
        self.lbl_autosave.setFixedWidth(24)
        layout.addWidget(self.lbl_autosave)

        return bar

    def _create_dual_columns(self) -> QWidget:
        """Create the dual-column workflow: Preparation | Execution."""
        widget = QWidget()
        layout = QHBoxLayout(widget)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(20)

        # === COLUMN A: PREPARATION ===
        col_a = self._create_preparation_column()
        layout.addWidget(col_a, 1)

        # Vertical separator
        vsep = QFrame()
        vsep.setFrameShape(QFrame.Shape.VLine)
        vsep.setStyleSheet("color: #ddd;")
        layout.addWidget(vsep)

        # === COLUMN B: EXECUTION ===
        col_b = self._create_execution_column()
        layout.addWidget(col_b, 1)

        return widget

    def _create_preparation_column(self) -> QWidget:
        """Column A: Annotation + Soundtrack + Config."""
        col = QWidget()
        layout = QVBoxLayout(col)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)

        # === ANNOTATION & TIMING ===
        group_annot = QGroupBox("Annotation && Timing")
        annot_form = QFormLayout(group_annot)
        annot_form.setSpacing(10)

        # Boom frame row
        boom_widget = QWidget()
        boom_layout = QHBoxLayout(boom_widget)
        boom_layout.setContentsMargins(0, 0, 0, 0)
        boom_layout.setSpacing(8)

        self.spin_boom = QSpinBox()
        self.spin_boom.setRange(0, 100000)
        self.spin_boom.setFixedWidth(100)
        boom_font = QFont()
        boom_font.setPointSize(14)
        boom_font.setBold(True)
        self.spin_boom.setFont(boom_font)
        self.spin_boom.setMinimumHeight(36)
        self.spin_boom.valueChanged.connect(self._on_boom_changed)
        boom_layout.addWidget(self.spin_boom)

        self.lbl_boom_secs = QLabel("(0.00s)")
        self.lbl_boom_secs.setStyleSheet("color: #1976d2; font-weight: bold;")
        boom_layout.addWidget(self.lbl_boom_secs)
        boom_layout.addStretch()

        annot_form.addRow("Boom Frame:", boom_widget)

        # Playback buttons
        play_widget = QWidget()
        play_layout = QHBoxLayout(play_widget)
        play_layout.setContentsMargins(0, 0, 0, 0)
        play_layout.setSpacing(8)

        self.btn_play = QPushButton("▶ Play")
        self.btn_play.setMinimumHeight(BUTTON_HEIGHT)
        self.btn_play.clicked.connect(self._play_video)
        self.btn_play.setEnabled(False)
        play_layout.addWidget(self.btn_play)

        self.btn_play_boom = QPushButton("▶ At Boom")
        self.btn_play_boom.setMinimumHeight(BUTTON_HEIGHT)
        self.btn_play_boom.clicked.connect(self._play_at_boom)
        self.btn_play_boom.setEnabled(False)
        play_layout.addWidget(self.btn_play_boom)

        annot_form.addRow("", play_widget)

        layout.addWidget(group_annot)

        # === SOUNDTRACK ===
        group_music = QGroupBox("Soundtrack")
        music_form = QFormLayout(group_music)
        music_form.setSpacing(10)

        self.combo_music = QComboBox()
        self.combo_music.setEnabled(False)
        music_form.addRow("Track:", self.combo_music)

        # Music buttons
        music_btns = QWidget()
        music_btns_layout = QHBoxLayout(music_btns)
        music_btns_layout.setContentsMargins(0, 0, 0, 0)
        music_btns_layout.setSpacing(8)

        self.btn_add_music = QPushButton("Add Music")
        self.btn_add_music.setMinimumHeight(BUTTON_HEIGHT)
        self.btn_add_music.clicked.connect(self._add_music)
        self.btn_add_music.setEnabled(False)
        music_btns_layout.addWidget(self.btn_add_music)

        self.lbl_current_music = QLabel("")
        self.lbl_current_music.setStyleSheet("color: #666; font-size: 11px;")
        music_btns_layout.addWidget(self.lbl_current_music, 1)

        music_form.addRow("", music_btns)

        layout.addWidget(group_music)

        # === CONFIG (Expandable) ===
        group_config = QGroupBox("Config")
        config_form = QFormLayout(group_config)
        config_form.setSpacing(8)

        # Template
        self.combo_template = QComboBox()
        self.combo_template.currentIndexChanged.connect(self._on_template_changed)
        config_form.addRow("Template:", self.combo_template)

        # Previously used label
        self.lbl_prev_config = QLabel("")
        self.lbl_prev_config.setStyleSheet("color: #666; font-size: 10px; font-style: italic;")
        config_form.addRow("", self.lbl_prev_config)

        # Zoom
        zoom_widget = QWidget()
        zoom_layout = QHBoxLayout(zoom_widget)
        zoom_layout.setContentsMargins(0, 0, 0, 0)
        zoom_layout.setSpacing(8)

        self.spin_zoom_start = QDoubleSpinBox()
        self.spin_zoom_start.setRange(0.5, 2.0)
        self.spin_zoom_start.setSingleStep(0.01)
        self.spin_zoom_start.setDecimals(2)
        self.spin_zoom_start.setValue(1.0)
        zoom_layout.addWidget(QLabel("Start:"))
        zoom_layout.addWidget(self.spin_zoom_start)

        self.spin_zoom_end = QDoubleSpinBox()
        self.spin_zoom_end.setRange(0.5, 2.0)
        self.spin_zoom_end.setSingleStep(0.01)
        self.spin_zoom_end.setDecimals(2)
        self.spin_zoom_end.setValue(1.08)
        zoom_layout.addWidget(QLabel("End:"))
        zoom_layout.addWidget(self.spin_zoom_end)
        zoom_layout.addStretch()

        config_form.addRow("Zoom:", zoom_widget)

        # Blur & Brightness
        bg_widget = QWidget()
        bg_layout = QHBoxLayout(bg_widget)
        bg_layout.setContentsMargins(0, 0, 0, 0)
        bg_layout.setSpacing(8)

        self.spin_blur = QSpinBox()
        self.spin_blur.setRange(5, 100)
        self.spin_blur.setValue(50)
        bg_layout.addWidget(QLabel("Blur:"))
        bg_layout.addWidget(self.spin_blur)

        self.spin_brightness = QDoubleSpinBox()
        self.spin_brightness.setRange(0.0, 1.0)
        self.spin_brightness.setSingleStep(0.05)
        self.spin_brightness.setDecimals(2)
        self.spin_brightness.setValue(1.0)
        bg_layout.addWidget(QLabel("Bright:"))
        bg_layout.addWidget(self.spin_brightness)
        bg_layout.addStretch()

        config_form.addRow("Background:", bg_widget)

        layout.addWidget(group_config)

        layout.addStretch()

        return col

    def _create_execution_column(self) -> QWidget:
        """Column B: Rendering + Publishing."""
        col = QWidget()
        layout = QVBoxLayout(col)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)

        # === RENDERING ===
        group_render = QGroupBox("Rendering")
        render_layout = QHBoxLayout(group_render)
        render_layout.setSpacing(10)

        self.btn_process = QPushButton("Process FX")
        self.btn_process.setMinimumHeight(BUTTON_HEIGHT + 4)
        self.btn_process.setStyleSheet(
            "background: #2196F3; color: white; font-weight: bold;"
        )
        self.btn_process.clicked.connect(self._process_video)
        self.btn_process.setEnabled(False)
        render_layout.addWidget(self.btn_process)

        self.btn_process_music = QPushButton("Process + Music")
        self.btn_process_music.setMinimumHeight(BUTTON_HEIGHT + 4)
        self.btn_process_music.setStyleSheet(
            "background: #4CAF50; color: white; font-weight: bold;"
        )
        self.btn_process_music.clicked.connect(self._process_with_music)
        self.btn_process_music.setEnabled(False)
        render_layout.addWidget(self.btn_process_music)

        layout.addWidget(group_render)

        # === PUBLISHING ===
        group_pub = QGroupBox("Publishing")
        pub_layout = QVBoxLayout(group_pub)
        pub_layout.setSpacing(4)
        pub_layout.setContentsMargins(8, 8, 8, 8)

        # Header row with regenerate button
        header_row = QHBoxLayout()
        header_row.addStretch()
        self.btn_regen = QPushButton("↻ Regenerate")
        self.btn_regen.setToolTip("Regenerate title and description")
        self.btn_regen.clicked.connect(self._regenerate_all)
        header_row.addWidget(self.btn_regen)
        pub_layout.addLayout(header_row)

        # Title row
        title_row = QHBoxLayout()
        title_row.setSpacing(4)
        title_row.addWidget(QLabel("Title:"))
        self.edit_title = QLineEdit()
        self.edit_title.setReadOnly(True)
        title_row.addWidget(self.edit_title, 1)
        self.btn_copy_title = QPushButton("⧉")
        self.btn_copy_title.setFixedSize(ICON_BUTTON_SIZE, ICON_BUTTON_SIZE)
        self.btn_copy_title.setToolTip("Copy title")
        self.btn_copy_title.clicked.connect(
            lambda: self._copy_to_clipboard(self.edit_title.text())
        )
        title_row.addWidget(self.btn_copy_title)
        pub_layout.addLayout(title_row)

        # Description row
        desc_row = QHBoxLayout()
        desc_row.setSpacing(4)
        desc_row.addWidget(QLabel("Desc:"), 0, Qt.AlignmentFlag.AlignTop)
        self.edit_desc = QTextEdit()
        self.edit_desc.setReadOnly(True)
        self.edit_desc.setMinimumHeight(90)
        desc_row.addWidget(self.edit_desc, 1)
        self.btn_copy_desc = QPushButton("⧉")
        self.btn_copy_desc.setFixedSize(ICON_BUTTON_SIZE, ICON_BUTTON_SIZE)
        self.btn_copy_desc.setToolTip("Copy description")
        self.btn_copy_desc.clicked.connect(
            lambda: self._copy_to_clipboard(self.edit_desc.toPlainText())
        )
        desc_row.addWidget(self.btn_copy_desc, 0, Qt.AlignmentFlag.AlignTop)
        pub_layout.addLayout(desc_row)

        # Path row
        path_row = QHBoxLayout()
        path_row.setSpacing(4)
        path_row.addWidget(QLabel("Path:"))
        self.edit_path = QLineEdit()
        self.edit_path.setReadOnly(True)
        self.edit_path.setStyleSheet("font-size: 10px;")
        path_row.addWidget(self.edit_path, 1)
        self.btn_copy_path = QPushButton("⧉")
        self.btn_copy_path.setFixedSize(ICON_BUTTON_SIZE, ICON_BUTTON_SIZE)
        self.btn_copy_path.setToolTip("Copy path")
        self.btn_copy_path.clicked.connect(
            lambda: self._copy_to_clipboard(self.edit_path.text())
        )
        path_row.addWidget(self.btn_copy_path)
        pub_layout.addLayout(path_row)

        # Upload button
        self.btn_upload = QPushButton("Upload to YouTube")
        self.btn_upload.setMinimumHeight(BUTTON_HEIGHT + 4)
        self.btn_upload.setStyleSheet(
            "background: #FF0000; color: white; font-weight: bold;"
        )
        self.btn_upload.clicked.connect(self._upload_video)
        self.btn_upload.setEnabled(False)
        pub_layout.addWidget(self.btn_upload)

        layout.addWidget(group_pub)

        layout.addStretch()

        # Delete button at bottom
        self.btn_delete = QPushButton("Delete")
        self.btn_delete.setMinimumHeight(28)
        self.btn_delete.clicked.connect(self._delete_video)
        self.btn_delete.setEnabled(False)
        layout.addWidget(self.btn_delete)

        return col

    def _setup_shortcuts(self) -> None:
        """Set up keyboard shortcuts."""
        QShortcut(QKeySequence("N"), self).activated.connect(self._next_video)
        QShortcut(QKeySequence("P"), self).activated.connect(self._prev_video)
        QShortcut(QKeySequence("O"), self).activated.connect(self._play_video)
        QShortcut(QKeySequence("Ctrl+O"), self).activated.connect(self._open_batch_dialog)
        QShortcut(QKeySequence("Ctrl+F"), self).activated.connect(
            lambda: self.txt_filter.setFocus()
        )

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
        self.filtered_videos = self.videos.copy()

        self.settings.setValue("last_batch", str(batch_dir))

        self.lbl_batch.setText(f"{batch_dir.name} ({len(self.videos)})")
        self.lbl_batch.setStyleSheet("font-weight: bold;")
        self.setWindowTitle(f"Video Production Studio - {batch_dir.name}")

        self.txt_filter.clear()
        self._refresh_video_list()

        if self.filtered_videos:
            self.video_list.setCurrentRow(0)

    def _on_filter_changed(self, text: str) -> None:
        """Filter video list based on search text."""
        text_lower = text.lower().strip()
        if not text_lower:
            self.filtered_videos = self.videos.copy()
        else:
            self.filtered_videos = [
                v for v in self.videos
                if text_lower in v.name.lower()
            ]
        self._refresh_video_list()

    def _refresh_video_list(self) -> None:
        """Refresh the video list widget."""
        self.video_list.clear()
        for video in self.filtered_videos:
            item = QListWidgetItem()
            item.setData(Qt.ItemDataRole.UserRole, video)

            # Status icon via standard icons
            icon = self._get_status_icon(video)
            if icon:
                item.setIcon(icon)

            item.setText(video.name)
            self.video_list.addItem(item)

    def _get_status_icon(self, video: VideoInfo) -> Optional[QIcon]:
        """Get status icon for video using system icons."""
        style = self.style()
        if video.boom_frame is None:
            return style.standardIcon(QStyle.StandardPixmap.SP_MessageBoxQuestion)
        elif video.has_processed:
            return style.standardIcon(QStyle.StandardPixmap.SP_DialogApplyButton)
        elif video.has_music:
            return style.standardIcon(QStyle.StandardPixmap.SP_MediaVolume)
        else:
            return style.standardIcon(QStyle.StandardPixmap.SP_FileIcon)

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
        self.badge_pendulums.set_value("—")
        self.badge_scheme.set_value("—")
        self.badge_date.set_value("—")
        self.badge_fps.set_value("—")
        self.badge_duration.set_value("—")
        self.badge_status.set_value("—")
        self.lbl_autosave.setText("")

        self.spin_boom.setValue(0)
        self.lbl_boom_secs.setText("(0.00s)")
        self.combo_music.clear()
        self.lbl_current_music.setText("")
        self.edit_title.clear()
        self.edit_desc.clear()
        self.edit_path.clear()
        self.lbl_prev_config.setText("")

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

        # Info-bar badges
        self.badge_pendulums.set_value(f"{video.pendulum_count:,}" if video.pendulum_count else "—")
        self.badge_scheme.set_value(video.color_scheme or "—")
        self.badge_date.set_value(video.created_at or "—")
        self.badge_fps.set_value(str(video.video_fps))
        self.badge_duration.set_value(f"{video.duration_seconds:.1f}s")

        status = "Processed" if video.has_processed else ("Music" if video.has_music else "Raw")
        self.badge_status.set_value(status)

        # Boom spinner
        self.spin_boom.blockSignals(True)
        self.spin_boom.setMaximum(int(video.duration_seconds * video.video_fps))
        self.spin_boom.setValue(video.boom_frame or 0)
        self.spin_boom.blockSignals(False)
        self._update_boom_label()

        # Current music
        self.lbl_current_music.setText(
            f"♪ {video.music_title}" if video.music_title else ""
        )

        # Update config based on previous processing or template
        self._update_config_from_video(video)

        # Publishing info
        if video.best_video_path:
            self.edit_path.setText(str(video.best_video_path.resolve()))
        else:
            self.edit_path.clear()

        try:
            metadata = VideoMetadata.from_file(video.metadata_path)
            self.edit_title.setText(generate_title(metadata))
            self.edit_desc.setText(generate_description(metadata))
        except Exception:
            self.edit_title.clear()
            self.edit_desc.clear()

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

            # Update list item icon
            row = self.video_list.currentRow()
            if row >= 0:
                item = self.video_list.item(row)
                icon = self._get_status_icon(self.current_video)
                if icon:
                    item.setIcon(icon)

            self.lbl_autosave.setText("✓")
            self.lbl_autosave.setStyleSheet("color: #4CAF50; font-size: 16px;")
            QTimer.singleShot(2000, lambda: self.lbl_autosave.setText(""))

        except Exception as e:
            self.lbl_autosave.setText("✗")
            self.lbl_autosave.setStyleSheet("color: #f44336; font-size: 16px;")
            self.statusBar().showMessage(f"Save error: {e}", 3000)

    def _update_config_from_video(self, video: VideoInfo) -> None:
        """Update config widgets based on video's previous processing or defaults."""
        if video.has_processed and video.processed_template:
            # Show what was previously used
            prev_parts = [f"Last: {video.processed_template}"]
            if video.processed_zoom_start is not None:
                prev_parts.append(f"zoom {video.processed_zoom_start:.2f}→{video.processed_zoom_end:.2f}")
            if video.processed_blur is not None:
                prev_parts.append(f"blur {video.processed_blur}")
            self.lbl_prev_config.setText(" | ".join(prev_parts))

            # Populate spinboxes with previous values
            if video.processed_zoom_start is not None:
                self.spin_zoom_start.setValue(video.processed_zoom_start)
            if video.processed_zoom_end is not None:
                self.spin_zoom_end.setValue(video.processed_zoom_end)
            if video.processed_blur is not None:
                self.spin_blur.setValue(video.processed_blur)
            if video.processed_brightness is not None:
                self.spin_brightness.setValue(video.processed_brightness)

            # Try to select the previously used template
            for i in range(self.combo_template.count()):
                if self.combo_template.itemText(i).startswith(video.processed_template):
                    self.combo_template.blockSignals(True)
                    self.combo_template.setCurrentIndex(i)
                    self.combo_template.blockSignals(False)
                    break
        else:
            self.lbl_prev_config.setText("")
            # Reset to defaults from current template selection
            self._on_template_changed(self.combo_template.currentIndex())

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

    def _regenerate_all(self) -> None:
        """Regenerate title and description from templates."""
        if not self.current_video or not self.current_video.has_metadata:
            return
        try:
            metadata = VideoMetadata.from_file(self.current_video.metadata_path)
            self.edit_title.setText(generate_title(metadata))
            self.edit_desc.setText(generate_description(metadata))
            self.statusBar().showMessage("Regenerated", 1500)
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
                        icon = self._get_status_icon(new_info)
                        if icon:
                            item.setIcon(icon)
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
            "Delete",
            f"Delete {self.current_video.name}?",
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
            self.filtered_videos = [v for v in self.filtered_videos if v.name != self.current_video.name]

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
    palette.setColor(QPalette.ColorRole.Window, QColor(250, 250, 250))
    app.setPalette(palette)

    window = MainWindow(batch_dir)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
