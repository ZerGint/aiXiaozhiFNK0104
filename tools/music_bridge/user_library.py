"""Filesystem-backed library of user supplied MP3 files.

Unlike the generated library, this directory is intentionally not indexed by
generation jobs.  Files are discovered on demand so copying an MP3 into the
folder makes it available immediately.
"""

from __future__ import annotations

import hashlib
from datetime import datetime, timezone
from pathlib import Path
from threading import RLock
from typing import Any

from mutagen.mp3 import MP3


class UserMusicLibrary:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self._lock = RLock()

    def _files(self) -> list[Path]:
        with self._lock:
            return sorted(
                (path for path in self.root.rglob("*") if path.is_file() and path.suffix.lower() == ".mp3"),
                key=lambda path: str(path.relative_to(self.root)).casefold(),
            )

    def _track_id(self, path: Path) -> str:
        relative = path.relative_to(self.root).as_posix().encode("utf-8")
        return "user_" + hashlib.sha256(relative).hexdigest()[:16]

    @staticmethod
    def _tag(tags: Any, *names: str) -> str:
        if tags is None:
            return ""
        for name in names:
            value = tags.get(name)
            if value:
                if isinstance(value, (list, tuple)):
                    return str(value[0])
                return str(value)
        return ""

    def _metadata(self, path: Path) -> dict[str, Any] | None:
        try:
            stat = path.stat()
            audio = MP3(path)
            tags = audio.tags
            relative = path.relative_to(self.root).as_posix()
            title = self._tag(tags, "TIT2", "title") or path.stem
            artist = self._tag(tags, "TPE1", "artist")
            album = self._tag(tags, "TALB", "album")
            return {
                "id": self._track_id(path),
                "title": title,
                "filename": relative,
                "provider": "user",
                "source": "user_music_library",
                "created_at": datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat(),
                "size": stat.st_size,
                "duration": float(getattr(audio.info, "length", 0.0) or 0.0),
                "artist": artist,
                "album": album,
            }
        except Exception:
            # A partially copied or malformed MP3 should not break the whole
            # library response. It will become visible after the copy is done.
            return None

    def list_tracks(self, query: str = "", limit: int = 200) -> list[dict[str, Any]]:
        needle = str(query or "").casefold().strip()
        tracks: list[dict[str, Any]] = []
        for path in self._files():
            track = self._metadata(path)
            if track is None:
                continue
            haystack = " ".join((track.get("title", ""), track.get("filename", ""), track.get("id", ""), track.get("artist", ""))).casefold()
            if needle and needle not in haystack:
                continue
            tracks.append(track)
            if len(tracks) >= max(1, min(limit, 200)):
                break
        return tracks

    def get(self, track_id: str) -> dict[str, Any] | None:
        if not track_id.startswith("user_"):
            return None
        return next((track for track in self.list_tracks() if track["id"] == track_id), None)

    def audio_path(self, track_id: str) -> Path | None:
        track = self.get(track_id)
        if track is None:
            return None
        path = (self.root / str(track["filename"])).resolve()
        try:
            path.relative_to(self.root)
        except ValueError:
            return None
        return path if path.is_file() else None
