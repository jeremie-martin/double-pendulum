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
    CAPTION_STYLES,
    generate_all_titles,
    generate_description,
    generate_tags,
    generate_title,
)
from .uploader import CATEGORY_MUSIC, YouTubeUploader
from .processing import ProcessingConfig, ProcessingPipeline
from .processing.thumbnails import extract_thumbnails

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


@main.command()
@click.argument("video_dir", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--output",
    "-o",
    type=click.Path(path_type=Path),
    default=None,
    help="Output directory (default: video_dir/processed/)",
)
@click.option(
    "--style",
    "-s",
    type=click.Choice(list(CAPTION_STYLES.keys()) + ["random"]),
    default="wait_for_it",
    help="Caption style for text overlays",
)
@click.option(
    "--shorts",
    is_flag=True,
    help="Format for YouTube Shorts (pad to 9:16)",
)
@click.option(
    "--zoom",
    is_flag=True,
    help="Add zoom punch-in effect around boom moment",
)
@click.option(
    "--zoom-factor",
    type=float,
    default=1.3,
    help="Maximum zoom level (1.3 = 30% zoom in)",
)
@click.option(
    "--no-thumbnail",
    is_flag=True,
    help="Skip thumbnail extraction",
)
@click.option(
    "--font",
    type=click.Path(exists=True, path_type=Path),
    default=None,
    help="Custom font file for text overlays",
)
@click.option(
    "--quality",
    type=click.IntRange(0, 51),
    default=18,
    help="CRF quality (0-51, lower is better, 18 = visually lossless)",
)
@click.option(
    "--force",
    is_flag=True,
    help="Overwrite existing processed output",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Show FFmpeg command without executing",
)
def process(
    video_dir: Path,
    output: Path | None,
    style: str,
    shorts: bool,
    zoom: bool,
    zoom_factor: float,
    no_thumbnail: bool,
    font: Path | None,
    quality: int,
    force: bool,
    dry_run: bool,
):
    """Process video with text overlays, effects, and Shorts formatting.

    Applies caption overlays, optional zoom effect, and can reformat
    for YouTube Shorts (9:16 aspect ratio with padding).

    Examples:

        # Basic processing with default style
        pendulum-upload process /path/to/video_0000

        # Full Shorts treatment
        pendulum-upload process /path/to/video_0000 --shorts --zoom --style science

        # Preview FFmpeg command
        pendulum-upload process /path/to/video_0000 --dry-run
    """
    # Build processing config
    config = ProcessingConfig(
        style=style,
        shorts=shorts,
        zoom=zoom,
        zoom_factor=zoom_factor,
        extract_thumbnails=not no_thumbnail,
        crf_quality=quality,
        font_path=font,
    )

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
    console.print(f"  Style: {style}")
    console.print(f"  Shorts mode: {'Yes' if shorts else 'No'}")
    console.print(f"  Zoom effect: {'Yes' if zoom else 'No'}")
    if zoom:
        console.print(f"  Zoom factor: {zoom_factor}x")
    console.print(f"  Quality (CRF): {quality}")

    if pipeline.metadata.boom_seconds:
        console.print(f"  Boom at: {pipeline.metadata.boom_seconds:.2f}s")
    else:
        console.print("  [yellow]Warning: No boom detected, timed effects may be skipped[/yellow]")

    if pipeline.metadata.score:
        console.print(f"  Peak causticness: {pipeline.metadata.score.peak_causticness:.4f}")

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
        return

    if result.success:
        console.print("[green]Processing complete![/green]")
        console.print(f"  Output: {result.output_dir}")
        if result.video_path:
            size_mb = result.video_path.stat().st_size / 1024 / 1024
            console.print(f"  Video: {result.video_path.name} ({size_mb:.1f} MB)")
        if result.thumbnails:
            console.print(f"  Thumbnails: {len(result.thumbnails)} extracted")
            for thumb in result.thumbnails:
                console.print(f"    - {thumb.name}")
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
        pendulum-upload thumbnail /path/to/video_0000

        # Extract at specific timestamps
        pendulum-upload thumbnail /path/to/video_0000 -t "boom,10.5"

        # Save to custom directory
        pendulum-upload thumbnail /path/to/video_0000 -o /path/to/thumbs/
    """
    # Load metadata
    metadata_path = video_dir / "metadata.json"
    video_path = video_dir / "video.mp4"

    if not metadata_path.exists():
        console.print(f"[red]Error:[/red] metadata.json not found in {video_dir}")
        raise SystemExit(1)

    if not video_path.exists():
        console.print(f"[red]Error:[/red] video.mp4 not found in {video_dir}")
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
            boom_seconds=metadata.boom_seconds if any(t in ["pre_boom", "boom"] for t in timestamp_list) else None,
            best_frame_seconds=metadata.best_frame_seconds if "best" in timestamp_list else None,
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
    "--style",
    "-s",
    type=click.Choice(list(CAPTION_STYLES.keys()) + ["random"]),
    default="random",
    help="Caption style (random = different style per video)",
)
@click.option(
    "--shorts",
    is_flag=True,
    help="Format for YouTube Shorts (pad to 9:16)",
)
@click.option(
    "--zoom",
    is_flag=True,
    help="Add zoom punch-in effect around boom moment",
)
@click.option(
    "--limit",
    "-n",
    type=int,
    default=None,
    help="Maximum number of videos to process",
)
@click.option(
    "--force",
    is_flag=True,
    help="Overwrite existing processed output",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Show what would be processed without executing",
)
def batch_process(
    batch_dir: Path,
    style: str,
    shorts: bool,
    zoom: bool,
    limit: int | None,
    force: bool,
    dry_run: bool,
):
    """Process all videos in a batch directory.

    Finds all subdirectories containing video.mp4 and metadata.json,
    and processes each with the specified options.

    Examples:

        # Process entire batch for Shorts
        pendulum-upload batch-process /path/to/batch_output --shorts --zoom

        # Process first 5 videos with random styles
        pendulum-upload batch-process /path/to/batch_output --limit 5 --style random
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

    console.print(f"Found [bold]{len(video_dirs)}[/bold] videos to process")
    console.print(f"  Style: {style}")
    console.print(f"  Shorts: {'Yes' if shorts else 'No'}")
    console.print(f"  Zoom: {'Yes' if zoom else 'No'}")
    console.print()

    results = []
    for i, video_dir in enumerate(video_dirs, 1):
        console.print(f"[bold]Video {i}/{len(video_dirs)}:[/bold] {video_dir.name}")

        config = ProcessingConfig(
            style=style,
            shorts=shorts,
            zoom=zoom,
            extract_thumbnails=True,
        )

        try:
            pipeline = ProcessingPipeline(video_dir, config)
            result = pipeline.run(dry_run=dry_run, force=force)

            if result.success:
                status = "DRY RUN" if dry_run else "OK"
                console.print(f"  [green]{status}[/green]")
                results.append((video_dir.name, True, str(result.output_dir)))
            else:
                console.print(f"  [red]FAILED:[/red] {result.error}")
                results.append((video_dir.name, False, result.error or "Unknown error"))

        except Exception as e:
            console.print(f"  [red]Error:[/red] {e}")
            results.append((video_dir.name, False, str(e)))

    # Print summary
    console.print()
    table = Table(title="Processing Summary")
    table.add_column("Video", style="cyan")
    table.add_column("Status", style="green")
    table.add_column("Output/Error")

    for name, success, info in results:
        if success:
            table.add_row(name, "OK", info)
        else:
            table.add_row(name, "[red]FAILED[/red]", info)

    console.print(table)

    # Summary stats
    succeeded = sum(1 for _, s, _ in results if s)
    console.print()
    console.print(f"Processed: {succeeded}/{len(results)} videos")


if __name__ == "__main__":
    main()
