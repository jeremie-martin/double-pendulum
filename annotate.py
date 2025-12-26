#!/usr/bin/env python3
"""
Minimal run/video annotation tool (PyQt6).

Features:
- Scan run_* directories
- Load/save annotations.json (version >= 2) compatible with the C++ optimize tool:
  {
    "version": 2,
    "annotations": [
      {"id": "...", "data_path": ".../simulation_data.bin", "targets": {...}, "notes": "..."}
    ]
  }
- Stores UI target definitions in root["target_defs"] (ignored by C++).
- Launch mpv for run video.mp4 (optionally seek to a target frame using simulation_data.bin header).
- No default annotations; targets can be missing per run.

Target types:
- "frame"  : stored as integer (but written as number in JSON)
- "score"  : UI shows 0..100, stored 0..1
- "number" : raw numeric value (stored as-is)

Usage:
  python annotate_runs.py --root output/eval2 --annotations output/eval2/annotations.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from PyQt6.QtCore import (
    QAbstractTableModel,
    QItemSelectionModel,
    QModelIndex,
    QObject,
    Qt,
    QSettings,
    QSortFilterProxyModel,
    QTimer,
    QUrl,
)
from PyQt6.QtGui import QAction, QDesktopServices, QDoubleValidator, QIcon, QIntValidator, QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSpacerItem,
    QSplitter,
    QTableView,
    QTextEdit,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

MPV_COMMAND = "mpv"  # must be in PATH; otherwise edit this.


# ---------------------------
# Data model
# ---------------------------

RUN_RE = re.compile(r"^run_\d{8}_\d{6}$")


@dataclass(frozen=True)
class RunInfo:
    run_id: str
    run_dir: Path
    video_path: Path
    data_path: Path
    has_video: bool
    has_data: bool


def read_sim_header_frame_duration(sim_bin: Path) -> Optional[Tuple[int, float]]:
    """
    Parse simulation_data::Header (v2) partial fields to get frame_count and duration_seconds.
    C++ header begins:
      char magic[8]
      uint32 format_version
      uint32 pendulum_count
      uint32 frame_count
      double duration_seconds
      double max_dt
      ...
    We only need frame_count and duration_seconds.
    """
    try:
        with sim_bin.open("rb") as f:
            head = f.read(8 + 4 + 4 + 4 + 8 + 8)  # 36 bytes
        if len(head) < 36:
            return None
        magic, fmt_ver, pend_cnt, frame_cnt, duration_s, max_dt = struct.unpack("<8sIII2d", head)
        if not (magic[:4] == b"PNDL"):
            return None
        if frame_cnt <= 0 or duration_s <= 0:
            return None
        frame_duration = float(duration_s) / float(frame_cnt)
        return int(frame_cnt), frame_duration
    except Exception:
        return None


def atomic_write_json(path: Path, obj: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + f".tmp.{os.getpid()}")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, sort_keys=False)
        f.write("\n")
    tmp.replace(path)


# ---------------------------
# Annotation store
# ---------------------------

TargetDef = Dict[str, str]  # name -> type ("frame"|"score"|"number")


class AnnotationStore(QObject):
    def __init__(self, root_dir: Path, annotations_path: Path):
        super().__init__()
        self.root_dir = root_dir
        self.annotations_path = annotations_path

        self.version: int = 2
        self.target_defs: TargetDef = {}  # no defaults
        # run_id -> annotation entry
        self.by_id: Dict[str, Dict[str, Any]] = {}

        self.dirty: bool = False

    def load(self) -> None:
        self.by_id.clear()
        self.target_defs = {}
        self.version = 2

        if not self.annotations_path.exists():
            self.dirty = False
            return

        try:
            data = json.loads(self.annotations_path.read_text(encoding="utf-8"))
        except Exception as e:
            raise RuntimeError(f"Failed to parse JSON: {e}")

        self.version = int(data.get("version", 1))
        # UI-only schema storage (ignored by C++)
        td = data.get("target_defs", {})
        if isinstance(td, dict):
            # only accept string->string
            for k, v in td.items():
                if isinstance(k, str) and isinstance(v, str):
                    if v in ("frame", "score", "number"):
                        self.target_defs[k] = v

        anns = data.get("annotations", [])
        if isinstance(anns, list):
            for obj in anns:
                if not isinstance(obj, dict):
                    continue
                rid = obj.get("id")
                if not isinstance(rid, str) or not rid:
                    continue
                # Normalize
                entry: Dict[str, Any] = {
                    "id": rid,
                    "data_path": obj.get("data_path", ""),
                    "notes": obj.get("notes", ""),
                }
                # v2 targets map
                targets = obj.get("targets", {})
                if isinstance(targets, dict):
                    entry["targets"] = dict(targets)
                else:
                    entry["targets"] = {}

                # Backward compat: boom_frame/peak_frame into targets if present
                for legacy in ("boom_frame", "peak_frame"):
                    if legacy in obj and isinstance(obj[legacy], (int, float)):
                        if legacy not in entry["targets"]:
                            entry["targets"][legacy] = obj[legacy]

                self.by_id[rid] = entry

        # If file contains targets not in target_defs, keep them visible as "number"
        for rid, entry in self.by_id.items():
            t = entry.get("targets", {})
            if isinstance(t, dict):
                for name in t.keys():
                    if name not in self.target_defs:
                        self.target_defs[name] = "number"

        self.dirty = False

    def to_json(self, include_unannotated: bool, all_runs: List[RunInfo]) -> Dict[str, Any]:
        out: Dict[str, Any] = {
            "version": 2,
            "target_defs": dict(self.target_defs),  # ignored by C++ loader
            "annotations": [],
        }

        # Only write entries that matter unless include_unannotated
        def entry_is_meaningful(e: Dict[str, Any]) -> bool:
            notes = e.get("notes", "")
            targets = e.get("targets", {})
            has_notes = isinstance(notes, str) and notes.strip() != ""
            has_targets = isinstance(targets, dict) and len(targets) > 0
            return has_notes or has_targets

        run_ids = [r.run_id for r in all_runs] if include_unannotated else list(self.by_id.keys())
        run_ids = sorted(set(run_ids))

        for rid in run_ids:
            e = self.by_id.get(rid)
            if e is None:
                # create an empty stub if requested
                if include_unannotated:
                    e = {
                        "id": rid,
                        "data_path": self.default_data_path_for_run(rid),
                        "notes": "",
                        "targets": {},
                    }
                else:
                    continue

            if not include_unannotated and not entry_is_meaningful(e):
                continue

            out["annotations"].append(
                {
                    "id": rid,
                    "data_path": e.get("data_path", self.default_data_path_for_run(rid)),
                    "targets": e.get("targets", {}),
                    "notes": e.get("notes", ""),
                }
            )
        return out

    def save(self, include_unannotated: bool, all_runs: List[RunInfo]) -> None:
        obj = self.to_json(include_unannotated=include_unannotated, all_runs=all_runs)
        atomic_write_json(self.annotations_path, obj)
        self.dirty = False

    def default_data_path_for_run(self, run_id: str) -> str:
        # Store as a normal path string; keep it stable & portable.
        # Example: output/eval2/run_.../simulation_data.bin
        return (self.root_dir / run_id / "simulation_data.bin").as_posix()

    def ensure_entry(self, run: RunInfo) -> Dict[str, Any]:
        if run.run_id not in self.by_id:
            self.by_id[run.run_id] = {
                "id": run.run_id,
                "data_path": run.data_path.as_posix(),
                "targets": {},
                "notes": "",
            }
            self.dirty = True
        else:
            # keep data_path updated if empty
            if not self.by_id[run.run_id].get("data_path"):
                self.by_id[run.run_id]["data_path"] = run.data_path.as_posix()
        return self.by_id[run.run_id]

    def set_note(self, run: RunInfo, note: str) -> None:
        e = self.ensure_entry(run)
        if e.get("notes", "") != note:
            e["notes"] = note
            self.dirty = True

    def get_note(self, run: RunInfo) -> str:
        e = self.by_id.get(run.run_id)
        if not e:
            return ""
        n = e.get("notes", "")
        return n if isinstance(n, str) else ""

    def set_target_value(self, run: RunInfo, name: str, value: Optional[float]) -> None:
        e = self.ensure_entry(run)
        targets = e.get("targets")
        if not isinstance(targets, dict):
            targets = {}
            e["targets"] = targets

        if value is None:
            if name in targets:
                del targets[name]
                self.dirty = True
            return

        # store numeric
        prev = targets.get(name)
        if prev != value:
            targets[name] = value
            self.dirty = True

    def get_target_value(self, run: RunInfo, name: str) -> Optional[float]:
        e = self.by_id.get(run.run_id)
        if not e:
            return None
        targets = e.get("targets")
        if not isinstance(targets, dict):
            return None
        v = targets.get(name)
        if isinstance(v, (int, float)):
            return float(v)
        return None

    def target_completion(self, run: RunInfo) -> Tuple[int, int]:
        total = len(self.target_defs)
        if total == 0:
            return 0, 0
        filled = 0
        for name in self.target_defs.keys():
            if self.get_target_value(run, name) is not None:
                filled += 1
        return filled, total


# ---------------------------
# Runs scanning
# ---------------------------

def scan_runs(root_dir: Path) -> List[RunInfo]:
    runs: List[RunInfo] = []
    if not root_dir.exists():
        return runs

    for p in sorted(root_dir.iterdir()):
        if not p.is_dir():
            continue
        if not RUN_RE.match(p.name):
            continue
        vid = p / "video.mp4"
        dat = p / "simulation_data.bin"
        runs.append(
            RunInfo(
                run_id=p.name,
                run_dir=p,
                video_path=vid,
                data_path=dat,
                has_video=vid.exists(),
                has_data=dat.exists(),
            )
        )
    return runs


# ---------------------------
# Qt model for runs table
# ---------------------------

class RunsTableModel(QAbstractTableModel):
    COLS = ["Run", "Video", "Data", "Annotated"]

    def __init__(self, runs: List[RunInfo], store: AnnotationStore):
        super().__init__()
        self.runs = runs
        self.store = store

    def rowCount(self, parent: QModelIndex = QModelIndex()) -> int:
        return len(self.runs)

    def columnCount(self, parent: QModelIndex = QModelIndex()) -> int:
        return len(self.COLS)

    def headerData(self, section: int, orientation: Qt.Orientation, role: int = Qt.ItemDataRole.DisplayRole):
        if role != Qt.ItemDataRole.DisplayRole:
            return None
        if orientation == Qt.Orientation.Horizontal:
            return self.COLS[section]
        return str(section + 1)

    def data(self, index: QModelIndex, role: int = Qt.ItemDataRole.DisplayRole):
        if not index.isValid():
            return None
        run = self.runs[index.row()]
        col = index.column()

        if role == Qt.ItemDataRole.DisplayRole:
            if col == 0:
                return run.run_id
            if col == 1:
                return "yes" if run.has_video else "no"
            if col == 2:
                return "yes" if run.has_data else "no"
            if col == 3:
                e = self.store.by_id.get(run.run_id)
                if not e:
                    if len(self.store.target_defs) > 0:
                        return f"0/{len(self.store.target_defs)}"
                    return ""
                targets = e.get("targets", {})
                notes = e.get("notes", "")
                filled, total = self.store.target_completion(run)
                n = "notes" if isinstance(notes, str) and notes.strip() else ""
                if total > 0:
                    base = f"{filled}/{total}"
                    return f"{base} + notes" if n else base
                return "notes" if n else ""

        if role == Qt.ItemDataRole.TextAlignmentRole:
            if col in (1, 2, 3):
                return Qt.AlignmentFlag.AlignCenter

        return None

    def refresh_row(self, run_id: str) -> None:
        for i, r in enumerate(self.runs):
            if r.run_id == run_id:
                top_left = self.index(i, 0)
                bottom_right = self.index(i, self.columnCount() - 1)
                self.dataChanged.emit(top_left, bottom_right, [])
                return

    def refresh_all(self) -> None:
        if not self.runs:
            return
        self.dataChanged.emit(
            self.index(0, 0),
            self.index(len(self.runs) - 1, self.columnCount() - 1),
            []
        )


class RunsFilterProxy(QSortFilterProxyModel):
    def __init__(self):
        super().__init__()
        self.setFilterCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.setFilterKeyColumn(0)  # Run column

    def set_text_filter(self, text: str) -> None:
        self.setFilterFixedString(text)


# ---------------------------
# Target management dialog
# ---------------------------

class TargetEditorDialog(QDialog):
    def __init__(self, parent: QWidget, target_defs: TargetDef):
        super().__init__(parent)
        self.setWindowTitle("Targets")
        self.setModal(True)
        self.resize(520, 360)

        self._defs = dict(target_defs)

        layout = QVBoxLayout(self)

        info = QLabel(
            "Targets are user-defined and optional per run.\n"
            "Type:\n"
            "  - frame  : integer frame index\n"
            "  - score  : UI 0..100, stored 0..1\n"
            "  - number : raw numeric\n"
        )
        info.setWordWrap(True)
        layout.addWidget(info)

        form = QFormLayout()
        self.name_edit = QLineEdit()
        self.type_combo = QComboBox()
        self.type_combo.addItems(["frame", "score", "number"])
        form.addRow("Name", self.name_edit)
        form.addRow("Type", self.type_combo)
        layout.addLayout(form)

        add_row = QHBoxLayout()
        self.add_btn = QPushButton("Add / Update")
        self.rm_btn = QPushButton("Remove")
        add_row.addWidget(self.add_btn)
        add_row.addWidget(self.rm_btn)
        add_row.addStretch(1)
        layout.addLayout(add_row)

        self.list_box = QTextEdit()
        self.list_box.setReadOnly(True)
        layout.addWidget(self.list_box, 1)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        self.add_btn.clicked.connect(self._on_add)
        self.rm_btn.clicked.connect(self._on_remove)

        self._refresh_view()

    def _refresh_view(self) -> None:
        lines = []
        for name in sorted(self._defs.keys()):
            lines.append(f"{name}: {self._defs[name]}")
        self.list_box.setPlainText("\n".join(lines))

    def _on_add(self) -> None:
        name = self.name_edit.text().strip()
        if not name:
            return
        t = self.type_combo.currentText().strip()
        if t not in ("frame", "score", "number"):
            return
        self._defs[name] = t
        self._refresh_view()

    def _on_remove(self) -> None:
        name = self.name_edit.text().strip()
        if not name:
            return
        if name in self._defs:
            del self._defs[name]
            self._refresh_view()

    def target_defs(self) -> TargetDef:
        return dict(self._defs)


# ---------------------------
# Target input widget row
# ---------------------------

class TargetRow(QWidget):
    def __init__(self, name: str, target_type: str, parent: QWidget):
        super().__init__(parent)
        self.name = name
        self.target_type = target_type

        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)

        self.label = QLabel(name)
        self.label.setMinimumWidth(160)
        self.label.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Preferred)
        row.addWidget(self.label)

        self.edit = QLineEdit()
        self.edit.setPlaceholderText("blank = unannotated")

        if target_type == "frame":
            self.edit.setValidator(QIntValidator(-1_000_000, 1_000_000))
            self.edit.setToolTip("Frame index (integer). Leave blank to remove.")
        elif target_type == "score":
            self.edit.setValidator(QDoubleValidator(0.0, 100.0, 3))
            self.edit.setToolTip("Score 0..100 in UI; stored as 0..1. Leave blank to remove.")
        else:
            self.edit.setValidator(QDoubleValidator(-1e18, 1e18, 6))
            self.edit.setToolTip("Raw numeric value. Leave blank to remove.")

        row.addWidget(self.edit, 1)

        self.clear_btn = QToolButton()
        self.clear_btn.setText("×")
        self.clear_btn.setToolTip("Clear")
        self.clear_btn.setAutoRaise(True)
        row.addWidget(self.clear_btn)

        self.clear_btn.clicked.connect(lambda: self.edit.setText(""))
        self.edit.textChanged.connect(self._update_validity_style)
        self._update_validity_style()

    def set_value_from_store(self, v: Optional[float]) -> None:
        if v is None:
            self.edit.setText("")
            return
        if self.target_type == "frame":
            self.edit.setText(str(int(round(v))))
        elif self.target_type == "score":
            self.edit.setText(f"{v * 100.0:.3f}".rstrip("0").rstrip("."))
        else:
            self.edit.setText(f"{v:.6g}")

    def parse_to_store_value(self) -> Optional[float]:
        s = self.edit.text().strip()
        if s == "":
            return None
        try:
            if self.target_type == "frame":
                return float(int(s))
            if self.target_type == "score":
                # 0..100 -> 0..1
                return float(s) / 100.0
            return float(s)
        except Exception:
            return None

    def _update_validity_style(self) -> None:
        text = self.edit.text().strip()
        if not text:
            self.edit.setStyleSheet("")
            return
        if self.edit.hasAcceptableInput():
            self.edit.setStyleSheet("")
            return
        # Simple red outline to flag invalid input
        self.edit.setStyleSheet("border: 1px solid #c33;")


# ---------------------------
# Main window
# ---------------------------

class MainWindow(QMainWindow):
    def __init__(self, root_dir: Path, annotations_path: Path):
        super().__init__()
        self.setWindowTitle("Run Annotator")
        self.resize(1200, 720)

        self.settings = QSettings("double-pendulum", "annotate")
        self.root_dir = root_dir
        self.store = AnnotationStore(root_dir=root_dir, annotations_path=annotations_path)

        try:
            self.store.load()
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to load annotations:\n{e}")

        self.runs: List[RunInfo] = scan_runs(self.root_dir)

        # Autosave debounce
        self.autosave_enabled = True
        self._autosave_timer = QTimer(self)
        self._autosave_timer.setSingleShot(True)
        self._autosave_timer.timeout.connect(self._autosave_now)

        self._building_ui = False
        self._current_run: Optional[RunInfo] = None
        self._target_rows: Dict[str, TargetRow] = {}

        self._init_ui()
        self._refresh_targets_panel()
        self._update_window_title()

    # -------- UI setup --------

    def _init_ui(self) -> None:
        # Menus
        file_menu = self.menuBar().addMenu("&File")

        act_open_root = QAction("Open Output Directory…", self)
        act_open_root.triggered.connect(self._choose_root_dir)
        file_menu.addAction(act_open_root)

        act_open_ann = QAction("Open Annotations…", self)
        act_open_ann.triggered.connect(self._choose_annotations_file)
        file_menu.addAction(act_open_ann)

        file_menu.addSeparator()

        act_save = QAction("Save", self)
        act_save.setShortcut("Ctrl+S")
        act_save.triggered.connect(self._save)
        file_menu.addAction(act_save)

        act_save_as = QAction("Save As…", self)
        act_save_as.triggered.connect(self._save_as)
        file_menu.addAction(act_save_as)

        file_menu.addSeparator()

        act_quit = QAction("Quit", self)
        act_quit.setShortcut("Ctrl+Q")
        act_quit.triggered.connect(self.close)
        file_menu.addAction(act_quit)

        tools_menu = self.menuBar().addMenu("&Tools")
        act_targets = QAction("Edit Targets…", self)
        act_targets.triggered.connect(self._edit_targets)
        tools_menu.addAction(act_targets)

        # Central layout
        splitter = QSplitter()
        splitter.setOrientation(Qt.Orientation.Horizontal)
        self.setCentralWidget(splitter)

        # Left pane: search + table
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(8, 8, 8, 8)

        self.search_edit = QLineEdit()
        self.search_edit.setPlaceholderText("Search run id…")
        left_layout.addWidget(self.search_edit)

        self.model = RunsTableModel(self.runs, self.store)
        self.proxy = RunsFilterProxy()
        self.proxy.setSourceModel(self.model)

        self.table = QTableView()
        self.table.setModel(self.proxy)
        self.table.setSelectionBehavior(QTableView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QTableView.SelectionMode.SingleSelection)
        self.table.setSortingEnabled(True)
        self.table.sortByColumn(0, Qt.SortOrder.DescendingOrder)
        self.table.verticalHeader().setVisible(False)
        self.table.setAlternatingRowColors(True)
        self.table.setColumnWidth(0, 260)
        self.table.setColumnWidth(1, 80)
        self.table.setColumnWidth(2, 80)
        self.table.setColumnWidth(3, 140)
        self.table.horizontalHeader().sortIndicatorChanged.connect(self._on_sort_changed)

        left_layout.addWidget(self.table, 1)

        splitter.addWidget(left)

        # Right pane: run actions + targets + notes
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(10, 10, 10, 10)

        # Top: selected run info
        top_box = QFrame()
        top_box.setFrameShape(QFrame.Shape.StyledPanel)
        top_layout = QVBoxLayout(top_box)

        self.sel_label = QLabel("No run selected")
        self.sel_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        top_layout.addWidget(self.sel_label)

        btn_row = QHBoxLayout()
        self.open_video_btn = QPushButton("Open Video (mpv)")
        self.open_video_btn.clicked.connect(self._open_video)
        btn_row.addWidget(self.open_video_btn)

        self.open_folder_btn = QPushButton("Open Folder")
        self.open_folder_btn.clicked.connect(self._open_folder)
        btn_row.addWidget(self.open_folder_btn)

        self.prev_missing_btn = QPushButton("Prev Missing")
        self.prev_missing_btn.clicked.connect(lambda: self._select_missing_run(-1))
        btn_row.addWidget(self.prev_missing_btn)

        self.next_missing_btn = QPushButton("Next Missing")
        self.next_missing_btn.clicked.connect(lambda: self._select_missing_run(1))
        btn_row.addWidget(self.next_missing_btn)

        btn_row.addStretch(1)

        self.autosave_cb = QCheckBox("Autosave")
        self.autosave_cb.setChecked(True)
        self.autosave_cb.stateChanged.connect(self._on_autosave_changed)
        btn_row.addWidget(self.autosave_cb)

        self.include_unannotated_cb = QCheckBox("Include unannotated on save")
        self.include_unannotated_cb.setChecked(False)
        btn_row.addWidget(self.include_unannotated_cb)

        self.save_btn = QPushButton("Save")
        self.save_btn.clicked.connect(self._save)
        btn_row.addWidget(self.save_btn)

        top_layout.addLayout(btn_row)

        seek_row = QHBoxLayout()
        seek_row.addWidget(QLabel("Seek on open:"))
        self.seek_combo = QComboBox()
        self.seek_combo.addItem("none")
        seek_row.addWidget(self.seek_combo, 1)
        top_layout.addLayout(seek_row)

        right_layout.addWidget(top_box)

        # Targets area
        targets_header = QHBoxLayout()
        targets_header.addWidget(QLabel("Targets"))
        targets_header.addStretch(1)
        self.targets_edit_btn = QPushButton("Edit targets…")
        self.targets_edit_btn.clicked.connect(self._edit_targets)
        targets_header.addWidget(self.targets_edit_btn)
        right_layout.addLayout(targets_header)

        self.targets_scroll = QScrollArea()
        self.targets_scroll.setWidgetResizable(True)
        self.targets_container = QWidget()
        self.targets_vbox = QVBoxLayout(self.targets_container)
        self.targets_vbox.setContentsMargins(0, 0, 0, 0)
        self.targets_vbox.setSpacing(6)
        self.targets_scroll.setWidget(self.targets_container)
        right_layout.addWidget(self.targets_scroll, 1)

        # Notes
        right_layout.addWidget(QLabel("Notes"))
        self.notes_edit = QTextEdit()
        self.notes_edit.textChanged.connect(self._on_notes_changed)
        right_layout.addWidget(self.notes_edit, 1)

        splitter.addWidget(right)
        splitter.setStretchFactor(0, 2)
        splitter.setStretchFactor(1, 3)

        # Signals
        self.search_edit.textChanged.connect(self.proxy.set_text_filter)
        self.table.selectionModel().selectionChanged.connect(self._on_selection_changed)
        self._setup_shortcuts()

        self._restore_table_state()
        # Initial selection (top row if any)
        if self.proxy.rowCount() > 0 and not self.table.selectionModel().selectedRows():
            self.table.selectRow(0)

    # -------- Helpers --------

    def _update_window_title(self) -> None:
        star = "*" if self.store.dirty else ""
        self.setWindowTitle(f"Run Annotator{star} — {self.root_dir.as_posix()} — {self.store.annotations_path.as_posix()}")

    def _current_run_from_proxy_index(self, proxy_index: QModelIndex) -> Optional[RunInfo]:
        if not proxy_index.isValid():
            return None
        src = self.proxy.mapToSource(proxy_index)
        if not src.isValid():
            return None
        return self.runs[src.row()]

    def _on_selection_changed(self, *_args) -> None:
        sel = self.table.selectionModel().selectedRows()
        if not sel:
            self._set_current_run(None)
            return
        run = self._current_run_from_proxy_index(sel[0])
        if run:
            self.settings.setValue("last_run_id", run.run_id)
        self._set_current_run(run)

    def _set_current_run(self, run: Optional[RunInfo]) -> None:
        # push pending edits of previous run (already done live, but safe)
        self._current_run = run
        self._building_ui = True
        try:
            if run is None:
                self.sel_label.setText("No run selected")
                self.open_video_btn.setEnabled(False)
                self.open_folder_btn.setEnabled(False)
                self.notes_edit.setPlainText("")
                self._refresh_targets_panel()
                return

            self.sel_label.setText(
                f"{run.run_id}\n"
                f"video: {run.video_path.as_posix()}\n"
                f"data : {run.data_path.as_posix()}"
            )
            self.open_video_btn.setEnabled(run.has_video)
            self.open_folder_btn.setEnabled(True)

            self.notes_edit.blockSignals(True)
            self.notes_edit.setPlainText(self.store.get_note(run))
            self.notes_edit.blockSignals(False)

            self._refresh_targets_panel()
        finally:
            self._building_ui = False
        self._update_missing_nav_state()

    def _refresh_targets_panel(self) -> None:
        # Clear old
        while self.targets_vbox.count():
            item = self.targets_vbox.takeAt(0)
            w = item.widget()
            if w:
                w.deleteLater()

        self._target_rows.clear()

        # Rebuild seek options
        self.seek_combo.blockSignals(True)
        self.seek_combo.clear()
        self.seek_combo.addItem("none")
        self.seek_combo.blockSignals(False)

        if self._current_run is None:
            self.targets_vbox.addSpacerItem(QSpacerItem(20, 20, QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding))
            return

        if not self.store.target_defs:
            msg = QLabel("No targets defined. Click “Edit targets…” to add your target list.")
            msg.setWordWrap(True)
            self.targets_vbox.addWidget(msg)
            self.targets_vbox.addSpacerItem(QSpacerItem(20, 20, QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding))
            return

        # rows in stable order
        for name in sorted(self.store.target_defs.keys()):
            t = self.store.target_defs[name]
            row = TargetRow(name=name, target_type=t, parent=self.targets_container)
            self._target_rows[name] = row
            v = self.store.get_target_value(self._current_run, name)
            row.set_value_from_store(v)

            # seek dropdown only for frame targets
            if t == "frame":
                self.seek_combo.addItem(name)

            # hook changes
            row.edit.textChanged.connect(self._on_target_changed)
            self.targets_vbox.addWidget(row)

        self.targets_vbox.addSpacerItem(QSpacerItem(20, 20, QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding))
        self._update_missing_nav_state()

    def _on_target_changed(self) -> None:
        if self._building_ui or self._current_run is None:
            return

        # Find which row triggered, update only that
        sender = self.sender()
        for name, row in self._target_rows.items():
            if row.edit is sender:
                if row.edit.text().strip() and not row.edit.hasAcceptableInput():
                    return
                value = row.parse_to_store_value()
                # For frame target: keep integer-ish
                if row.target_type == "frame" and value is not None:
                    value = float(int(round(value)))
                self.store.set_target_value(self._current_run, name, value)
                self.model.refresh_row(self._current_run.run_id)
                self._mark_dirty_and_maybe_autosave()
                self._update_window_title()
                self._update_missing_nav_state()
                return

    def _on_notes_changed(self) -> None:
        if self._building_ui or self._current_run is None:
            return
        self.store.set_note(self._current_run, self.notes_edit.toPlainText())
        self.model.refresh_row(self._current_run.run_id)
        self._mark_dirty_and_maybe_autosave()
        self._update_window_title()
        self._update_missing_nav_state()

    def _on_autosave_changed(self, state: int) -> None:
        self.autosave_enabled = state == Qt.CheckState.Checked
        if self.autosave_enabled:
            self._mark_dirty_and_maybe_autosave()

    def _restore_table_state(self) -> None:
        sort_col = self.settings.value("sort_column", 0, type=int)
        sort_order_val = self.settings.value("sort_order", Qt.SortOrder.DescendingOrder.value, type=int)
        try:
            sort_order = Qt.SortOrder(sort_order_val)
        except Exception:
            sort_order = Qt.SortOrder.DescendingOrder
        self.table.sortByColumn(sort_col, sort_order)

        last_run_id = self.settings.value("last_run_id", "", type=str)
        if last_run_id:
            self._select_run_by_id(last_run_id)

    def _on_sort_changed(self, column: int, order: Qt.SortOrder) -> None:
        self.settings.setValue("sort_column", column)
        self.settings.setValue("sort_order", int(order.value))

    def _select_run_by_id(self, run_id: str) -> bool:
        for i, run in enumerate(self.runs):
            if run.run_id == run_id:
                src_index = self.model.index(i, 0)
                proxy_index = self.proxy.mapFromSource(src_index)
                if proxy_index.isValid():
                    self.table.selectRow(proxy_index.row())
                    return True
                return False
        return False

    def _setup_shortcuts(self) -> None:
        def guard(handler):
            def _wrapped() -> None:
                if self._text_input_has_focus():
                    return
                handler()
            return _wrapped

        def bind(seq: str, handler) -> None:
            sc = QShortcut(QKeySequence(seq), self)
            sc.setContext(Qt.ShortcutContext.WindowShortcut)
            sc.activated.connect(handler)

        bind("N", guard(self._select_next_run))
        bind("P", guard(self._select_prev_run))
        bind("O", guard(self._open_video))
        bind("S", guard(self._save))

    def _text_input_has_focus(self) -> bool:
        w = QApplication.focusWidget()
        return isinstance(w, (QLineEdit, QTextEdit))

    def _select_prev_run(self) -> None:
        self._select_adjacent_run(-1)

    def _select_next_run(self) -> None:
        self._select_adjacent_run(1)

    def _select_adjacent_run(self, direction: int) -> None:
        if self.proxy.rowCount() == 0:
            return
        sel = self.table.selectionModel().selectedRows()
        row = sel[0].row() if sel else 0
        new_row = max(0, min(self.proxy.rowCount() - 1, row + direction))
        self.table.selectRow(new_row)

    def _run_missing_targets(self, run: RunInfo) -> bool:
        filled, total = self.store.target_completion(run)
        if total == 0:
            return False
        return filled < total

    def _select_missing_run(self, direction: int) -> None:
        if self._text_input_has_focus():
            return
        if self.proxy.rowCount() == 0:
            return
        if len(self.store.target_defs) == 0:
            return
        sel = self.table.selectionModel().selectedRows()
        start = sel[0].row() if sel else 0
        total_rows = self.proxy.rowCount()
        for offset in range(1, total_rows + 1):
            row = (start + direction * offset) % total_rows
            src = self.proxy.mapToSource(self.proxy.index(row, 0))
            if not src.isValid():
                continue
            run = self.runs[src.row()]
            if self._run_missing_targets(run):
                self.table.selectRow(row)
                return
        self.statusBar().showMessage("No runs with missing targets", 2000)

    def _update_missing_nav_state(self) -> None:
        if len(self.store.target_defs) == 0 or self.proxy.rowCount() == 0:
            self.prev_missing_btn.setEnabled(False)
            self.next_missing_btn.setEnabled(False)
            return
        any_missing = False
        for row in range(self.proxy.rowCount()):
            src = self.proxy.mapToSource(self.proxy.index(row, 0))
            if not src.isValid():
                continue
            run = self.runs[src.row()]
            if self._run_missing_targets(run):
                any_missing = True
                break
        self.prev_missing_btn.setEnabled(any_missing)
        self.next_missing_btn.setEnabled(any_missing)

    def _mark_dirty_and_maybe_autosave(self) -> None:
        if not self.store.dirty:
            return
        if self.autosave_enabled:
            # debounce
            self._autosave_timer.start(900)

    def _autosave_now(self) -> None:
        # autosave should not pop dialogs; fail silently with a status bar msg
        try:
            self.store.save(
                include_unannotated=self.include_unannotated_cb.isChecked(),
                all_runs=self.runs,
            )
            self.statusBar().showMessage("Autosaved", 1500)
        except Exception as e:
            self.statusBar().showMessage(f"Autosave failed: {e}", 4000)
        self._update_window_title()

    # -------- Actions --------

    def _edit_targets(self) -> None:
        dlg = TargetEditorDialog(self, self.store.target_defs)
        dlg.exec()
        new_defs = dlg.target_defs()
        if new_defs != self.store.target_defs:
            self.store.target_defs = new_defs
            # Ensure any existing stored targets are still representable
            for rid, entry in self.store.by_id.items():
                t = entry.get("targets", {})
                if isinstance(t, dict):
                    for k in t.keys():
                        if k not in self.store.target_defs:
                            self.store.target_defs[k] = "number"
            self.store.dirty = True
            self._refresh_targets_panel()
            self.model.refresh_all()
            self._update_window_title()
            self._mark_dirty_and_maybe_autosave()

    def _open_local_path(self, path: Path) -> None:
        url = QUrl.fromLocalFile(str(path.resolve()))
        QDesktopServices.openUrl(url)

    def _open_folder(self) -> None:
        if not self._current_run:
            return
        self._open_local_path(self._current_run.run_dir)

    def _open_video(self) -> None:
        if not self._current_run or not self._current_run.has_video:
            return

        video = self._current_run.video_path
        seek_target = self.seek_combo.currentText().strip()

        start_args: List[str] = []
        if seek_target and seek_target != "none":
            # Use simulation_data.bin header to estimate seconds
            v = self.store.get_target_value(self._current_run, seek_target)
            if v is not None:
                hdr = read_sim_header_frame_duration(self._current_run.data_path)
                if hdr is not None:
                    _frame_cnt, frame_dur = hdr
                    seconds = float(int(round(v))) * frame_dur
                    # mpv accepts --start=seconds
                    start_args = [f"--start={seconds:.6f}"]

        if shutil.which(MPV_COMMAND):
            cmd = [MPV_COMMAND, *start_args, video.as_posix()]
            try:
                subprocess.Popen(cmd)
                return
            except Exception as e:
                QMessageBox.warning(self, "mpv failed", f"Failed to run mpv:\n{e}\n\nFalling back to default opener.")
        # fallback: OS default opener
        self._open_local_path(video)

    def _save(self) -> None:
        try:
            self.store.save(
                include_unannotated=self.include_unannotated_cb.isChecked(),
                all_runs=self.runs,
            )
            self.statusBar().showMessage("Saved", 1500)
        except Exception as e:
            QMessageBox.critical(self, "Save failed", f"Save failed:\n{e}")
        self._update_window_title()

    def _save_as(self) -> None:
        path_str, _ = QFileDialog.getSaveFileName(
            self,
            "Save Annotations As",
            str(self.store.annotations_path),
            "JSON Files (*.json);;All Files (*)",
        )
        if not path_str:
            return
        self.store.annotations_path = Path(path_str)
        self._save()
        self._update_window_title()

    def _choose_root_dir(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "Select Output Directory", str(self.root_dir))
        if not d:
            return
        new_root = Path(d)
        if new_root == self.root_dir:
            return

        if self.store.dirty:
            r = QMessageBox.question(
                self,
                "Unsaved changes",
                "You have unsaved changes. Save before switching output directory?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No | QMessageBox.StandardButton.Cancel,
            )
            if r == QMessageBox.StandardButton.Cancel:
                return
            if r == QMessageBox.StandardButton.Yes:
                self._save()
                if self.store.dirty:
                    return

        self.root_dir = new_root
        self.store.root_dir = new_root
        self.runs = scan_runs(new_root)
        self.model = RunsTableModel(self.runs, self.store)
        self.proxy.setSourceModel(self.model)
        self._set_current_run(None)
        if self.proxy.rowCount() > 0:
            self.table.selectRow(0)
        self._update_window_title()

    def _choose_annotations_file(self) -> None:
        path_str, _ = QFileDialog.getOpenFileName(
            self,
            "Open Annotations JSON",
            str(self.store.annotations_path),
            "JSON Files (*.json);;All Files (*)",
        )
        if not path_str:
            return
        new_path = Path(path_str)
        if new_path == self.store.annotations_path:
            return

        if self.store.dirty:
            r = QMessageBox.question(
                self,
                "Unsaved changes",
                "You have unsaved changes. Save before switching annotations file?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No | QMessageBox.StandardButton.Cancel,
            )
            if r == QMessageBox.StandardButton.Cancel:
                return
            if r == QMessageBox.StandardButton.Yes:
                self._save()
                if self.store.dirty:
                    return

        self.store.annotations_path = new_path
        try:
            self.store.load()
        except Exception as e:
            QMessageBox.critical(self, "Error", f"Failed to load annotations:\n{e}")
        self.model.refresh_all()
        self._set_current_run(self._current_run)
        self._update_window_title()

    # -------- Close handling --------

    def closeEvent(self, event) -> None:
        if self.store.dirty:
            r = QMessageBox.question(
                self,
                "Unsaved changes",
                "Save changes before quitting?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No | QMessageBox.StandardButton.Cancel,
            )
            if r == QMessageBox.StandardButton.Cancel:
                event.ignore()
                return
            if r == QMessageBox.StandardButton.Yes:
                self._save()
                if self.store.dirty:
                    event.ignore()
                    return
        if self._current_run:
            self.settings.setValue("last_run_id", self._current_run.run_id)
        event.accept()


# ---------------------------
# Main
# ---------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=str, default="output/eval2", help="Root directory containing run_* folders")
    ap.add_argument("--annotations", type=str, default="output/eval2/annotations.json", help="Path to annotations JSON")
    args = ap.parse_args()

    root_dir = Path(args.root)
    ann_path = Path(args.annotations)

    app = QApplication(sys.argv)
    w = MainWindow(root_dir=root_dir, annotations_path=ann_path)
    w.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
