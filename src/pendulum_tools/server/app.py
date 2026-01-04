"""NiceGUI application setup and routing."""

from pathlib import Path
from typing import TYPE_CHECKING, Optional

from nicegui import app, ui

from .state import WatcherState, WatcherStatus

if TYPE_CHECKING:
    from .watcher import WatcherThread


# Global state - initialized by run_server()
_state: Optional[WatcherState] = None
_watcher: Optional["WatcherThread"] = None


def get_state() -> WatcherState:
    """Get the global watcher state."""
    if _state is None:
        raise RuntimeError("Server not initialized")
    return _state


def get_watcher() -> Optional["WatcherThread"]:
    """Get the global watcher thread."""
    return _watcher


def set_watcher(watcher: "WatcherThread") -> None:
    """Set the global watcher thread."""
    global _watcher
    _watcher = watcher


def _status_color(status: str) -> str:
    """Get color for status badge."""
    colors = {
        "RUNNING": "positive",
        "PAUSED": "warning",
        "AUTH_REQUIRED": "negative",
        "STOPPED": "grey",
        "STARTING": "info",
        "STOPPING": "warning",
    }
    return colors.get(status, "grey")


def _job_status_color(status: str) -> str:
    """Get color for job status."""
    colors = {
        "PENDING": "grey",
        "SETTLING": "info",
        "PROCESSING": "primary",
        "ADDING_MUSIC": "primary",
        "UPLOADING": "accent",
        "WAITING_DELAY": "info",
        "COMPLETED": "positive",
        "FAILED": "negative",
        "SKIPPED": "warning",
    }
    return colors.get(status, "grey")


def create_app(state: WatcherState) -> None:
    """Create the NiceGUI application with all routes."""
    global _state
    _state = state

    @ui.page("/")
    def dashboard_page():
        """Main dashboard page."""
        ui.dark_mode(True)

        with ui.header().classes("items-center justify-between"):
            ui.label("Pendulum Watcher Dashboard").classes("text-xl font-bold")
            with ui.row().classes("gap-2"):
                pause_btn = ui.button("Pause", on_click=_toggle_pause)
                ui.button("Process Now", on_click=_process_now).props("color=primary")
                ui.button("Settings", on_click=lambda: ui.navigate.to("/settings"))

        # Status bar
        status_label = ui.label().classes("text-lg")
        stats_label = ui.label().classes("text-sm text-gray-400")

        ui.separator()

        # Auth error alert
        auth_alert = ui.card().classes("bg-red-900 w-full hidden")
        with auth_alert:
            with ui.row().classes("items-center justify-between w-full"):
                ui.icon("warning", color="red").classes("text-2xl")
                auth_msg = ui.label().classes("flex-1")
                ui.button("Re-authenticate", on_click=_reauthenticate).props("flat color=white")

        ui.separator()

        # Current job section
        ui.label("Current Job").classes("text-lg font-semibold mt-4")
        current_card = ui.card().classes("w-full")
        with current_card:
            current_name = ui.label("No active job").classes("font-mono")
            current_status = ui.label("").classes("text-sm")
            current_progress = ui.linear_progress(value=0).classes("w-full")

        # Pending queue section
        ui.label("Pending Queue").classes("text-lg font-semibold mt-4")
        pending_container = ui.column().classes("w-full gap-1")

        # Recent completed section
        ui.label("Recent").classes("text-lg font-semibold mt-4")
        history_container = ui.column().classes("w-full gap-1")

        # Navigation
        ui.separator().classes("mt-4")
        with ui.row().classes("gap-4"):
            ui.link("DASHBOARD", "/").classes("text-blue-400")
            ui.link("MUSIC", "/music").classes("text-blue-400")
            ui.link("HISTORY", "/history").classes("text-blue-400")
            ui.link("ERRORS", "/errors").classes("text-blue-400")
            ui.link("SETTINGS", "/settings").classes("text-blue-400")

        def refresh():
            """Refresh UI from state."""
            snapshot = state.snapshot()

            # Status bar
            status = snapshot["status"]
            status_label.text = f"Status: {status}"
            status_label.classes(replace=f"text-lg text-{_status_color(status)}-400")

            stats_label.text = (
                f"Processed: {snapshot['total_processed']} | "
                f"Failed: {snapshot['total_failed']} | "
                f"Queue: {snapshot['pending_count']}"
            )

            # Update pause button text
            pause_btn.text = "Resume" if snapshot["paused"] else "Pause"

            # Auth alert
            if snapshot["auth_error"]:
                auth_alert.classes(remove="hidden")
                auth_msg.text = f"Authentication failed: {snapshot['auth_error']}"
            else:
                auth_alert.classes(add="hidden")

            # Current job
            current = snapshot["current_job"]
            if current:
                current_name.text = current["dir_name"]
                current_status.text = f"{current['status']} - {current['progress_message']}"
                current_progress.value = current["progress_percent"] / 100
            else:
                current_name.text = "No active job"
                current_status.text = ""
                current_progress.value = 0

            # Pending queue
            pending_container.clear()
            with pending_container:
                for job in snapshot["pending_queue"][:10]:
                    with ui.row().classes("w-full items-center gap-2 bg-gray-800 rounded px-2 py-1"):
                        ui.badge(job["status"], color=_job_status_color(job["status"]))
                        ui.label(job["dir_name"]).classes("font-mono flex-1")

            # History
            history_container.clear()
            with history_container:
                # Show failed first
                for job in snapshot["failed_history"][:5]:
                    with ui.row().classes("w-full items-center gap-2 bg-gray-800 rounded px-2 py-1"):
                        ui.icon("close", color="red")
                        ui.label(job["dir_name"]).classes("font-mono")
                        ui.label(job.get("error", "Unknown error")[:40]).classes(
                            "text-sm text-gray-400 flex-1"
                        )
                        ui.button(
                            "Retry",
                            on_click=lambda d=job["dir_name"]: _retry_job(d),
                        ).props("flat dense")

                # Then completed
                for job in snapshot["completed_history"][:5]:
                    with ui.row().classes("w-full items-center gap-2 bg-gray-800 rounded px-2 py-1"):
                        if job["status"] == "COMPLETED":
                            ui.icon("check", color="green")
                        else:
                            ui.icon("skip_next", color="orange")
                        ui.label(job["dir_name"]).classes("font-mono")
                        if job.get("video_url"):
                            ui.link(job["video_url"], job["video_url"]).classes(
                                "text-sm text-blue-400"
                            )

        # Poll for updates every second
        ui.timer(1.0, refresh)
        refresh()

    @ui.page("/music")
    def music_page():
        """Music management page."""
        ui.dark_mode(True)

        # Get music state and manager
        from ..music import MusicManager, get_music_state

        music_state = get_music_state()

        # Try to get music directory from watcher
        watcher = get_watcher()
        music_dir = getattr(watcher, "music_dir", None) if watcher else None

        with ui.header().classes("items-center justify-between"):
            ui.label("Music Library").classes("text-xl font-bold")
            with ui.row().classes("gap-2"):
                ui.button("Refresh", on_click=lambda: _refresh_music_ui(tracks_container, music_dir, music_state)).props("flat")
                ui.button("Back", on_click=lambda: ui.navigate.to("/")).props("flat")

        # Bulk actions
        with ui.row().classes("gap-2 mt-4"):
            ui.button("Reset All Weights", on_click=lambda: _reset_all_weights(music_state, tracks_container, music_dir)).props("flat")
            ui.button("Enable All", on_click=lambda: _enable_all_tracks(music_state, tracks_container, music_dir)).props("flat color=positive")
            ui.button("Disable All", on_click=lambda: _disable_all_tracks(music_state, tracks_container, music_dir)).props("flat color=negative")

        ui.separator()

        # Tracks list
        tracks_container = ui.column().classes("w-full gap-2 mt-2")

        def render_tracks():
            tracks_container.clear()
            if not music_dir:
                with tracks_container:
                    ui.label("No music directory configured.").classes("text-gray-400")
                return

            try:
                manager = MusicManager(music_dir)
                tracks = manager.tracks
            except FileNotFoundError:
                with tracks_container:
                    ui.label(f"Music directory not found: {music_dir}").classes("text-red-400")
                return

            if not tracks:
                with tracks_container:
                    ui.label("No tracks found in database.").classes("text-gray-400")
                return

            # Get all weights
            weights = music_state.snapshot()

            with tracks_container:
                # Header row
                with ui.row().classes("w-full items-center gap-4 font-bold text-gray-400 px-2"):
                    ui.label("Enabled").classes("w-16")
                    ui.label("Track").classes("flex-1")
                    ui.label("Drop").classes("w-16 text-right")
                    ui.label("Weight").classes("w-40")
                    ui.label("Used").classes("w-16 text-right")

                for track in tracks:
                    tw = weights.get(track.id, {})
                    enabled = tw.get("enabled", True)
                    weight = tw.get("weight", 1.0)
                    use_count = tw.get("use_count", 0)

                    with ui.row().classes("w-full items-center gap-4 bg-gray-800 rounded px-2 py-2"):
                        # Enable checkbox
                        cb = ui.checkbox(value=enabled).classes("w-16")
                        cb.on_value_change(lambda e, tid=track.id: _toggle_track_enabled(tid, e.value, music_state))

                        # Track info
                        with ui.column().classes("flex-1"):
                            ui.label(track.title or track.id).classes("font-bold")
                            ui.label(track.id).classes("text-xs text-gray-400")

                        # Drop time
                        ui.label(f"{track.drop_time_seconds:.1f}s").classes("w-16 text-right text-gray-300")

                        # Weight slider with value display
                        with ui.row().classes("w-40 items-center gap-1"):
                            slider = ui.slider(min=0, max=3.0, step=0.1, value=weight).classes("flex-1")
                            weight_label = ui.label(f"{weight:.1f}").classes("w-8 text-right")
                            slider.on_value_change(lambda e, tid=track.id, lbl=weight_label: (
                                _set_track_weight(tid, e.value, music_state),
                                lbl.set_text(f"{e.value:.1f}")
                            ))

                        # Use count
                        ui.label(str(use_count)).classes("w-16 text-right text-gray-400")

        render_tracks()

        # Navigation
        ui.separator().classes("mt-4")
        with ui.row().classes("gap-4"):
            ui.link("DASHBOARD", "/").classes("text-blue-400")
            ui.link("MUSIC", "/music").classes("text-blue-400")
            ui.link("HISTORY", "/history").classes("text-blue-400")
            ui.link("ERRORS", "/errors").classes("text-blue-400")
            ui.link("SETTINGS", "/settings").classes("text-blue-400")

    @ui.page("/history")
    def history_page():
        """Full processing history page."""
        ui.dark_mode(True)

        with ui.header().classes("items-center justify-between"):
            ui.label("Processing History").classes("text-xl font-bold")
            ui.button("Back", on_click=lambda: ui.navigate.to("/")).props("flat")

        history_container = ui.column().classes("w-full gap-1 mt-4")

        def refresh():
            snapshot = state.snapshot()
            history_container.clear()
            with history_container:
                all_jobs = snapshot["completed_history"]
                if not all_jobs:
                    ui.label("No processing history yet.").classes("text-gray-400")
                for job in all_jobs:
                    with ui.card().classes("w-full"):
                        with ui.row().classes("items-center gap-2"):
                            if job["status"] == "COMPLETED":
                                ui.icon("check", color="green")
                            elif job["status"] == "SKIPPED":
                                ui.icon("skip_next", color="orange")
                            else:
                                ui.icon("help", color="grey")
                            ui.label(job["dir_name"]).classes("font-mono font-bold")
                        if job.get("video_url"):
                            ui.link(job["video_url"], job["video_url"]).classes("text-blue-400")
                        with ui.row().classes("gap-4 text-sm text-gray-400"):
                            if job.get("template_used"):
                                ui.label(f"Template: {job['template_used']}")
                            if job.get("music_track"):
                                ui.label(f"Music: {job['music_track']}")

        ui.timer(2.0, refresh)
        refresh()

        # Navigation
        ui.separator().classes("mt-4")
        with ui.row().classes("gap-4"):
            ui.link("DASHBOARD", "/").classes("text-blue-400")
            ui.link("MUSIC", "/music").classes("text-blue-400")
            ui.link("HISTORY", "/history").classes("text-blue-400")
            ui.link("ERRORS", "/errors").classes("text-blue-400")
            ui.link("SETTINGS", "/settings").classes("text-blue-400")

    @ui.page("/errors")
    def errors_page():
        """Error dashboard page."""
        ui.dark_mode(True)

        with ui.header().classes("items-center justify-between"):
            ui.label("Error Dashboard").classes("text-xl font-bold")
            with ui.row().classes("gap-2"):
                ui.button("Clear All", on_click=_clear_all_errors).props("flat color=red")
                ui.button("Back", on_click=lambda: ui.navigate.to("/")).props("flat")

        # Auth error alert
        auth_alert = ui.card().classes("bg-red-900 w-full mt-4 hidden")
        with auth_alert:
            with ui.row().classes("items-center justify-between w-full"):
                ui.icon("warning", color="red").classes("text-2xl")
                auth_msg = ui.label().classes("flex-1")
                ui.button("Re-authenticate", on_click=_reauthenticate).props("flat color=white")
                ui.button("Dismiss", on_click=lambda: state.clear_auth_error()).props(
                    "flat color=white"
                )

        ui.label("Failed Jobs").classes("text-lg font-semibold mt-4")
        failed_container = ui.column().classes("w-full gap-2")

        def refresh():
            snapshot = state.snapshot()

            # Auth alert
            if snapshot["auth_error"]:
                auth_alert.classes(remove="hidden")
                auth_msg.text = f"Authentication failed: {snapshot['auth_error']}"
            else:
                auth_alert.classes(add="hidden")

            # Failed jobs
            failed_container.clear()
            with failed_container:
                failed_jobs = snapshot["failed_history"]
                if not failed_jobs:
                    ui.label("No failed jobs.").classes("text-gray-400")
                for job in failed_jobs:
                    with ui.card().classes("w-full"):
                        with ui.row().classes("items-center justify-between w-full"):
                            with ui.column().classes("flex-1"):
                                ui.label(job["dir_name"]).classes("font-mono font-bold")
                                ui.label(job.get("error", "Unknown error")).classes(
                                    "text-sm text-red-400"
                                )
                                category = job.get("error_category", "UNKNOWN")
                                ui.badge(category, color="red" if category == "SYSTEMIC" else "orange")
                            with ui.row().classes("gap-2"):
                                ui.button(
                                    "Retry",
                                    on_click=lambda d=job["dir_name"]: _retry_job(d),
                                ).props("flat")
                                ui.button(
                                    "Skip",
                                    on_click=lambda d=job["dir_name"]: _skip_job(d),
                                ).props("flat color=grey")

        ui.timer(2.0, refresh)
        refresh()

        # Navigation
        ui.separator().classes("mt-4")
        with ui.row().classes("gap-4"):
            ui.link("DASHBOARD", "/").classes("text-blue-400")
            ui.link("MUSIC", "/music").classes("text-blue-400")
            ui.link("HISTORY", "/history").classes("text-blue-400")
            ui.link("ERRORS", "/errors").classes("text-blue-400")
            ui.link("SETTINGS", "/settings").classes("text-blue-400")

    @ui.page("/settings")
    def settings_page():
        """Runtime settings page."""
        ui.dark_mode(True)

        with ui.header().classes("items-center justify-between"):
            ui.label("Settings").classes("text-xl font-bold")
            ui.button("Back", on_click=lambda: ui.navigate.to("/")).props("flat")

        # Current settings
        snapshot = state.snapshot()
        settings = snapshot["settings"]

        ui.label("Upload Settings").classes("text-lg font-semibold mt-4")
        with ui.card().classes("w-full"):
            upload_delay = ui.number(
                "Upload Delay (seconds)",
                value=settings["upload_delay"],
                min=0,
                max=7200,
            ).classes("w-full")

            privacy = ui.select(
                options=["private", "unlisted", "public"],
                value=settings["privacy"],
                label="Privacy",
            ).classes("w-full")

            playlist_id = ui.input(
                "Playlist ID",
                value=settings["playlist_id"] or "",
            ).classes("w-full")

        ui.label("Watcher Settings").classes("text-lg font-semibold mt-4")
        with ui.card().classes("w-full"):
            poll_interval = ui.number(
                "Poll Interval (seconds)",
                value=settings["poll_interval"],
                min=1,
                max=60,
            ).classes("w-full")

            settle_time = ui.number(
                "Settle Time (seconds)",
                value=settings["settle_time"],
                min=1,
                max=120,
            ).classes("w-full")

            delete_after = ui.checkbox(
                "Delete after upload",
                value=settings["delete_after_upload"],
            )

        def apply_settings():
            state.update_settings(
                upload_delay=upload_delay.value,
                privacy=privacy.value,
                playlist_id=playlist_id.value or None,
                poll_interval=poll_interval.value,
                settle_time=settle_time.value,
                delete_after_upload=delete_after.value,
            )
            ui.notify("Settings applied", type="positive")

        ui.button("Apply Changes", on_click=apply_settings).classes("mt-4")

        # Navigation
        ui.separator().classes("mt-4")
        with ui.row().classes("gap-4"):
            ui.link("DASHBOARD", "/").classes("text-blue-400")
            ui.link("MUSIC", "/music").classes("text-blue-400")
            ui.link("HISTORY", "/history").classes("text-blue-400")
            ui.link("ERRORS", "/errors").classes("text-blue-400")
            ui.link("SETTINGS", "/settings").classes("text-blue-400")


def _toggle_pause():
    """Toggle pause state."""
    state = get_state()
    watcher = get_watcher()
    if watcher:
        if watcher.is_paused():
            watcher.resume()
            ui.notify("Resumed", type="positive")
        else:
            watcher.pause()
            ui.notify("Paused", type="warning")
    else:
        state.paused = not state.paused
        ui.notify("Paused" if state.paused else "Resumed", type="info")


def _reauthenticate():
    """Trigger re-authentication."""
    watcher = get_watcher()
    if watcher:
        ui.notify("Authenticating... Please check your browser if prompted.", type="info")
        if watcher.trigger_reauth():
            ui.notify("Authentication successful!", type="positive")
        else:
            ui.notify("Authentication failed. Check credentials.", type="negative")
    else:
        ui.notify("Watcher not running", type="warning")


def _refresh_music():
    """Refresh music library (legacy - use _refresh_music_ui instead)."""
    ui.notify("Navigate to Music page to refresh", type="info")


def _refresh_music_ui(container, music_dir, music_state):
    """Refresh the music UI by reloading the database."""
    if not music_dir:
        ui.notify("No music directory configured", type="warning")
        return

    from ..music import MusicManager

    try:
        manager = MusicManager(music_dir)
        tracks = manager.tracks

        # Re-render the tracks
        container.clear()
        weights = music_state.snapshot()

        with container:
            # Header row
            with ui.row().classes("w-full items-center gap-4 font-bold text-gray-400 px-2"):
                ui.label("Enabled").classes("w-16")
                ui.label("Track").classes("flex-1")
                ui.label("Drop").classes("w-16 text-right")
                ui.label("Weight").classes("w-40")
                ui.label("Used").classes("w-16 text-right")

            for track in tracks:
                tw = weights.get(track.id, {})
                enabled = tw.get("enabled", True)
                weight = tw.get("weight", 1.0)
                use_count = tw.get("use_count", 0)

                with ui.row().classes("w-full items-center gap-4 bg-gray-800 rounded px-2 py-2"):
                    cb = ui.checkbox(value=enabled).classes("w-16")
                    cb.on_value_change(lambda e, tid=track.id: _toggle_track_enabled(tid, e.value, music_state))

                    with ui.column().classes("flex-1"):
                        ui.label(track.title or track.id).classes("font-bold")
                        ui.label(track.id).classes("text-xs text-gray-400")

                    ui.label(f"{track.drop_time_seconds:.1f}s").classes("w-16 text-right text-gray-300")

                    with ui.row().classes("w-40 items-center gap-1"):
                        slider = ui.slider(min=0, max=3.0, step=0.1, value=weight).classes("flex-1")
                        weight_label = ui.label(f"{weight:.1f}").classes("w-8 text-right")
                        slider.on_value_change(lambda e, tid=track.id, lbl=weight_label: (
                            _set_track_weight(tid, e.value, music_state),
                            lbl.set_text(f"{e.value:.1f}")
                        ))

                    ui.label(str(use_count)).classes("w-16 text-right text-gray-400")

        ui.notify(f"Loaded {len(tracks)} tracks", type="positive")
    except FileNotFoundError as e:
        ui.notify(f"Music directory not found: {e}", type="negative")


def _toggle_track_enabled(track_id: str, enabled: bool, music_state):
    """Toggle whether a track is enabled."""
    music_state.set_enabled(track_id, enabled)


def _set_track_weight(track_id: str, weight: float, music_state):
    """Set the weight for a track."""
    music_state.set_weight(track_id, weight)


def _reset_all_weights(music_state, container, music_dir):
    """Reset all weights to 1.0."""
    music_state.reset_all_weights()
    _refresh_music_ui(container, music_dir, music_state)
    ui.notify("All weights reset to 1.0", type="info")


def _enable_all_tracks(music_state, container, music_dir):
    """Enable all tracks."""
    music_state.enable_all()
    _refresh_music_ui(container, music_dir, music_state)
    ui.notify("All tracks enabled", type="positive")


def _disable_all_tracks(music_state, container, music_dir):
    """Disable all tracks."""
    music_state.disable_all()
    _refresh_music_ui(container, music_dir, music_state)
    ui.notify("All tracks disabled", type="warning")


def _process_now():
    """Trigger immediate processing of a pending video."""
    state = get_state()
    watcher = get_watcher()

    if not watcher:
        ui.notify("Watcher not running", type="warning")
        return

    # Check if there's a current job
    if state.get_current_job():
        ui.notify("Already processing a video", type="info")
        return

    # Check if paused
    if watcher.is_paused():
        ui.notify("Watcher is paused. Resume to process.", type="warning")
        return

    # Check pending queue
    pending = state.peek_pending()
    if not pending:
        ui.notify("No pending videos to process", type="info")
        return

    ui.notify(f"Processing will start shortly...", type="positive")


def _retry_job(dir_name: str):
    """Retry a failed job."""
    state = get_state()
    if state.retry_failed(dir_name):
        ui.notify(f"Retrying {dir_name}", type="positive")
    else:
        ui.notify(f"Could not find {dir_name}", type="negative")


def _skip_job(dir_name: str):
    """Skip a failed job."""
    state = get_state()
    if state.clear_failed(dir_name):
        ui.notify(f"Skipped {dir_name}", type="info")
    else:
        ui.notify(f"Could not find {dir_name}", type="negative")


def _clear_all_errors():
    """Clear all failed jobs."""
    state = get_state()
    count = state.clear_all_failed()
    state.clear_auth_error()
    ui.notify(f"Cleared {count} failed jobs", type="info")


def run_server(
    state: WatcherState,
    host: str = "0.0.0.0",
    port: int = 8080,
    title: str = "Pendulum Watcher",
) -> None:
    """Run the NiceGUI server.

    This function blocks until the server is shut down.
    """
    create_app(state)
    ui.run(
        host=host,
        port=port,
        title=title,
        reload=False,
        show=False,
    )
