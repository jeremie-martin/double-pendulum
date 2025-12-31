"""Command-line interface for pendulum-tools."""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

import click
from rich.console import Console
from rich.logging import RichHandler
from rich.panel import Panel
from rich.progress import Progress, SpinnerColumn, TextColumn
from rich.table import Table

from .config import UserConfig, get_config
from .constants import DEFAULT_CRF_QUALITY, DEFAULT_NVENC_CQ, FALLBACK_BOOM_SECONDS
from .models import VideoMetadata
from .processing import (
    ProcessingConfig,
    ProcessingPipeline,
    is_nvenc_available,
    load_template_system,
)
from .processing.thumbnails import extract_thumbnails
from .templates import (
    generate_all_titles,
    generate_description,
    generate_tags,
    generate_title,
)
from .uploader import CATEGORY_MUSIC, YouTubeUploader

console = Console()
logger = logging.getLogger(__name__)


def setup_logging(log_file: Optional[Path] = None, verbose: bool = False) -> None:
    """Configure logging for CLI operations.

    Args:
        log_file: Optional path to write logs to file
        verbose: If True, show DEBUG level messages
    """
    level = logging.DEBUG if verbose else logging.INFO

    handlers: list[logging.Handler] = [
        RichHandler(console=console, show_time=False, show_path=False)
    ]

    if log_file:
        log_file.parent.mkdir(parents=True, exist_ok=True)
        file_handler = logging.FileHandler(log_file)
        file_handler.setFormatter(
            logging.Formatter("%(asctime)s - %(name)s - %(levelname)s - %(message)s")
        )
        handlers.append(file_handler)

    logging.basicConfig(
        level=level,
        handlers=handlers,
        force=True,
    )


def get_video_path(video_dir: Path, for_upload: bool = False) -> Path:
    """Get the appropriate video file from a directory.

    For upload: prefers most processed version (with music > processed > with music only > raw)
    For processing: prefers video.mp4 > video_raw.mp4

    Priority for upload:
        1. video_processed_final.mp4 (processed + music)
        2. video_processed.mp4 (processed, no music)
        3. video.mp4 (with music only)
        4. video_raw.mp4 (original)
    """
    if for_upload:
        # Prefer most processed version for upload
        candidates = [
            "video_processed_final.mp4",
            "video_processed.mp4",
            "video.mp4",
            "video_raw.mp4",
        ]
    else:
        # For processing, prefer video.mp4 (may have music) > raw
        candidates = ["video.mp4", "video_raw.mp4"]

    for name in candidates:
        path = video_dir / name
        if path.exists():
            return path

    raise FileNotFoundError(f"No video file found in {video_dir}")


def get_template_names() -> list[str]:
    """Get available template names, or empty list if config not found."""
    try:
        lib, _ = load_template_system()
        return lib.list_templates()
    except FileNotFoundError:
        return []


def _add_music_to_video(
    video_dir: Path,
    video_path: Path,
    metadata: "VideoMetadata",
    music_dir: Optional[Path],
    track_id: Optional[str],
) -> bool:
    """Add music to a processed video. Returns True on success."""
    from .music import MusicManager

    # Resolve music directory from config
    user_config = get_config()
    resolved_music_dir = user_config.get_music_dir(music_dir)

    try:
        manager = MusicManager(resolved_music_dir)
    except FileNotFoundError as e:
        console.print(f"[red]Music error:[/red] {e}")
        return False

    # Get boom info - use VIDEO time (frame/fps), not simulation time
    boom_frame = metadata.results.boom_frame if metadata.results else None
    if boom_frame is None or boom_frame == 0:
        console.print("[red]Music error:[/red] No boom frame in metadata")
        return False

    video_fps = metadata.config.video_fps
    video_boom_seconds = boom_frame / video_fps

    # Select track
    if track_id:
        selected_track = manager.get_track(track_id)
        if not selected_track:
            console.print(f"[red]Music error:[/red] Track not found: {track_id}")
            return False
    else:
        selected_track = manager.pick_track_for_boom(video_boom_seconds)
        if not selected_track:
            console.print(
                f"[yellow]Warning:[/yellow] No tracks with drop > {video_boom_seconds:.1f}s, using random"
            )
            selected_track = manager.random_track()

    # Output: replace _processed.mp4 with _final.mp4
    output_path = video_path.with_name(video_path.stem + "_final.mp4")

    console.print("[bold]Adding Music:[/bold]")
    console.print(f"  Track: {selected_track.title}")
    console.print(
        f"  Boom: {video_boom_seconds:.2f}s → Drop: {selected_track.drop_time_seconds:.2f}s"
    )

    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        progress.add_task("Muxing audio...", total=None)

        success = MusicManager.mux_with_audio(
            video_path=video_path,
            audio_path=selected_track.filepath,
            output_path=output_path,
            boom_frame=boom_frame,
            drop_time_ms=selected_track.drop_time_ms,
            video_fps=video_fps,
        )

    if success:
        console.print(f"[green]Music added:[/green] {output_path.name}")
        # Update metadata
        metadata_path = video_dir / "metadata.json"
        MusicManager.update_metadata_with_music(metadata_path, selected_track)
        return True
    else:
        console.print("[red]Failed to add music[/red]")
        return False


@click.group()
@click.version_option(version="0.2.0")
def main():
    """Double Pendulum Post-Processing and Upload Tools.

    Tools for adding music, applying effects, and uploading videos to YouTube.
    """
    pass


# =============================================================================
# Music Commands
# =============================================================================


@main.group()
def music():
    """Music management commands."""
    pass


@music.command(name="list")
@click.option(
    "--music-dir",
    "-d",
    type=click.Path(exists=True, path_type=Path),
    default=None,
    help="Music database directory (default: from config or ./music)",
)
def music_list(music_dir: Optional[Path]):
    """List all available music tracks."""
    from .music import MusicManager

    # Resolve music directory from config
    config = get_config()
    resolved_music_dir = config.get_music_dir(music_dir)

    try:
        manager = MusicManager(resolved_music_dir)
    except FileNotFoundError as e:
        console.print(f"[red]Error:[/red] {e}")
        console.print(f"[dim]Searched: {resolved_music_dir}[/dim]")
        raise SystemExit(1)

    console.print()
    console.print("[bold]Available Music Tracks:[/bold]")
    console.print()

    table = Table()
    table.add_column("ID", style="cyan")
    table.add_column("Title")
    table.add_column("Drop Time", style="green")
    table.add_column("File")

    for track in manager.tracks:
        table.add_row(
            track.id,
            track.title,
            f"{track.drop_time_seconds:.1f}s",
            track.filepath.name,
        )

    console.print(table)
    console.print(f"\n[dim]Total: {len(manager.tracks)} tracks[/dim]")


@music.command(name="add")
@click.argument("video_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--track",
    "-t",
    type=str,
    default=None,
    help="Track ID (omit for auto-select based on boom timing)",
)
@click.option(
    "--music-dir",
    "-d",
    type=click.Path(exists=True, path_type=Path),
    default=None,
    help="Music database directory (default: from config or ./music)",
)
@click.option(
    "--output",
    "-o",
    type=click.Path(path_type=Path),
    default=None,
    help="Output path (default: video_dir/video.mp4)",
)
@click.option(
    "--force-random",
    is_flag=True,
    help="Skip confirmation when falling back to random track",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Show what would be done without executing",
)
def music_add(
    video_dir: Path,
    track: Optional[str],
    music_dir: Optional[Path],
    output: Optional[Path],
    force_random: bool,
    dry_run: bool,
):
    """Add music to video (lossless mux).

    Creates video.mp4 from video_raw.mp4 with music track synchronized
    to the boom moment.

    If --track is not specified, automatically selects a track where
    the music drop happens after the visual boom.
    """
    from .music import MusicManager

    # Resolve music directory from config
    user_config = get_config()
    resolved_music_dir = user_config.get_music_dir(music_dir)

    # Load metadata
    metadata_path = video_dir / "metadata.json"
    if not metadata_path.exists():
        console.print(f"[red]Error:[/red] metadata.json not found in {video_dir}")
        raise SystemExit(1)

    try:
        metadata = VideoMetadata.from_file(metadata_path)
    except Exception as e:
        console.print(f"[red]Error loading metadata:[/red] {e}")
        raise SystemExit(1)

    # Find input video
    raw_video = video_dir / "video_raw.mp4"
    if not raw_video.exists():
        # Fall back to video.mp4 if video_raw.mp4 doesn't exist
        raw_video = video_dir / "video.mp4"
        if not raw_video.exists():
            console.print(f"[red]Error:[/red] No video file found in {video_dir}")
            raise SystemExit(1)

    # Load music manager
    try:
        manager = MusicManager(resolved_music_dir)
    except FileNotFoundError as e:
        console.print(f"[red]Error:[/red] {e}")
        console.print(f"[dim]Searched: {resolved_music_dir}[/dim]")
        raise SystemExit(1)

    # Get boom info - use VIDEO time (frame/fps), not simulation time
    boom_frame = metadata.results.boom_frame if metadata.results else None
    if boom_frame is None or boom_frame == 0:
        console.print("[red]Error:[/red] No boom frame detected in metadata")
        raise SystemExit(1)

    video_fps = metadata.config.video_fps
    video_boom_seconds = boom_frame / video_fps  # Video time, not simulation time

    # Select track
    if track:
        selected_track = manager.get_track(track)
        if not selected_track:
            console.print(f"[red]Error:[/red] Track not found: {track}")
            console.print("Use 'pendulum-tools music list' to see available tracks")
            raise SystemExit(1)
    else:
        # Auto-select based on VIDEO boom timing (not simulation time)
        selected_track = manager.pick_track_for_boom(video_boom_seconds)
        if not selected_track:
            console.print(
                f"[yellow]Warning:[/yellow] No tracks with drop time > {video_boom_seconds:.1f}s"
            )
            console.print(
                "[dim]Music drop will happen before the visual boom (sync will be off)[/dim]"
            )

            if not force_random:
                if not click.confirm("Use a random track anyway?"):
                    console.print(
                        "[red]Aborted.[/red] Use --track to specify a track manually."
                    )
                    raise SystemExit(1)

            selected_track = manager.random_track()
            console.print(
                f"[yellow]Selected random track:[/yellow] {selected_track.title}"
            )

    # Determine output path
    output_path = output or (video_dir / "video.mp4")

    console.print()
    console.print("[bold]Music Addition:[/bold]")
    console.print(f"  Input: {raw_video}")
    console.print(f"  Output: {output_path}")
    console.print(f"  Track: {selected_track.title} ({selected_track.id})")
    console.print(f"  Boom: {video_boom_seconds:.2f}s (frame {boom_frame})")
    console.print(f"  Drop: {selected_track.drop_time_seconds:.2f}s")

    if dry_run:
        console.print()
        console.print("[yellow]Dry run - not executing[/yellow]")
        return

    # Mux video with audio
    console.print()
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        progress.add_task("Muxing audio...", total=None)

        success = MusicManager.mux_with_audio(
            video_path=raw_video,
            audio_path=selected_track.filepath,
            output_path=output_path,
            boom_frame=boom_frame,
            drop_time_ms=selected_track.drop_time_ms,
            video_fps=metadata.config.video_fps,
        )

    if success:
        console.print("[green]Music added successfully![/green]")
        console.print(f"  Output: {output_path}")

        # Update metadata
        MusicManager.update_metadata_with_music(metadata_path, selected_track)
        console.print("  Metadata updated")
    else:
        console.print("[red]Failed to add music[/red]")
        raise SystemExit(1)


@music.command(name="sync")
@click.argument("video_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--music-dir",
    "-d",
    type=click.Path(exists=True, path_type=Path),
    default=None,
    help="Music database directory (default: from config or ./music)",
)
def music_sync(video_dir: Path, music_dir: Optional[Path]):
    """Show which tracks are valid for this video's boom timing.

    Lists all tracks and indicates which ones have their drop
    after the visual boom (suitable for sync).
    """
    from .music import MusicManager

    # Resolve music directory from config
    user_config = get_config()
    resolved_music_dir = user_config.get_music_dir(music_dir)

    # Load metadata
    metadata_path = video_dir / "metadata.json"
    if not metadata_path.exists():
        console.print(f"[red]Error:[/red] metadata.json not found in {video_dir}")
        raise SystemExit(1)

    try:
        metadata = VideoMetadata.from_file(metadata_path)
    except Exception as e:
        console.print(f"[red]Error loading metadata:[/red] {e}")
        raise SystemExit(1)

    # Load music manager
    try:
        manager = MusicManager(resolved_music_dir)
    except FileNotFoundError as e:
        console.print(f"[red]Error:[/red] {e}")
        console.print(f"[dim]Searched: {resolved_music_dir}[/dim]")
        raise SystemExit(1)

    # Use VIDEO time (frame/fps), not simulation time
    boom_frame = metadata.results.boom_frame if metadata.results else None
    video_fps = metadata.config.video_fps
    video_boom_seconds = boom_frame / video_fps if boom_frame else 0

    console.print()
    console.print(
        f"[bold]Boom time (video):[/bold] {video_boom_seconds:.2f}s (frame {boom_frame})"
    )
    console.print()

    table = Table(title="Track Compatibility")
    table.add_column("ID", style="cyan")
    table.add_column("Title")
    table.add_column("Drop Time")
    table.add_column("Status")

    valid_count = 0
    for track in manager.tracks:
        is_valid = track.drop_time_seconds > video_boom_seconds
        status = "[green]OK[/green]" if is_valid else "[red]Too early[/red]"
        if is_valid:
            valid_count += 1

        table.add_row(
            track.id,
            track.title,
            f"{track.drop_time_seconds:.1f}s",
            status,
        )

    console.print(table)
    console.print(f"\n[dim]Valid tracks: {valid_count}/{len(manager.tracks)}[/dim]")


@main.command()
@click.argument("video_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--credentials",
    "-c",
    type=click.Path(path_type=Path),
    default=None,
    help="Directory containing client_secrets.json (default: from config or ./credentials)",
)
@click.option(
    "--privacy",
    "-p",
    type=click.Choice(["private", "unlisted", "public"]),
    default="private",
    help="Privacy status for the uploaded video",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Show what would be uploaded without actually uploading",
)
def upload(video_dir: Path, credentials: Optional[Path], privacy: str, dry_run: bool):
    """Upload a single video from VIDEO_DIR.

    VIDEO_DIR must contain metadata.json and a video file.
    Prefers processed videos: video_processed_final.mp4 > video_processed.mp4 > video.mp4
    """
    # Resolve credentials directory from config
    user_config = get_config()
    resolved_credentials = user_config.get_credentials_dir(credentials)

    metadata_path = video_dir / "metadata.json"

    if not metadata_path.exists():
        console.print(f"[red]Error:[/red] metadata.json not found in {video_dir}")
        raise SystemExit(1)

    try:
        video_path = get_video_path(video_dir, for_upload=True)
    except FileNotFoundError:
        console.print(f"[red]Error:[/red] No video file found in {video_dir}")
        raise SystemExit(1)

    # Load metadata and generate YouTube metadata
    try:
        metadata = VideoMetadata.from_file(metadata_path)
    except Exception as e:
        console.print(f"[red]Error loading metadata:[/red] {e}")
        raise SystemExit(1)

    title = generate_title(metadata)
    description = generate_description(metadata)
    tags = generate_tags(metadata)

    # Display what will be uploaded
    console.print()
    console.print(Panel(title, title="Title", border_style="blue"))
    console.print()
    console.print(Panel(description, title="Description", border_style="green"))
    console.print()
    console.print(f"[bold]Tags:[/bold] {', '.join(tags[:10])}...")
    console.print(f"[bold]Privacy:[/bold] {privacy}")
    console.print(f"[bold]Video:[/bold] {video_path}")
    console.print(
        f"[bold]Size:[/bold] {video_path.stat().st_size / 1024 / 1024:.1f} MB"
    )

    if dry_run:
        console.print()
        console.print("[yellow]Dry run - not uploading[/yellow]")
        return

    # Upload
    try:
        uploader = YouTubeUploader(resolved_credentials)
        uploader.authenticate()
    except FileNotFoundError as e:
        console.print(f"\n[red]Error:[/red] {e}")
        console.print(f"[dim]Searched: {resolved_credentials}[/dim]")
        raise SystemExit(1)

    console.print()
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task("Uploading...", total=None)

        def update_progress(pct: float):
            progress.update(task, description=f"Uploading... {pct * 100:.0f}%")

        video_id = uploader.upload(
            video_path=video_path,
            title=title,
            description=description,
            tags=tags,
            privacy_status=privacy,
            category_id=CATEGORY_MUSIC,
            progress_callback=update_progress,
        )

    if video_id:
        url = f"https://www.youtube.com/watch?v={video_id}"
        console.print()
        console.print("[green]Upload successful![/green]")
        console.print(f"[bold]URL:[/bold] {url}")
    else:
        console.print()
        console.print("[red]Upload failed[/red]")
        raise SystemExit(1)


@main.command()
@click.argument("batch_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--credentials",
    "-c",
    type=click.Path(path_type=Path),
    default=None,
    help="Directory containing client_secrets.json (default: from config or ./credentials)",
)
@click.option(
    "--privacy",
    "-p",
    type=click.Choice(["private", "unlisted", "public"]),
    default="private",
    help="Privacy status for uploaded videos",
)
@click.option(
    "--limit",
    "-n",
    type=int,
    default=None,
    help="Maximum number of videos to upload",
)
@click.option(
    "--log-file",
    type=click.Path(path_type=Path),
    default=None,
    help="Write detailed logs to file (default: batch_dir/upload_TIMESTAMP.log)",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Show what would be uploaded without actually uploading",
)
def batch(
    batch_dir: Path,
    credentials: Optional[Path],
    privacy: str,
    limit: Optional[int],
    log_file: Optional[Path],
    dry_run: bool,
):
    """Upload all videos from a batch directory.

    Finds all subdirectories containing metadata.json and a video file.
    """
    # Resolve credentials directory from config
    user_config = get_config()
    resolved_credentials = user_config.get_credentials_dir(credentials)

    # Setup logging
    if log_file is None and not dry_run:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_file = batch_dir / f"upload_{timestamp}.log"
    if log_file:
        setup_logging(log_file)
        logger.info(f"Batch upload started: {batch_dir}")

    # Find all video directories (must have metadata and a video file)
    video_dirs = []
    for item in batch_dir.iterdir():
        if item.is_dir():
            has_metadata = (item / "metadata.json").exists()
            has_video = (item / "video.mp4").exists() or (
                item / "video_raw.mp4"
            ).exists()
            if has_metadata and has_video:
                video_dirs.append(item)

    video_dirs.sort()
    if limit:
        video_dirs = video_dirs[:limit]

    if not video_dirs:
        console.print(f"[yellow]No videos found in {batch_dir}[/yellow]")
        return

    console.print(f"Found [bold]{len(video_dirs)}[/bold] videos to upload")
    if log_file:
        console.print(f"[dim]Logging to: {log_file}[/dim]")
    console.print()

    uploader = None
    if not dry_run:
        try:
            uploader = YouTubeUploader(resolved_credentials)
            uploader.authenticate()
        except FileNotFoundError as e:
            console.print(f"[red]Error:[/red] {e}")
            console.print(f"[dim]Searched: {resolved_credentials}[/dim]")
            raise SystemExit(1)

    results = []
    for i, video_dir in enumerate(video_dirs, 1):
        console.print(f"\n[bold]Video {i}/{len(video_dirs)}:[/bold] {video_dir.name}")
        logger.info(f"Processing video {i}/{len(video_dirs)}: {video_dir.name}")

        try:
            metadata = VideoMetadata.from_file(video_dir / "metadata.json")
        except Exception as e:
            error_msg = f"Error loading metadata: {e}"
            console.print(f"  [red]{error_msg}[/red]")
            logger.error(f"{video_dir.name}: {error_msg}")
            results.append((video_dir.name, None, str(e)))
            continue

        title = generate_title(metadata)

        # Find best video file
        try:
            video_path = get_video_path(video_dir, for_upload=True)
        except FileNotFoundError:
            console.print("  [red]No video file found[/red]")
            logger.error(f"{video_dir.name}: No video file found")
            results.append((video_dir.name, None, "No video file"))
            continue

        console.print(f"  Title: {title}")
        console.print(f"  Video: {video_path.name}")

        if dry_run:
            results.append((video_dir.name, "dry-run", title))
            continue

        description = generate_description(metadata)
        tags = generate_tags(metadata)

        try:
            video_id = uploader.upload(
                video_path=video_path,
                title=title,
                description=description,
                tags=tags,
                privacy_status=privacy,
                category_id=CATEGORY_MUSIC,
            )
            if video_id:
                url = f"https://youtu.be/{video_id}"
                console.print(f"  [green]Uploaded:[/green] {url}")
                logger.info(f"{video_dir.name}: Uploaded successfully - {url}")
                results.append((video_dir.name, video_id, url))
            else:
                console.print("  [red]Upload failed[/red]")
                logger.error(f"{video_dir.name}: Upload failed (no video ID returned)")
                results.append((video_dir.name, None, "Upload failed"))
        except Exception as e:
            console.print(f"  [red]Error:[/red] {e}")
            logger.error(f"{video_dir.name}: Upload error - {e}")
            results.append((video_dir.name, None, str(e)))

    # Print summary
    console.print()
    table = Table(title="Upload Summary")
    table.add_column("Video", style="cyan")
    table.add_column("Status", style="green")
    table.add_column("URL/Error")

    succeeded = 0
    failed = 0
    for name, video_id, info in results:
        if video_id and video_id != "dry-run":
            table.add_row(name, "OK", info)
            succeeded += 1
        elif video_id == "dry-run":
            table.add_row(name, "DRY RUN", info)
        else:
            table.add_row(name, "[red]FAILED[/red]", info)
            failed += 1

    console.print(table)

    # Log final summary
    if log_file:
        logger.info(
            f"Batch upload completed: {succeeded} succeeded, {failed} failed out of {len(results)} total"
        )


@main.command()
@click.argument("video_dir", type=click.Path(exists=True, path_type=Path))
def preview(video_dir: Path):
    """Preview generated metadata for a video without uploading.

    Shows all title variations and the generated description/tags.
    """
    metadata_path = video_dir / "metadata.json"

    if not metadata_path.exists():
        console.print(f"[red]Error:[/red] metadata.json not found in {video_dir}")
        raise SystemExit(1)

    try:
        metadata = VideoMetadata.from_file(metadata_path)
    except Exception as e:
        console.print(f"[red]Error loading metadata:[/red] {e}")
        raise SystemExit(1)

    # Show simulation info
    console.print()
    console.print("[bold]Simulation Info:[/bold]")
    console.print(f"  Pendulums: {metadata.config.pendulum_count:,}")
    console.print(f"  Duration: {metadata.config.duration_seconds:.1f}s physics")
    console.print(
        f"  Video: {metadata.video_duration:.1f}s at {metadata.config.video_fps}fps"
    )
    console.print(f"  Speed: {metadata.simulation_speed:.1f}x")
    console.print(f"  Resolution: {metadata.config.width}x{metadata.config.height}")
    if metadata.boom_seconds:
        console.print(f"  Boom at: {metadata.boom_seconds:.1f}s")

    # Show all title options
    console.print()
    console.print("[bold]Title Options:[/bold]")
    for i, title in enumerate(generate_all_titles(metadata), 1):
        console.print(f"  {i}. {title}")

    # Show description
    console.print()
    console.print(Panel(generate_description(metadata), title="Sample Description"))

    # Show tags
    console.print()
    tags = generate_tags(metadata)
    console.print(f"[bold]Tags ({len(tags)}):[/bold]")
    console.print(f"  {', '.join(tags)}")


@main.command(name="list-templates")
def list_templates():
    """List all available processing templates.

    Templates define motion effects (zoom, punch, shake) and caption timing.
    """
    try:
        lib, pools = load_template_system()
    except FileNotFoundError as e:
        console.print(f"[red]Error:[/red] {e}")
        raise SystemExit(1)

    console.print()
    console.print("[bold]Available Templates:[/bold]")
    console.print()

    table = Table()
    table.add_column("Name", style="cyan")
    table.add_column("Description")
    table.add_column("Motion", style="green")
    table.add_column("Captions", style="yellow")

    for name in lib.list_templates():
        template = lib.get(name)

        # Describe motion
        motion_parts = []
        if template.motion:
            if template.motion.slow_zoom:
                motion_parts.append("zoom")
            if template.motion.boom_punch:
                motion_parts.append("punch")
            if template.motion.shake:
                motion_parts.append("shake")
        motion_str = ", ".join(motion_parts) if motion_parts else "none"

        # Count captions
        caption_count = len(template.captions)

        table.add_row(name, template.description, motion_str, str(caption_count))

    console.print(table)

    console.print()
    console.print("[bold]Available Text Pools:[/bold]")
    console.print(f"  {', '.join(pools.list_pools())}")


@main.command()
@click.argument("video_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--output",
    "-o",
    type=click.Path(path_type=Path),
    default=None,
    help="Output directory for auxiliary files (default: video_dir/)",
)
@click.option(
    "--template",
    "-t",
    type=str,
    default="minimal_science",
    help="Template name (use 'random' for random selection, run 'list-templates' command to see options)",
)
@click.option(
    "--seed",
    type=int,
    default=None,
    help="Random seed for reproducible text selection",
)
@click.option(
    "--shorts",
    is_flag=True,
    help="Format for YouTube Shorts (pad to 9:16)",
)
@click.option(
    "--blur-bg",
    is_flag=True,
    help="Use blurred video as background instead of black bars (Shorts only)",
)
@click.option(
    "--blur-strength",
    type=click.IntRange(5, 100),
    default=None,
    help="Blur strength for Shorts background (5-100, default=50)",
)
@click.option(
    "--bg-brightness",
    type=click.FloatRange(0.0, 1.0),
    default=None,
    help="Background brightness for Shorts (0.0-1.0, default=1.0)",
)
@click.option(
    "--no-thumbnail",
    is_flag=True,
    help="Skip thumbnail extraction",
)
@click.option(
    "--quality",
    type=click.IntRange(0, 51),
    default=DEFAULT_CRF_QUALITY,
    help=f"CRF quality for libx264 (0-51, lower is better, default={DEFAULT_CRF_QUALITY})",
)
@click.option(
    "--nvenc-cq",
    type=click.IntRange(0, 51),
    default=DEFAULT_NVENC_CQ,
    help=f"Quality for NVENC encoder (0-51, lower is better, default={DEFAULT_NVENC_CQ})",
)
@click.option(
    "--force",
    is_flag=True,
    help="Overwrite existing processed output",
)
@click.option(
    "--no-nvenc",
    is_flag=True,
    help="Disable NVIDIA hardware encoding (use CPU libx264)",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Show FFmpeg command without executing",
)
@click.option(
    "--music",
    is_flag=True,
    help="Add music to the processed video",
)
@click.option(
    "--music-dir",
    type=click.Path(exists=True, path_type=Path),
    default=None,
    help="Music database directory (default: from config or ./music)",
)
@click.option(
    "--track",
    type=str,
    default=None,
    help="Specific track ID for music (default: auto-select based on boom timing)",
)
@click.option(
    "--zoom-start",
    type=float,
    default=None,
    help="Override slow zoom start scale (e.g., 1.0)",
)
@click.option(
    "--zoom-end",
    type=float,
    default=None,
    help="Override slow zoom end scale (e.g., 1.1)",
)
def process(
    video_dir: Path,
    output: Optional[Path],
    template: str,
    seed: Optional[int],
    shorts: bool,
    blur_bg: bool,
    blur_strength: Optional[int],
    bg_brightness: Optional[float],
    no_thumbnail: bool,
    quality: int,
    nvenc_cq: int,
    force: bool,
    no_nvenc: bool,
    dry_run: bool,
    music: bool,
    music_dir: Optional[Path],
    track: Optional[str],
    zoom_start: Optional[float],
    zoom_end: Optional[float],
):
    """Process video with motion effects and text overlays.

    Reads video.mp4 (or video_raw.mp4) and outputs video_processed.mp4.

    Templates define motion effects (slow zoom, boom punch, shake) and
    caption timing with randomly selected text from pools.

    Examples:

        # Basic processing with default template
        pendulum-tools process /path/to/video_0000

        # Full Shorts treatment with blurred background
        pendulum-tools process /path/to/video_0000 --shorts --blur-bg -t hype_mrbeast

        # Custom NVENC quality
        pendulum-tools process /path/to/video_0000 --nvenc-cq 18

        # Use CPU encoding with custom quality
        pendulum-tools process /path/to/video_0000 --no-nvenc --quality 15

        # Preview FFmpeg command
        pendulum-tools process /path/to/video_0000 --dry-run
    """
    # Build processing config
    config_kwargs = {
        "template": template,
        "seed": seed,
        "shorts": shorts,
        "blurred_background": blur_bg,
        "extract_thumbnails": not no_thumbnail,
        "crf_quality": quality,
        "nvenc_cq": nvenc_cq,
        "use_nvenc": not no_nvenc,
        "slow_zoom_start": zoom_start,
        "slow_zoom_end": zoom_end,
    }
    if blur_strength is not None:
        config_kwargs["blur_strength"] = blur_strength
    if bg_brightness is not None:
        config_kwargs["background_brightness"] = bg_brightness
    config = ProcessingConfig(**config_kwargs)

    # Initialize pipeline
    try:
        pipeline = ProcessingPipeline(video_dir, config)
    except FileNotFoundError as e:
        console.print(f"[red]Error:[/red] {e}")
        raise SystemExit(1)

    # Show preview info
    console.print()
    console.print("[bold]Processing Configuration:[/bold]")
    console.print(f"  Input: {video_dir}")
    console.print(f"  Template: {template}")
    if seed is not None:
        console.print(f"  Seed: {seed}")
    console.print(f"  Shorts mode: {'Yes' if shorts else 'No'}")
    if shorts:
        console.print(f"  Blurred BG: {'Yes' if blur_bg else 'No'}")
    # Show effective encoder (considering NVENC availability)
    effective_nvenc = config.use_nvenc and is_nvenc_available()
    if config.use_nvenc and not effective_nvenc:
        console.print("  Encoder: libx264 (CPU) [dim](NVENC not available)[/dim]")
    else:
        console.print(
            f"  Encoder: {'NVENC (GPU)' if effective_nvenc else 'libx264 (CPU)'}"
        )
    console.print(f"  Quality: {config.nvenc_cq if effective_nvenc else quality}")

    if pipeline.metadata.boom_seconds:
        console.print(f"  Boom at: {pipeline.metadata.boom_seconds:.2f}s")
    else:
        console.print(
            f"  [yellow]Warning: No boom detected, using fallback time of {FALLBACK_BOOM_SECONDS}s[/yellow]"
        )
        console.print(
            "  [dim]Motion effects may not sync correctly with visual content[/dim]"
        )

    if pipeline.metadata.scores:
        console.print(
            f"  Causticness score: {pipeline.metadata.scores.causticness or 0:.4f}"
        )

    console.print()

    # Run processing
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task("Processing video...", total=None)

        result = pipeline.run(output_dir=output, dry_run=dry_run, force=force)

    # Show results
    if dry_run:
        console.print("[bold]FFmpeg Command (dry run):[/bold]")
        console.print()
        console.print(Panel(result.ffmpeg_command or "", title="FFmpeg"))
        console.print()
        console.print(f"[bold]Template used:[/bold] {result.template_used}")
        if result.captions_text:
            console.print("[bold]Captions:[/bold]")
            for text in result.captions_text:
                console.print(f"  - {text}")
        return

    if result.success:
        # Save processing params to metadata
        result.save_to_metadata(video_dir / "metadata.json")
        console.print("[green]Processing complete![/green]")
        console.print(f"  Output: {result.output_dir}")
        console.print(f"  Template: {result.template_used}")
        if result.video_path:
            size_mb = result.video_path.stat().st_size / 1024 / 1024
            console.print(f"  Video: {result.video_path.name} ({size_mb:.1f} MB)")
        if result.thumbnails:
            console.print(f"  Thumbnails: {len(result.thumbnails)} extracted")
            for thumb in result.thumbnails:
                console.print(f"    - {thumb.name}")
        if result.captions_text:
            console.print("  Captions:")
            for text in result.captions_text:
                console.print(f"    - {text}")

        # Add music if requested
        if music and result.video_path:
            console.print()
            _add_music_to_video(
                video_dir=video_dir,
                video_path=result.video_path,
                metadata=pipeline.metadata,
                music_dir=music_dir,
                track_id=track,
            )
    else:
        console.print(f"[red]Processing failed:[/red] {result.error}")
        if result.ffmpeg_command:
            console.print()
            console.print("[dim]FFmpeg command was:[/dim]")
            console.print(result.ffmpeg_command)
        raise SystemExit(1)


@main.command()
@click.argument("video_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--output",
    "-o",
    type=click.Path(path_type=Path),
    default=None,
    help="Output directory (default: video_dir/)",
)
@click.option(
    "--timestamps",
    "-t",
    type=str,
    default="pre_boom,boom,best",
    help="Comma-separated list of timestamps: pre_boom, boom, best, or seconds",
)
def thumbnail(video_dir: Path, output: Path | None, timestamps: str):
    """Extract thumbnail frames from a video.

    Extracts frames at key moments for use as video thumbnails.

    Timestamp options:
        pre_boom - 0.5 seconds before boom
        boom     - At the boom moment
        best     - Frame with best causticness score (if available)
        <number> - Specific time in seconds (e.g., "5.5")

    Examples:

        # Extract default thumbnails (pre_boom, boom, best)
        pendulum-tools thumbnail /path/to/video_0000

        # Extract at specific timestamps
        pendulum-tools thumbnail /path/to/video_0000 -t "boom,10.5"

        # Save to custom directory
        pendulum-tools thumbnail /path/to/video_0000 -o /path/to/thumbs/
    """
    # Load metadata
    metadata_path = video_dir / "metadata.json"

    if not metadata_path.exists():
        console.print(f"[red]Error:[/red] metadata.json not found in {video_dir}")
        raise SystemExit(1)

    # Find video file (prefer video.mp4, fallback to video_raw.mp4)
    try:
        video_path = get_video_path(video_dir)
    except FileNotFoundError:
        console.print(f"[red]Error:[/red] No video file found in {video_dir}")
        raise SystemExit(1)

    try:
        metadata = VideoMetadata.from_file(metadata_path)
    except Exception as e:
        console.print(f"[red]Error loading metadata:[/red] {e}")
        raise SystemExit(1)

    output_dir = output or video_dir

    console.print()
    console.print("[bold]Extracting thumbnails:[/bold]")
    console.print(f"  Video: {video_path}")
    console.print(f"  Output: {output_dir}")

    # Parse timestamps
    timestamp_list = [t.strip() for t in timestamps.split(",")]

    # Extract thumbnails
    try:
        thumbs = extract_thumbnails(
            video_path,
            output_dir,
            boom_seconds=metadata.boom_seconds
            if any(t in ["pre_boom", "boom"] for t in timestamp_list)
            else None,
            best_frame_seconds=metadata.best_frame_seconds
            if "best" in timestamp_list
            else None,
            video_duration=metadata.video_duration,
        )

        console.print()
        console.print(f"[green]Extracted {len(thumbs)} thumbnails:[/green]")
        for thumb in thumbs:
            console.print(f"  - {thumb.name}")

    except Exception as e:
        console.print(f"[red]Error extracting thumbnails:[/red] {e}")
        raise SystemExit(1)


@main.command(name="batch-process")
@click.argument("batch_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--template",
    "-t",
    type=str,
    default="random",
    help="Template name ('random' = different template per video)",
)
@click.option(
    "--shorts",
    is_flag=True,
    help="Format for YouTube Shorts (pad to 9:16)",
)
@click.option(
    "--blur-bg",
    is_flag=True,
    help="Use blurred video as background instead of black bars",
)
@click.option(
    "--limit",
    "-n",
    type=int,
    default=None,
    help="Maximum number of videos to process",
)
@click.option(
    "--nvenc-cq",
    type=click.IntRange(0, 51),
    default=DEFAULT_NVENC_CQ,
    help=f"Quality for NVENC encoder (0-51, lower is better, default={DEFAULT_NVENC_CQ})",
)
@click.option(
    "--force",
    is_flag=True,
    help="Overwrite existing processed output",
)
@click.option(
    "--no-nvenc",
    is_flag=True,
    help="Disable NVIDIA hardware encoding (use CPU libx264)",
)
@click.option(
    "--log-file",
    type=click.Path(path_type=Path),
    default=None,
    help="Write detailed logs to file (default: batch_dir/process_TIMESTAMP.log)",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Show what would be processed without executing",
)
def batch_process(
    batch_dir: Path,
    template: str,
    shorts: bool,
    blur_bg: bool,
    limit: Optional[int],
    nvenc_cq: int,
    force: bool,
    no_nvenc: bool,
    log_file: Optional[Path],
    dry_run: bool,
):
    """Process all videos in a batch directory.

    Finds all subdirectories containing video.mp4 (or video_raw.mp4) and
    metadata.json, and processes each with the specified options.
    Outputs video_processed.mp4 in each video directory.

    Examples:

        # Process entire batch for Shorts with blurred background
        pendulum-tools batch-process /path/to/batch_output --shorts --blur-bg

        # Process first 5 videos with random templates
        pendulum-tools batch-process /path/to/batch_output --limit 5 -t random

        # Use specific template for all with custom quality
        pendulum-tools batch-process /path/to/batch_output -t minimal_science --nvenc-cq 18
    """
    # Setup logging
    if log_file is None and not dry_run:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_file = batch_dir / f"process_{timestamp}.log"
    if log_file:
        setup_logging(log_file)
        logger.info(f"Batch processing started: {batch_dir}")

    # Find all video directories (must have metadata and a video file)
    video_dirs = []
    for item in batch_dir.iterdir():
        if item.is_dir():
            has_metadata = (item / "metadata.json").exists()
            has_video = (item / "video.mp4").exists() or (
                item / "video_raw.mp4"
            ).exists()
            if has_metadata and has_video:
                video_dirs.append(item)

    video_dirs.sort()
    if limit:
        video_dirs = video_dirs[:limit]

    if not video_dirs:
        console.print(f"[yellow]No videos found in {batch_dir}[/yellow]")
        return

    console.print(f"Found [bold]{len(video_dirs)}[/bold] videos to process")
    console.print(f"  Template: {template}")
    console.print(f"  Shorts: {'Yes' if shorts else 'No'}")
    console.print(f"  Blurred BG: {'Yes' if blur_bg else 'No'}")
    console.print(f"  Encoder: {'NVENC (GPU)' if not no_nvenc else 'libx264 (CPU)'}")
    if log_file:
        console.print(f"  [dim]Logging to: {log_file}[/dim]")
    console.print()

    results = []
    for i, video_dir in enumerate(video_dirs, 1):
        console.print(f"[bold]Video {i}/{len(video_dirs)}:[/bold] {video_dir.name}")
        logger.info(f"Processing video {i}/{len(video_dirs)}: {video_dir.name}")

        # Use video index as seed for reproducible random selection
        config = ProcessingConfig(
            template=template,
            seed=i if template == "random" else None,
            shorts=shorts,
            blurred_background=blur_bg,
            extract_thumbnails=True,
            nvenc_cq=nvenc_cq,
            use_nvenc=not no_nvenc,
        )

        try:
            pipeline = ProcessingPipeline(video_dir, config)
            result = pipeline.run(dry_run=dry_run, force=force)

            if result.success:
                if not dry_run:
                    result.save_to_metadata(video_dir / "metadata.json")
                status = "DRY RUN" if dry_run else "OK"
                console.print(
                    f"  [green]{status}[/green] (template: {result.template_used})"
                )
                logger.info(
                    f"{video_dir.name}: Processed successfully (template: {result.template_used})"
                )
                results.append((video_dir.name, True, result.template_used or template))
            else:
                console.print(f"  [red]FAILED:[/red] {result.error}")
                logger.error(f"{video_dir.name}: Processing failed - {result.error}")
                results.append((video_dir.name, False, result.error or "Unknown error"))

        except Exception as e:
            console.print(f"  [red]Error:[/red] {e}")
            logger.error(f"{video_dir.name}: Exception - {e}")
            results.append((video_dir.name, False, str(e)))

    # Print summary
    console.print()
    table = Table(title="Processing Summary")
    table.add_column("Video", style="cyan")
    table.add_column("Status", style="green")
    table.add_column("Template/Error")

    for name, success, info in results:
        if success:
            table.add_row(name, "OK", info)
        else:
            table.add_row(name, "[red]FAILED[/red]", info)

    console.print(table)

    # Summary stats
    succeeded = sum(1 for _, s, _ in results if s)
    failed = len(results) - succeeded
    console.print()
    console.print(f"Processed: {succeeded}/{len(results)} videos")

    # Log final summary
    if log_file:
        logger.info(
            f"Batch processing completed: {succeeded} succeeded, {failed} failed out of {len(results)} total"
        )


# =============================================================================
# Auto and Watch Commands
# =============================================================================


@dataclass
class AutoProcessResult:
    """Result from auto-processing a single video."""

    video_dir: Path
    status: str  # success, skipped, process_failed, music_failed, upload_failed, rate_limited
    video_id: Optional[str] = None
    error: Optional[str] = None

    @property
    def succeeded(self) -> bool:
        return self.status == "success"


def _auto_process_single(
    video_dir: Path,
    uploader: Optional["YouTubeUploader"],
    music_dir: Optional[Path],
    privacy: str,
    dry_run: bool,
    user_config: "UserConfig",
    log: Any,
    playlist_id: Optional[str] = None,
    delete_after_upload: bool = False,
) -> AutoProcessResult:
    """Process, add music, and upload a single video.

    Args:
        video_dir: Directory containing video and metadata
        uploader: YouTube uploader (None for dry run)
        music_dir: Music database directory
        privacy: Privacy status for upload
        dry_run: If True, don't actually upload
        user_config: User configuration
        log: Logger instance
        playlist_id: Optional playlist ID to add video to after upload
        delete_after_upload: If True, delete video directory after successful upload

    Returns:
        AutoProcessResult with status and optional error message.
    """
    import json

    from .exceptions import RateLimitError, UploadError
    from .music import MusicManager

    metadata_path = video_dir / "metadata.json"

    # Check if already uploaded
    try:
        with open(metadata_path) as f:
            meta_data = json.load(f)
        if "upload" in meta_data and meta_data["upload"].get("video_id"):
            video_id = meta_data["upload"]["video_id"]
            log.info(f"Skipping {video_dir.name}: already uploaded as {video_id}")
            return AutoProcessResult(
                video_dir, "skipped", error=f"Already uploaded: {video_id}"
            )
    except (FileNotFoundError, json.JSONDecodeError, KeyError):
        pass

    # Check if already processed (has video_processed_final.mp4)
    final_video = video_dir / "video_processed_final.mp4"
    if final_video.exists():
        log.info(f"Skipping {video_dir.name}: already has final video")
        return AutoProcessResult(video_dir, "skipped", error="Already processed")

    # Load metadata
    try:
        metadata = VideoMetadata.from_file(metadata_path)
    except Exception as e:
        log.error(f"{video_dir.name}: Failed to load metadata: {e}")
        return AutoProcessResult(video_dir, "process_failed", error=str(e))

    # Step 1: Process video
    log.info(f"Processing {video_dir.name}")
    config = ProcessingConfig(
        template="random",
        seed=hash(video_dir.name) % (2**32),  # Deterministic but varied
        shorts=user_config.processing.shorts,
        blurred_background=user_config.processing.blur_bg,
        extract_thumbnails=True,
        use_nvenc=user_config.use_nvenc,
        nvenc_cq=user_config.nvenc_cq,
    )

    try:
        pipeline = ProcessingPipeline(video_dir, config)
        result = pipeline.run(force=user_config.processing.force)

        if not result.success:
            log.error(f"{video_dir.name}: Processing failed: {result.error}")
            return AutoProcessResult(video_dir, "process_failed", error=result.error)

        result.save_to_metadata(metadata_path)
        log.info(
            f"{video_dir.name}: Processing complete (template: {result.template_used})"
        )

    except Exception as e:
        log.error(f"{video_dir.name}: Processing exception: {e}")
        return AutoProcessResult(video_dir, "process_failed", error=str(e))

    # Step 2: Add music (required for upload)
    music_added = False
    if result.video_path:
        log.info(f"Adding music to {video_dir.name}")
        try:
            # Reload metadata after processing
            metadata = VideoMetadata.from_file(metadata_path)

            # Resolve music directory
            resolved_music_dir = user_config.get_music_dir(music_dir)

            try:
                manager = MusicManager(resolved_music_dir)
            except FileNotFoundError as e:
                log.error(f"{video_dir.name}: Music directory not found: {e}")
                return AutoProcessResult(
                    video_dir, "music_failed", error=f"Music directory not found: {e}"
                )

            # Get boom info
            boom_frame = metadata.results.boom_frame if metadata.results else None
            if not boom_frame or boom_frame <= 0:
                log.error(f"{video_dir.name}: No boom frame detected, cannot add music")
                return AutoProcessResult(
                    video_dir, "music_failed", error="No boom frame detected"
                )

            video_fps = metadata.config.video_fps
            video_boom_seconds = boom_frame / video_fps

            # Select track (auto-select based on boom timing)
            selected_track = manager.pick_track_for_boom(video_boom_seconds)
            if not selected_track:
                selected_track = manager.random_track()

            if not selected_track:
                log.error(f"{video_dir.name}: No music tracks available")
                return AutoProcessResult(
                    video_dir, "music_failed", error="No music tracks available"
                )

            # Output path
            output_path = result.video_path.with_name(
                result.video_path.stem + "_final.mp4"
            )

            success = MusicManager.mux_with_audio(
                video_path=result.video_path,
                audio_path=selected_track.filepath,
                output_path=output_path,
                boom_frame=boom_frame,
                drop_time_ms=selected_track.drop_time_ms,
                video_fps=video_fps,
            )

            if success:
                MusicManager.update_metadata_with_music(metadata_path, selected_track)
                log.info(f"{video_dir.name}: Music added ({selected_track.title})")
                music_added = True
            else:
                log.error(f"{video_dir.name}: FFmpeg muxing failed")
                return AutoProcessResult(
                    video_dir, "music_failed", error="FFmpeg muxing failed"
                )

        except Exception as e:
            log.error(f"{video_dir.name}: Music error: {e}")
            return AutoProcessResult(video_dir, "music_failed", error=str(e))

    if not music_added:
        log.error(f"{video_dir.name}: Music was not added, cannot upload")
        return AutoProcessResult(video_dir, "music_failed", error="Music not added")

    if dry_run:
        log.info(f"{video_dir.name}: Dry run - would upload")
        return AutoProcessResult(video_dir, "success", video_id="dry-run")

    # Step 3: Upload
    log.info(f"Uploading {video_dir.name}")
    try:
        # Reload metadata after music addition
        metadata = VideoMetadata.from_file(metadata_path)
        video_path = get_video_path(video_dir, for_upload=True)

        title = generate_title(metadata)
        description = generate_description(metadata)
        tags = generate_tags(metadata)

        video_id = uploader.upload(
            video_path=video_path,
            title=title,
            description=description,
            tags=tags,
            privacy_status=privacy,
            category_id=CATEGORY_MUSIC,
        )

        if video_id:
            # Save upload info to metadata.json
            try:
                with open(metadata_path) as f:
                    meta_data = json.load(f)
                meta_data["upload"] = {
                    "video_id": video_id,
                    "url": f"https://youtu.be/{video_id}",
                    "privacy": privacy,
                    "uploaded_at": datetime.now().isoformat(),
                }
                with open(metadata_path, "w") as f:
                    json.dump(meta_data, f, indent=2)
                    f.write("\n")
            except Exception as e:
                log.warning(f"{video_dir.name}: Failed to save upload info: {e}")

            log.info(
                f"{video_dir.name}: Uploaded successfully: https://youtu.be/{video_id}"
            )

            # Add to playlist if configured
            if playlist_id and uploader:
                if uploader.add_to_playlist(video_id, playlist_id):
                    log.info(f"{video_dir.name}: Added to playlist {playlist_id}")
                else:
                    log.warning(
                        f"{video_dir.name}: Failed to add to playlist {playlist_id}"
                    )

            # Delete video directory if requested
            if delete_after_upload:
                import shutil

                try:
                    shutil.rmtree(video_dir)
                    log.info(f"{video_dir.name}: Deleted video directory")
                except Exception as e:
                    log.warning(f"{video_dir.name}: Failed to delete directory: {e}")

            return AutoProcessResult(video_dir, "success", video_id=video_id)
        else:
            log.error(f"{video_dir.name}: Upload returned no video ID")
            return AutoProcessResult(
                video_dir, "upload_failed", error="No video ID returned"
            )

    except RateLimitError as e:
        log.warning(f"{video_dir.name}: Rate limited: {e}")
        return AutoProcessResult(video_dir, "rate_limited", error=str(e))
    except UploadError as e:
        log.error(f"{video_dir.name}: Upload error: {e}")
        return AutoProcessResult(video_dir, "upload_failed", error=str(e))
    except Exception as e:
        log.error(f"{video_dir.name}: Upload exception: {e}")
        return AutoProcessResult(video_dir, "upload_failed", error=str(e))


def _print_auto_summary(results: list[AutoProcessResult]) -> None:
    """Print summary table for auto-process results."""
    table = Table(title="Auto-Process Summary")
    table.add_column("Video", style="cyan")
    table.add_column("Status")
    table.add_column("Info")

    counts = {
        "success": 0,
        "skipped": 0,
        "failed": 0,
        "rate_limited": 0,
        "music_failed": 0,
    }

    for r in results:
        if r.succeeded:
            status = "[green]SUCCESS[/green]"
            info = (
                f"https://youtu.be/{r.video_id}"
                if r.video_id and r.video_id != "dry-run"
                else "dry run"
            )
            counts["success"] += 1
        elif r.status == "skipped":
            status = "[yellow]SKIPPED[/yellow]"
            info = r.error or ""
            counts["skipped"] += 1
        elif r.status == "rate_limited":
            status = "[yellow]RATE LIMIT[/yellow]"
            info = r.error or ""
            counts["rate_limited"] += 1
        elif r.status == "music_failed":
            status = "[red]MUSIC FAILED[/red]"
            info = r.error or ""
            counts["music_failed"] += 1
        else:
            status = f"[red]{r.status.upper()}[/red]"
            info = r.error or ""
            counts["failed"] += 1

        table.add_row(r.video_dir.name, status, info)

    console.print(table)
    console.print()
    parts = [f"Success: {counts['success']}", f"Skipped: {counts['skipped']}"]
    if counts["rate_limited"] > 0:
        parts.append(f"Rate Limited: {counts['rate_limited']}")
    if counts["music_failed"] > 0:
        parts.append(f"Music Failed: {counts['music_failed']}")
    if counts["failed"] > 0:
        parts.append(f"Failed: {counts['failed']}")
    console.print(" | ".join(parts))


@main.command()
@click.argument("batch_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--credentials",
    "-c",
    type=click.Path(path_type=Path),
    default=None,
    help="Directory containing client_secrets.json",
)
@click.option(
    "--music-dir",
    "-m",
    type=click.Path(exists=True, path_type=Path),
    default=None,
    help="Music database directory",
)
@click.option(
    "--privacy",
    "-p",
    type=click.Choice(["private", "unlisted", "public"]),
    default="private",
    help="Privacy status for uploaded videos",
)
@click.option(
    "--limit",
    "-n",
    type=int,
    default=None,
    help="Maximum number of videos to process",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Show what would be done without executing",
)
@click.option(
    "--playlist",
    type=str,
    default=None,
    help="Playlist ID to add uploaded videos to (default: from config)",
)
@click.option(
    "--delete-after-upload",
    is_flag=True,
    default=None,
    help="Delete video directory after successful upload (default: from config)",
)
@click.option(
    "--verbose",
    "-v",
    is_flag=True,
    help="Enable verbose logging",
)
def auto(
    batch_dir: Path,
    credentials: Optional[Path],
    music_dir: Optional[Path],
    privacy: str,
    limit: Optional[int],
    dry_run: bool,
    playlist: Optional[str],
    delete_after_upload: Optional[bool],
    verbose: bool,
):
    """Auto-process all unprocessed videos: process -> music -> upload.

    Finds videos that don't have video_processed_final.mp4 and runs the
    full pipeline on each. Continues on upload failures (logs and skips).

    Uses random template, shorts mode, and blurred background by default
    (configurable via ~/.config/pendulum-tools/config.toml).

    Examples:

        # Process and upload all videos in batch
        pendulum-tools auto /path/to/batch_output --privacy public

        # Limit to 10 videos
        pendulum-tools auto /path/to/batch_output --limit 10

        # Preview without executing
        pendulum-tools auto /path/to/batch_output --dry-run
    """
    from .logging import get_logger, setup_logging

    setup_logging(verbose=verbose)
    log = get_logger("auto")

    user_config = get_config()
    resolved_credentials = user_config.get_credentials_dir(credentials)
    resolved_music_dir = user_config.get_music_dir(music_dir)

    # Resolve playlist_id: CLI > config
    resolved_playlist_id = playlist if playlist is not None else user_config.playlist_id
    # Resolve delete_after_upload: CLI > config
    resolved_delete_after_upload = (
        delete_after_upload
        if delete_after_upload is not None
        else user_config.delete_after_upload
    )

    # Find video directories that need processing
    video_dirs = []
    for item in sorted(batch_dir.iterdir()):
        if item.is_dir() and item.name.startswith("video_"):
            has_metadata = (item / "metadata.json").exists()
            has_video = (item / "video.mp4").exists() or (
                item / "video_raw.mp4"
            ).exists()
            has_final = (item / "video_processed_final.mp4").exists()

            if has_metadata and has_video and not has_final:
                video_dirs.append(item)

    if limit:
        video_dirs = video_dirs[:limit]

    if not video_dirs:
        console.print("[yellow]No unprocessed videos found[/yellow]")
        return

    console.print(f"Found [bold]{len(video_dirs)}[/bold] videos to auto-process")
    console.print(f"  Shorts: {'Yes' if user_config.processing.shorts else 'No'}")
    console.print(f"  Blur BG: {'Yes' if user_config.processing.blur_bg else 'No'}")
    console.print(f"  Privacy: {privacy}")
    if resolved_playlist_id:
        console.print(f"  Playlist: {resolved_playlist_id}")
    if resolved_delete_after_upload:
        console.print("  Delete after upload: Yes")
    log.info(f"Starting auto-process of {len(video_dirs)} videos in {batch_dir}")

    # Authenticate uploader
    uploader = None
    if not dry_run:
        try:
            uploader = YouTubeUploader(resolved_credentials)
            uploader.authenticate()
        except FileNotFoundError as e:
            console.print(f"[red]Error:[/red] {e}")
            raise SystemExit(1)

    # Process each video
    results: list[AutoProcessResult] = []
    for i, video_dir in enumerate(video_dirs, 1):
        console.print(f"\n[bold]Video {i}/{len(video_dirs)}:[/bold] {video_dir.name}")

        result = _auto_process_single(
            video_dir=video_dir,
            uploader=uploader,
            music_dir=resolved_music_dir,
            privacy=privacy,
            dry_run=dry_run,
            user_config=user_config,
            log=log,
            playlist_id=resolved_playlist_id,
            delete_after_upload=resolved_delete_after_upload,
        )
        results.append(result)

        # Display result
        if result.succeeded:
            if result.video_id and result.video_id != "dry-run":
                console.print(
                    f"  [green]SUCCESS[/green] https://youtu.be/{result.video_id}"
                )
            else:
                console.print("  [green]SUCCESS[/green] (dry run)")
        elif result.status == "skipped":
            console.print(f"  [yellow]SKIPPED[/yellow] {result.error}")
        elif result.status == "rate_limited":
            console.print(f"  [yellow]RATE LIMITED[/yellow] {result.error}")
        else:
            console.print(f"  [red]FAILED ({result.status})[/red] {result.error}")

    # Print summary
    console.print()
    _print_auto_summary(results)

    # Log summary
    counts = {}
    for r in results:
        counts[r.status] = counts.get(r.status, 0) + 1
    summary = ", ".join(f"{k}: {v}" for k, v in sorted(counts.items()))
    log.info(f"Auto-process complete: {summary}")


@main.command()
@click.argument("batch_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--credentials",
    "-c",
    type=click.Path(path_type=Path),
    default=None,
    help="Directory containing client_secrets.json",
)
@click.option(
    "--music-dir",
    "-m",
    type=click.Path(exists=True, path_type=Path),
    default=None,
    help="Music database directory",
)
@click.option(
    "--privacy",
    "-p",
    type=click.Choice(["private", "unlisted", "public"]),
    default="private",
    help="Privacy status for uploaded videos",
)
@click.option(
    "--poll-interval",
    type=float,
    default=5.0,
    help="Seconds between directory scans (default: 5.0)",
)
@click.option(
    "--settle-time",
    type=float,
    default=10.0,
    help="Seconds to wait after new directory detected before processing (default: 10.0)",
)
@click.option(
    "--upload-delay",
    type=float,
    default=60.0,
    help="Seconds to wait between uploads to avoid rate limiting (default: 60.0)",
)
@click.option(
    "--playlist",
    type=str,
    default=None,
    help="Playlist ID to add uploaded videos to (default: from config)",
)
@click.option(
    "--delete-after-upload",
    is_flag=True,
    default=None,
    help="Delete video directory after successful upload (default: from config)",
)
@click.option(
    "--verbose",
    "-v",
    is_flag=True,
    help="Enable verbose logging",
)
def watch(
    batch_dir: Path,
    credentials: Optional[Path],
    music_dir: Optional[Path],
    privacy: str,
    poll_interval: float,
    settle_time: float,
    upload_delay: float,
    playlist: Optional[str],
    delete_after_upload: Optional[bool],
    verbose: bool,
):
    """Watch batch directory and auto-process new videos as they appear.

    Monitors the batch directory for new video_XXXX folders. When a new
    folder appears with video.mp4 and metadata.json, waits for settle_time
    then runs the full pipeline (process -> music -> upload).

    Waits upload_delay seconds between uploads to avoid YouTube rate limiting.
    Gracefully handles rate limits by skipping and continuing.
    Press Ctrl+C to stop watching.

    Examples:

        # Watch and upload as public
        pendulum-tools watch /path/to/batch_output --privacy public

        # Custom timing (2 min delay between uploads)
        pendulum-tools watch /path/to/batch_output --upload-delay 120
    """
    import signal
    import time

    from .logging import get_logger, setup_logging

    setup_logging(verbose=verbose)
    log = get_logger("watch")

    user_config = get_config()
    resolved_credentials = user_config.get_credentials_dir(credentials)
    resolved_music_dir = user_config.get_music_dir(music_dir)

    # Resolve playlist_id: CLI > config
    resolved_playlist_id = playlist if playlist is not None else user_config.playlist_id
    # Resolve delete_after_upload: CLI > config
    resolved_delete_after_upload = (
        delete_after_upload
        if delete_after_upload is not None
        else user_config.delete_after_upload
    )

    # Authenticate uploader
    try:
        uploader = YouTubeUploader(resolved_credentials)
        uploader.authenticate()
    except FileNotFoundError as e:
        console.print(f"[red]Error:[/red] {e}")
        raise SystemExit(1)

    # Track processed directories
    processed: set[str] = set()
    pending: dict[str, float] = {}  # dir_name -> first_seen_time

    # Find already-processed or already-uploaded directories
    import json

    for item in batch_dir.iterdir():
        if item.is_dir() and item.name.startswith("video_"):
            # Check for final video
            if (item / "video_processed_final.mp4").exists():
                processed.add(item.name)
            # Check for upload marker in metadata
            metadata_path = item / "metadata.json"
            if metadata_path.exists():
                try:
                    with open(metadata_path) as f:
                        meta_data = json.load(f)
                    if "upload" in meta_data and meta_data["upload"].get("video_id"):
                        processed.add(item.name)
                except (json.JSONDecodeError, KeyError):
                    pass

    running = True

    def handle_sigint(signum, frame):
        nonlocal running
        console.print("\n[yellow]Stopping watcher...[/yellow]")
        running = False

    signal.signal(signal.SIGINT, handle_sigint)

    console.print(f"[bold]Watching:[/bold] {batch_dir}")
    console.print(f"  Shorts: {'Yes' if user_config.processing.shorts else 'No'}")
    console.print(f"  Blur BG: {'Yes' if user_config.processing.blur_bg else 'No'}")
    console.print(f"  Privacy: {privacy}")
    if resolved_playlist_id:
        console.print(f"  Playlist: {resolved_playlist_id}")
    if resolved_delete_after_upload:
        console.print("  Delete after upload: Yes")
    console.print(
        f"[dim]Poll: {poll_interval}s, Settle: {settle_time}s, Upload delay: {upload_delay}s[/dim]"
    )
    console.print("[dim]Press Ctrl+C to stop[/dim]")
    console.print()
    log.info(f"Starting watch on {batch_dir}")

    total_processed = 0
    total_failed = 0

    while running:
        try:
            # Scan for new directories
            current_time = time.time()

            for item in sorted(batch_dir.iterdir(), key=lambda x: x.name):
                if not item.is_dir() or not item.name.startswith("video_"):
                    continue

                dir_name = item.name

                # Skip already processed
                if dir_name in processed:
                    continue

                # Check if ready (has required files)
                has_metadata = (item / "metadata.json").exists()
                has_video = (item / "video.mp4").exists() or (
                    item / "video_raw.mp4"
                ).exists()

                if not (has_metadata and has_video):
                    continue

                # Track pending directories
                if dir_name not in pending:
                    pending[dir_name] = current_time
                    console.print(f"[cyan]New video detected:[/cyan] {dir_name}")
                    log.info(f"New video detected: {dir_name}, waiting {settle_time}s")
                    continue

                # Check if settle time has passed
                if current_time - pending[dir_name] < settle_time:
                    continue

                # Process this directory (only ONE per iteration)
                del pending[dir_name]
                console.print(f"\n[bold]Processing:[/bold] {dir_name}")

                result = _auto_process_single(
                    video_dir=item,
                    uploader=uploader,
                    music_dir=resolved_music_dir,
                    privacy=privacy,
                    dry_run=False,
                    user_config=user_config,
                    log=log,
                    playlist_id=resolved_playlist_id,
                    delete_after_upload=resolved_delete_after_upload,
                )

                processed.add(dir_name)

                if result.succeeded:
                    total_processed += 1
                    console.print(
                        f"  [green]SUCCESS[/green] https://youtu.be/{result.video_id}"
                    )
                    # Wait before next upload to avoid rate limiting
                    if upload_delay > 0 and running:
                        console.print(
                            f"[dim]Waiting {upload_delay}s before next upload...[/dim]"
                        )
                        log.info(f"Upload delay: waiting {upload_delay}s")
                        time.sleep(upload_delay)
                elif result.status == "rate_limited":
                    total_failed += 1
                    console.print("  [yellow]RATE LIMITED[/yellow] - skipping")
                else:
                    total_failed += 1
                    console.print(f"  [red]FAILED[/red] {result.error}")

                console.print(
                    f"[dim]Total: {total_processed} processed, {total_failed} failed[/dim]"
                )

                # Process only ONE video per loop iteration, then rescan
                break

            # Wait before next scan
            time.sleep(poll_interval)

        except Exception as e:
            log.error(f"Watch loop error: {e}")
            console.print(f"[red]Error:[/red] {e}")
            time.sleep(poll_interval)

    # Final summary
    console.print()
    console.print("[bold]Watch stopped.[/bold]")
    console.print(f"Processed: {total_processed}, Failed: {total_failed}")
    log.info(f"Watch stopped. Processed: {total_processed}, Failed: {total_failed}")


if __name__ == "__main__":
    main()
