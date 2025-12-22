"""Command-line interface for the pendulum uploader."""

from __future__ import annotations

from pathlib import Path

import click
from rich.console import Console
from rich.panel import Panel
from rich.progress import Progress, SpinnerColumn, TextColumn
from rich.table import Table

from .models import VideoMetadata
from .templates import (
    generate_all_titles,
    generate_description,
    generate_tags,
    generate_title,
)
from .uploader import CATEGORY_MUSIC, YouTubeUploader

console = Console()


@click.group()
@click.version_option(version="0.1.0")
def main():
    """Double Pendulum YouTube Uploader.

    Upload pendulum simulation videos to YouTube with auto-generated metadata.
    """
    pass


@main.command()
@click.argument("video_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--credentials",
    "-c",
    type=click.Path(path_type=Path),
    default=Path("credentials"),
    help="Directory containing client_secrets.json",
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
def upload(video_dir: Path, credentials: Path, privacy: str, dry_run: bool):
    """Upload a single video from VIDEO_DIR.

    VIDEO_DIR must contain metadata.json and video.mp4.
    """
    metadata_path = video_dir / "metadata.json"
    video_path = video_dir / "video.mp4"

    if not metadata_path.exists():
        console.print(f"[red]Error:[/red] metadata.json not found in {video_dir}")
        raise SystemExit(1)

    if not video_path.exists():
        console.print(f"[red]Error:[/red] video.mp4 not found in {video_dir}")
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
    console.print(f"[bold]Size:[/bold] {video_path.stat().st_size / 1024 / 1024:.1f} MB")

    if dry_run:
        console.print()
        console.print("[yellow]Dry run - not uploading[/yellow]")
        return

    # Upload
    try:
        uploader = YouTubeUploader(credentials)
        uploader.authenticate()
    except FileNotFoundError as e:
        console.print(f"\n[red]Error:[/red] {e}")
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
    default=Path("credentials"),
    help="Directory containing client_secrets.json",
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
    "--dry-run",
    is_flag=True,
    help="Show what would be uploaded without actually uploading",
)
def batch(batch_dir: Path, credentials: Path, privacy: str, limit: int, dry_run: bool):
    """Upload all videos from a batch directory.

    Finds all subdirectories containing metadata.json and video.mp4.
    """
    # Find all video directories
    video_dirs = []
    for item in batch_dir.iterdir():
        if item.is_dir():
            if (item / "metadata.json").exists() and (item / "video.mp4").exists():
                video_dirs.append(item)

    video_dirs.sort()
    if limit:
        video_dirs = video_dirs[:limit]

    if not video_dirs:
        console.print(f"[yellow]No videos found in {batch_dir}[/yellow]")
        return

    console.print(f"Found [bold]{len(video_dirs)}[/bold] videos to upload")
    console.print()

    uploader = None
    if not dry_run:
        try:
            uploader = YouTubeUploader(credentials)
            uploader.authenticate()
        except FileNotFoundError as e:
            console.print(f"[red]Error:[/red] {e}")
            raise SystemExit(1)

    results = []
    for i, video_dir in enumerate(video_dirs, 1):
        console.print(f"\n[bold]Video {i}/{len(video_dirs)}:[/bold] {video_dir.name}")

        try:
            metadata = VideoMetadata.from_file(video_dir / "metadata.json")
        except Exception as e:
            console.print(f"  [red]Error loading metadata:[/red] {e}")
            results.append((video_dir.name, None, str(e)))
            continue

        title = generate_title(metadata)
        console.print(f"  Title: {title}")

        if dry_run:
            results.append((video_dir.name, "dry-run", title))
            continue

        description = generate_description(metadata)
        tags = generate_tags(metadata)

        try:
            video_id = uploader.upload(
                video_path=video_dir / "video.mp4",
                title=title,
                description=description,
                tags=tags,
                privacy_status=privacy,
                category_id=CATEGORY_MUSIC,
            )
            if video_id:
                url = f"https://youtu.be/{video_id}"
                console.print(f"  [green]Uploaded:[/green] {url}")
                results.append((video_dir.name, video_id, url))
            else:
                console.print(f"  [red]Upload failed[/red]")
                results.append((video_dir.name, None, "Upload failed"))
        except Exception as e:
            console.print(f"  [red]Error:[/red] {e}")
            results.append((video_dir.name, None, str(e)))

    # Print summary
    console.print()
    table = Table(title="Upload Summary")
    table.add_column("Video", style="cyan")
    table.add_column("Status", style="green")
    table.add_column("URL/Error")

    for name, video_id, info in results:
        if video_id and video_id != "dry-run":
            table.add_row(name, "OK", info)
        elif video_id == "dry-run":
            table.add_row(name, "DRY RUN", info)
        else:
            table.add_row(name, "[red]FAILED[/red]", info)

    console.print(table)


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
    console.print(f"  Video: {metadata.video_duration:.1f}s at {metadata.config.video_fps}fps")
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


if __name__ == "__main__":
    main()
