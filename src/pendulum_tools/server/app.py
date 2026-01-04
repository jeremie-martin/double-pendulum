"""NiceGUI dashboard for video processor.

Simple, focused UI for monitoring and controlling video processing.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from nicegui import ui

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
                ui.button("Settings", on_click=lambda: ui.navigate.to("/settings"))

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

        # Recent
        ui.label("Recent").classes("text-lg font-semibold mt-4")
        history_container = ui.column().classes("w-full gap-1")

        # Navigation
        ui.separator().classes("mt-4")
        with ui.row().classes("gap-4"):
            ui.link("Dashboard", "/").classes("text-blue-400")
            ui.link("Music", "/music").classes("text-blue-400")
            ui.link("Settings", "/settings").classes("text-blue-400")

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
                for job in snapshot["pending_queue"][:10]:
                    with ui.row().classes("w-full items-center bg-gray-800 rounded px-2 py-1"):
                        ui.icon("schedule", size="sm").classes("text-gray-400")
                        ui.label(job["dir_name"]).classes("font-mono")

            # History
            history_container.clear()
            with history_container:
                # Failed first
                for job in list(snapshot["failed"])[:3]:
                    with ui.row().classes("w-full items-center bg-gray-800 rounded px-2 py-1"):
                        ui.icon("close", color="red")
                        ui.label(job["dir_name"]).classes("font-mono")
                        ui.label(job.get("error", "")[:30]).classes("text-sm text-gray-400")

                # Completed
                for job in list(snapshot["completed"])[:5]:
                    with ui.row().classes("w-full items-center bg-gray-800 rounded px-2 py-1"):
                        ui.icon("check", color="green")
                        ui.label(job["dir_name"]).classes("font-mono")
                        if job.get("video_url"):
                            ui.link(job["video_url"], job["video_url"]).classes("text-sm text-blue-400")

        ui.timer(1.0, refresh)
        refresh()

    @ui.page("/music")
    def music_page():
        ui.dark_mode(True)

        with ui.header().classes("items-center justify-between"):
            ui.label("Music Library").classes("text-xl font-bold")
            with ui.row().classes("gap-2"):
                ui.button("Refresh", on_click=lambda: render_tracks())
                ui.button("Back", on_click=lambda: ui.navigate.to("/"))

        # Bulk actions
        with ui.row().classes("gap-2 mt-4"):
            ui.button("Reset Weights", on_click=lambda: reset_weights())
            ui.button("Enable All", on_click=lambda: enable_all())
            ui.button("Disable All", on_click=lambda: disable_all())

        ui.separator()
        tracks_container = ui.column().classes("w-full gap-2 mt-2")

        def reset_weights():
            if _music_manager:
                _music_manager.reset_all_weights()
                render_tracks()
                ui.notify("Weights reset to 1.0")

        def enable_all():
            if _music_manager:
                _music_manager.enable_all()
                render_tracks()
                ui.notify("All tracks enabled")

        def disable_all():
            if _music_manager:
                _music_manager.disable_all()
                render_tracks()
                ui.notify("All tracks disabled")

        def render_tracks():
            tracks_container.clear()

            if not _music_manager:
                with tracks_container:
                    ui.label("No music directory configured").classes("text-gray-400")
                return

            _music_manager.reload()
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

        render_tracks()

        # Navigation
        ui.separator().classes("mt-4")
        with ui.row().classes("gap-4"):
            ui.link("Dashboard", "/").classes("text-blue-400")
            ui.link("Music", "/music").classes("text-blue-400")
            ui.link("Settings", "/settings").classes("text-blue-400")

    @ui.page("/settings")
    def settings_page():
        ui.dark_mode(True)

        with ui.header().classes("items-center justify-between"):
            ui.label("Settings").classes("text-xl font-bold")
            ui.button("Back", on_click=lambda: ui.navigate.to("/"))

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

        def apply_settings():
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

        ui.button("Apply", on_click=apply_settings).classes("mt-4")

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

            ui.button("Re-authenticate", on_click=lambda: _processor and _processor.trigger_reauth())
            ui.timer(2.0, refresh_auth)
            refresh_auth()

        # Navigation
        ui.separator().classes("mt-4")
        with ui.row().classes("gap-4"):
            ui.link("Dashboard", "/").classes("text-blue-400")
            ui.link("Music", "/music").classes("text-blue-400")
            ui.link("Settings", "/settings").classes("text-blue-400")


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
