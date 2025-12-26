"""YouTube API integration for video uploads."""

from __future__ import annotations

import pickle
from pathlib import Path
from typing import Callable, Optional

from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build
from googleapiclient.http import MediaFileUpload

# OAuth 2.0 scope for uploading videos
SCOPES = ["https://www.googleapis.com/auth/youtube.upload"]

# YouTube category IDs
CATEGORY_MUSIC = "10"
CATEGORY_SCIENCE_TECH = "28"
CATEGORY_FILM_ANIMATION = "1"
CATEGORY_EDUCATION = "27"


class YouTubeUploader:
    """Handles YouTube API authentication and video uploads."""

    def __init__(self, credentials_dir: Path):
        """Initialize the uploader with a credentials directory.

        Args:
            credentials_dir: Directory containing client_secrets.json
                           and where token.pickle will be stored.
        """
        self.credentials_dir = Path(credentials_dir)
        self.client_secrets = self.credentials_dir / "client_secrets.json"
        self.token_path = self.credentials_dir / "token.pickle"
        self.youtube = None
        self._credentials: Optional[Credentials] = None

    def authenticate(self) -> bool:
        """Authenticate with YouTube API using OAuth 2.0.

        Returns:
            True if authentication was successful.

        Raises:
            FileNotFoundError: If client_secrets.json is not found.
        """
        creds = None

        # Load cached credentials
        if self.token_path.exists():
            with open(self.token_path, "rb") as token:
                creds = pickle.load(token)

        # Refresh or get new credentials
        if not creds or not creds.valid:
            if creds and creds.expired and creds.refresh_token:
                creds.refresh(Request())
            else:
                if not self.client_secrets.exists():
                    raise FileNotFoundError(
                        f"client_secrets.json not found at {self.client_secrets}\n"
                        "Please download OAuth 2.0 credentials from Google Cloud Console."
                    )
                flow = InstalledAppFlow.from_client_secrets_file(
                    str(self.client_secrets), SCOPES
                )
                creds = flow.run_local_server(port=0)

            # Save credentials for next time
            self.credentials_dir.mkdir(parents=True, exist_ok=True)
            with open(self.token_path, "wb") as token:
                pickle.dump(creds, token)

        self._credentials = creds
        self.youtube = build("youtube", "v3", credentials=creds)
        return True

    def upload(
        self,
        video_path: Path,
        title: str,
        description: str,
        tags: list[str],
        privacy_status: str = "private",
        category_id: str = CATEGORY_MUSIC,
        progress_callback: Optional[Callable[[float], None]] = None,
    ) -> Optional[str]:
        """Upload a video to YouTube.

        Args:
            video_path: Path to the video file.
            title: Video title.
            description: Video description.
            tags: List of tags for the video.
            privacy_status: One of "private", "unlisted", or "public".
            category_id: YouTube category ID (default: Music).
            progress_callback: Optional callback for upload progress (0.0 to 1.0).

        Returns:
            Video ID on success, None on failure.

        Raises:
            RuntimeError: If not authenticated.
        """
        if not self.youtube:
            raise RuntimeError("Not authenticated. Call authenticate() first.")

        body = {
            "snippet": {
                "title": title,
                "description": description,
                "tags": tags,
                "categoryId": category_id,
            },
            "status": {
                "privacyStatus": privacy_status,
                "selfDeclaredMadeForKids": False,
            },
        }

        # Use resumable upload for reliability
        media = MediaFileUpload(
            str(video_path),
            mimetype="video/mp4",
            resumable=True,
            chunksize=10 * 1024 * 1024,  # 10MB chunks
        )

        request = self.youtube.videos().insert(
            part="snippet,status",
            body=body,
            media_body=media,
        )

        response = None
        while response is None:
            status, response = request.next_chunk()
            if status and progress_callback:
                progress_callback(status.progress())

        return response.get("id")

    @property
    def is_authenticated(self) -> bool:
        """Check if the uploader is authenticated."""
        return self.youtube is not None and self._credentials is not None
