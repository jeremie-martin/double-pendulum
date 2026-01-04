"""NiceGUI dashboard for video processor.

Simple, focused UI for monitoring and controlling video processing.
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
from typing import Optional

from nicegui import ui

from ..archive import load_upload_records
from ..logging import get_logger
from ..music import MusicManager
from .processor import ProcessorState, ProcessorStatus, VideoProcessor

log = get_logger(__name__)

# Global references (set by run_dashboard)
_processor: Optional[VideoProcessor] = None
_music_manager: Optional[MusicManager] = None


def _status_color(status: str) -> str:
    """Get color for status badge."""
    colors = {
        "starting": "blue",
        "running": "green",
        "paused": "orange",
        "waiting": "blue",
        "auth_required": "red",
        "stopped": "grey",
    }
    return colors.get(status, "grey")


def _format_timestamp(iso_str: str) -> str:
    """Format ISO timestamp to readable string."""
    try:
        dt = datetime.fromisoformat(iso_str)
        return dt.strftime("%m/%d %H:%M")
    except (ValueError, TypeError):
        return ""


def _nav_links():
    """Render navigation links."""
    ui.separator().classes("mt-4")
    with ui.row().classes("gap-4"):
        ui.link("Dashboard", "/").classes("text-blue-400")
        ui.link("Music", "/music").classes("text-blue-400")
        ui.link("History", "/history").classes("text-blue-400")
        ui.link("Settings", "/settings").classes("text-blue-400")


def create_dashboard(state: ProcessorState) -> None:
    """Create the dashboard UI."""

    @ui.page("/")
    def main_page():
        ui.dark_mode(True)

        # Header
        with ui.header().classes("items-center justify-between"):
            ui.label("Pendulum Watcher").classes("text-xl font-bold")
            with ui.row().classes("gap-2"):
                pause_btn = ui.button("Pause")
                ui.link("Settings", "/settings").classes(
                    "px-4 py-2 bg-gray-700 rounded text-white no-underline"
                )

        # Status section
        with ui.card().classes("w-full"):
            with ui.row().classes("items-center gap-4"):
                status_badge = ui.badge("").classes("text-lg")
                stats_label = ui.label()

            wait_label = ui.label().classes("text-sm text-gray-400")

        # Current job
        ui.label("Current").classes("text-lg font-semibold mt-4")
        with ui.card().classes("w-full"):
            current_name = ui.label("No active job").classes("font-mono")
            current_progress = ui.label().classes("text-sm text-gray-400")

        # Queue
        ui.label("Queue").classes("text-lg font-semibold mt-4")
        queue_container = ui.column().classes("w-full gap-1")

        # Recent uploads (from archive)
        with ui.row().classes("items-center justify-between mt-4"):
            ui.label("Recent Uploads").classes("text-lg font-semibold")
            ui.link("View all →", "/history").classes("text-blue-400 text-sm")

        history_container = ui.column().classes("w-full gap-1")

        _nav_links()

        def toggle_pause():
            if _processor:
                if _processor.is_paused():
                    _processor.resume()
                else:
                    _processor.pause()

        pause_btn.on_click(toggle_pause)

        def refresh():
            snapshot = state.snapshot()

            # Status
            status = snapshot["status"]
            status_badge.text = status.upper()
            status_badge.props(f'color="{_status_color(status)}"')

            stats_label.text = (
                f"Processed: {snapshot['total_processed']} | "
                f"Failed: {snapshot['total_failed']} | "
                f"Queue: {len(snapshot['pending_queue'])}"
            )

            # Wait info
            if snapshot["wait_until"]:
                wait_label.text = f"Waiting: {snapshot['wait_reason']}"
            else:
                wait_label.text = ""

            # Pause button
            pause_btn.text = "Resume" if _processor and _processor.is_paused() else "Pause"

            # Current job
            current = snapshot["current_job"]
            if current:
                current_name.text = current["dir_name"]
                current_progress.text = current["progress"]
            else:
                current_name.text = "No active job"
                current_progress.text = ""

            # Queue
            queue_container.clear()
            with queue_container:
                pending = snapshot["pending_queue"][:10]
                if not pending:
                    ui.label("No videos in queue").classes("text-gray-400 text-sm")
                for job in pending:
                    with ui.row().classes("w-full items-center bg-gray-800 rounded px-2 py-1"):
                        ui.icon("schedule", size="sm").classes("text-gray-400")
                        ui.label(job["dir_name"]).classes("font-mono")

            # Recent from archive
            history_container.clear()
            with history_container:
                try:
                    records = load_upload_records()
                    recent = records[-10:][::-1]  # Last 10, newest first

                    if not recent:
                        ui.label("No uploads yet").classes("text-gray-400 text-sm")

                    for record in recent:
                        with ui.row().classes("w-full items-center gap-2 bg-gray-800 rounded px-2 py-1"):
                            # Timestamp
                            ui.label(_format_timestamp(record.uploaded_at)).classes(
                                "w-20 text-xs text-gray-400"
                            )
                            # Title (truncated)
                            title = record.title[:30] + "..." if len(record.title) > 30 else record.title
                            ui.label(title).classes("flex-1 text-sm")
                            # Theme
                            if record.theme:
                                ui.badge(record.theme, color="blue").classes("text-xs")
                            # Music
                            if record.music_title:
                                music = record.music_title[:15] + "..." if len(record.music_title) > 15 else record.music_title
                                ui.label(f"♪ {music}").classes("text-xs text-gray-400")
                            # URL
                            ui.link("↗", record.url).classes("text-blue-400")
                except Exception as e:
                    ui.label(f"Error loading history: {e}").classes("text-red-400 text-sm")

        ui.timer(2.0, refresh)
        refresh()

    @ui.page("/music")
    def music_page():
        ui.dark_mode(True)

        tracks_container = ui.column().classes("w-full gap-2")

        def do_reset_weights():
            if _music_manager:
                _music_manager.reset_all_weights()
                _music_manager.reload()
                do_render_tracks()
                ui.notify("Weights reset to 1.0", type="positive")

        def do_enable_all():
            if _music_manager:
                _music_manager.enable_all()
                _music_manager.reload()
                do_render_tracks()
                ui.notify("All tracks enabled", type="positive")

        def do_disable_all():
            if _music_manager:
                _music_manager.disable_all()
                _music_manager.reload()
                do_render_tracks()
                ui.notify("All tracks disabled", type="warning")

        def do_refresh():
            if _music_manager:
                _music_manager.reload()
                do_render_tracks()
                ui.notify("Refreshed", type="info")

        def do_render_tracks():
            tracks_container.clear()

            if not _music_manager:
                with tracks_container:
                    ui.label("No music directory configured").classes("text-gray-400")
                return

            tracks = _music_manager.tracks

            if not tracks:
                with tracks_container:
                    ui.label("No tracks found").classes("text-gray-400")
                return

            with tracks_container:
                # Header
                with ui.row().classes("w-full gap-4 font-bold text-gray-400 px-2"):
                    ui.label("On").classes("w-12")
                    ui.label("Track").classes("flex-1")
                    ui.label("Drop").classes("w-16 text-right")
                    ui.label("Weight").classes("w-32")
                    ui.label("Used").classes("w-12 text-right")

                for track in tracks:
                    with ui.row().classes("w-full items-center gap-4 bg-gray-800 rounded px-2 py-2"):
                        # Enable checkbox
                        cb = ui.checkbox(value=track.enabled).classes("w-12")
                        cb.on_value_change(
                            lambda e, tid=track.id: _music_manager.set_enabled(tid, e.value)
                        )

                        # Track info
                        with ui.column().classes("flex-1"):
                            ui.label(track.title or track.id).classes("font-bold")
                            ui.label(track.id).classes("text-xs text-gray-400")

                        # Drop time
                        ui.label(f"{track.drop_time_seconds:.1f}s").classes("w-16 text-right")

                        # Weight slider with value
                        with ui.row().classes("w-32 items-center gap-1"):
                            slider = ui.slider(min=0, max=3.0, step=0.1, value=track.weight).classes("flex-1")
                            weight_label = ui.label(f"{track.weight:.1f}").classes("w-8 text-right text-sm")

                            def on_weight_change(e, tid=track.id, lbl=weight_label):
                                _music_manager.set_weight(tid, e.value)
                                lbl.text = f"{e.value:.1f}"

                            slider.on_value_change(on_weight_change)

                        # Use count
                        ui.label(str(track.use_count)).classes("w-12 text-right text-gray-400")

        # Header
        with ui.header().classes("items-center justify-between"):
            ui.label("Music Library").classes("text-xl font-bold")
            with ui.row().classes("gap-2"):
                ui.button("Refresh", on_click=do_refresh)
                ui.link("Back", "/").classes("px-4 py-2 bg-gray-700 rounded text-white no-underline")

        # Bulk actions
        with ui.row().classes("gap-2 mt-4"):
            ui.button("Reset Weights", on_click=do_reset_weights)
            ui.button("Enable All", on_click=do_enable_all).props("color=positive")
            ui.button("Disable All", on_click=do_disable_all).props("color=warning")

        ui.separator()

        # Render tracks
        do_render_tracks()

        _nav_links()

    @ui.page("/history")
    def history_page():
        ui.dark_mode(True)

        with ui.header().classes("items-center justify-between"):
            ui.label("Upload History").classes("text-xl font-bold")
            ui.link("Back", "/").classes("px-4 py-2 bg-gray-700 rounded text-white no-underline")

        # Load all records
        try:
            records = load_upload_records()
            records = records[::-1]  # Newest first

            ui.label(f"{len(records)} uploads").classes("text-gray-400 mt-4")

            # Table header
            with ui.row().classes("w-full gap-2 font-bold text-gray-400 px-2 mt-2"):
                ui.label("Date").classes("w-24")
                ui.label("Title").classes("flex-1")
                ui.label("Theme").classes("w-32")
                ui.label("Music").classes("w-40")
                ui.label("Link").classes("w-12")

            # Records
            for record in records:
                with ui.row().classes("w-full items-center gap-2 bg-gray-800 rounded px-2 py-1"):
                    # Date
                    ui.label(_format_timestamp(record.uploaded_at)).classes("w-24 text-xs text-gray-400")

                    # Title
                    title = record.title[:40] + "..." if len(record.title) > 40 else record.title
                    ui.label(title).classes("flex-1 text-sm")

                    # Theme
                    if record.theme:
                        ui.badge(record.theme, color="blue").classes("w-32 text-xs")
                    else:
                        ui.label("-").classes("w-32 text-gray-500")

                    # Music
                    if record.music_title:
                        music = record.music_title[:20] + "..." if len(record.music_title) > 20 else record.music_title
                        ui.label(f"♪ {music}").classes("w-40 text-xs text-gray-400")
                    else:
                        ui.label("-").classes("w-40 text-gray-500")

                    # URL
                    ui.link("↗", record.url).classes("w-12 text-blue-400")

        except Exception as e:
            ui.label(f"Error loading history: {e}").classes("text-red-400 mt-4")

        _nav_links()

    @ui.page("/settings")
    def settings_page():
        ui.dark_mode(True)

        def do_apply_settings():
            state.upload_delay = upload_delay.value
            state.privacy = privacy.value
            state.playlist_id = playlist_id.value or None
            state.poll_interval = poll_interval.value
            state.settle_time = settle_time.value
            state.delete_after_upload = delete_after.value

            # Interrupt any current wait so new settings apply
            if _processor:
                _processor.interrupt_wait()

            ui.notify("Settings applied", type="positive")
            log.info(f"Settings updated: delay={state.upload_delay}, privacy={state.privacy}")

        def do_reauth():
            if _processor:
                ui.notify("Re-authenticating...", type="info")
                if _processor.trigger_reauth():
                    ui.notify("Authentication successful!", type="positive")
                else:
                    ui.notify("Authentication failed", type="negative")

        with ui.header().classes("items-center justify-between"):
            ui.label("Settings").classes("text-xl font-bold")
            ui.link("Back", "/").classes("px-4 py-2 bg-gray-700 rounded text-white no-underline")

        # Current settings
        ui.label("Upload Settings").classes("text-lg font-semibold mt-4")
        with ui.card().classes("w-full"):
            upload_delay = ui.number(
                "Upload Delay (seconds)",
                value=state.upload_delay,
                min=0,
                max=7200,
            ).classes("w-full")

            privacy = ui.select(
                options=["private", "unlisted", "public"],
                value=state.privacy,
                label="Privacy",
            ).classes("w-full")

            playlist_id = ui.input(
                "Playlist ID",
                value=state.playlist_id or "",
            ).classes("w-full")

        ui.label("Timing Settings").classes("text-lg font-semibold mt-4")
        with ui.card().classes("w-full"):
            poll_interval = ui.number(
                "Poll Interval (seconds)",
                value=state.poll_interval,
                min=1,
                max=60,
            ).classes("w-full")

            settle_time = ui.number(
                "Settle Time (seconds)",
                value=state.settle_time,
                min=1,
                max=120,
            ).classes("w-full")

            delete_after = ui.checkbox(
                "Delete after upload",
                value=state.delete_after_upload,
            )

        ui.button("Apply", on_click=do_apply_settings).classes("mt-4").props("color=positive")

        # Auth section
        ui.label("Authentication").classes("text-lg font-semibold mt-4")
        with ui.card().classes("w-full"):
            auth_status = ui.label()

            def refresh_auth():
                if state.auth_error:
                    auth_status.text = f"Error: {state.auth_error}"
                    auth_status.classes("text-red-400")
                else:
                    auth_status.text = "Authenticated"
                    auth_status.classes("text-green-400")

            ui.button("Re-authenticate", on_click=do_reauth)
            ui.timer(2.0, refresh_auth)
            refresh_auth()

        _nav_links()


def run_dashboard(
    processor: VideoProcessor,
    state: ProcessorState,
    music_dir: Optional[Path] = None,
    host: str = "0.0.0.0",
    port: int = 8080,
) -> None:
    """Run the dashboard server.

    Args:
        processor: The video processor instance
        state: Shared processor state
        music_dir: Path to music directory (optional)
        host: Server host
        port: Server port
    """
    global _processor, _music_manager

    _processor = processor

    if music_dir:
        try:
            _music_manager = MusicManager(music_dir)
            log.info(f"Loaded music library: {len(_music_manager.tracks)} tracks")
        except FileNotFoundError as e:
            log.warning(f"Music directory not found: {e}")
            _music_manager = None

    create_dashboard(state)

    log.info(f"Starting dashboard at http://{host}:{port}/")
    ui.run(
        host=host,
        port=port,
        title="Pendulum Watcher",
        reload=False,
        show=False,
    )
