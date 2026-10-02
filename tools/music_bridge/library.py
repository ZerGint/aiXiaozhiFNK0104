from __future__ import annotations

import json
import os
import re
import tempfile
from pathlib import Path
from threading import RLock
from typing import Any


_ILLEGAL_FILENAME = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
_RESERVED_NAMES = {"CON", "PRN", "AUX", "NUL"} | {
    f"{prefix}{number}"
    for prefix in ("COM", "LPT")
    for number in range(1, 10)
}


def sanitize_title(title: str, job_id: str, *, max_title_length: int = 100) -> str:
    """Return a Windows/FAT-safe human-readable title component."""
    value = _ILLEGAL_FILENAME.sub(" ", str(title or ""))
    value = re.sub(r"\s+", " ", value).strip(" .")
    value = value[:max_title_length].rstrip(" .")
    if not value:
        value = "Generated Track"
    if value.upper() in _RESERVED_NAMES:
        value = f"_{value}"
    return value


def track_filename(title: str, job_id: str) -> str:
    return f"{sanitize_title(title, job_id)} [{job_id[:8]}].mp3"


def _atomic_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


class MusicLibrary:
    """Persistent Bridge-side track index and metadata sidecars."""

    def __init__(self, root: Path):
        self.root = root
        self.index_path = root / "library.json"
        self._lock = RLock()
        self._tracks: dict[str, dict[str, Any]] = {}
        self._load()

    def _load(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        try:
            payload = json.loads(self.index_path.read_text(encoding="utf-8"))
            if not isinstance(payload, dict):
                raise ValueError("library index is not an object")
            tracks = payload.get("tracks", [])
            if not isinstance(tracks, list):
                raise ValueError("tracks is not a list")
            for track in tracks:
                if isinstance(track, dict) and str(track.get("id", "")):
                    self._tracks[str(track["id"])] = dict(track)
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            self._rebuild_from_sidecars()
        self._repair_all_sizes()

    def reload(self) -> None:
        """Reload the index so another bridge component's changes are visible."""
        with self._lock:
            self._tracks.clear()
            self._load()

    def _rebuild_from_sidecars(self) -> None:
        for sidecar in self.root.glob("*.json"):
            if sidecar == self.index_path:
                continue
            try:
                track = json.loads(sidecar.read_text(encoding="utf-8"))
            except (OSError, ValueError, TypeError, json.JSONDecodeError):
                continue
            if isinstance(track, dict) and str(track.get("id", "")):
                self._tracks[str(track["id"])] = self._index_record(track)
        self._write_index()

    @staticmethod
    def _index_record(track: dict[str, Any]) -> dict[str, Any]:
        """Keep the persistent index small; full metadata stays in the sidecar."""
        keys = (
            "id", "title", "filename", "provider", "created_at", "ready_at",
            "duration", "size",
        )
        return {key: track[key] for key in keys if key in track}

    def _write_index(self) -> None:
        tracks = sorted(
            self._tracks.values(),
            key=lambda item: (str(item.get("created_at", "")), str(item.get("id", ""))),
        )
        _atomic_json(self.index_path, {"version": 1, "tracks": tracks})

    def _repair_track_size_locked(self, track_id: str) -> bool:
        track = self._tracks.get(track_id)
        if not track:
            return False
        filename = str(track.get("filename", ""))
        if not filename:
            return False
        path = self.root / filename
        try:
            actual_size = path.stat().st_size
        except OSError:
            return False
        try:
            recorded_size = int(track.get("size", -1))
        except (TypeError, ValueError):
            recorded_size = -1
        changed = recorded_size != actual_size
        if changed:
            repaired = dict(track)
            repaired["size"] = actual_size
            self._tracks[track_id] = repaired

        sidecar = self.root / f"{Path(filename).stem}.json"
        try:
            full = json.loads(sidecar.read_text(encoding="utf-8"))
            if isinstance(full, dict):
                try:
                    sidecar_size = int(full.get("size", -1))
                except (TypeError, ValueError):
                    sidecar_size = -1
                if sidecar_size != actual_size:
                    full["size"] = actual_size
                    _atomic_json(sidecar, full)
                    changed = True
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            # The index remains repairable even when an optional sidecar is
            # missing or malformed.
            pass
        return changed

    def _repair_all_sizes(self) -> None:
        with self._lock:
            changed = False
            for track_id in self._tracks:
                changed = self._repair_track_size_locked(track_id) or changed
            if changed:
                self._write_index()

    def upsert(self, track: dict[str, Any]) -> None:
        track_id = str(track["id"])
        with self._lock:
            self._tracks[track_id] = self._index_record(track)
            _atomic_json(self.root / f"{Path(str(track['filename'])).stem}.json", track)
            self._write_index()

    def list_tracks(self) -> list[dict[str, Any]]:
        with self._lock:
            changed = False
            for track_id in self._tracks:
                changed = self._repair_track_size_locked(track_id) or changed
            if changed:
                self._write_index()
            tracks = sorted(
                self._tracks.values(),
                key=lambda item: str(item.get("created_at", "")),
                reverse=True,
            )
            return [dict(track) for track in tracks]

    def get(self, track_id: str) -> dict[str, Any] | None:
        with self._lock:
            track = self._tracks.get(track_id)
            if not track:
                return None
            if self._repair_track_size_locked(track_id):
                self._write_index()
                track = self._tracks[track_id]
            filename = str(track.get("filename", ""))
            sidecar = self.root / f"{Path(filename).stem}.json"
            try:
                full = json.loads(sidecar.read_text(encoding="utf-8"))
            except (OSError, ValueError, TypeError, json.JSONDecodeError):
                return dict(track)
            return dict(full) if isinstance(full, dict) else dict(track)

    def audio_path(self, track_id: str) -> Path | None:
        track = self.get(track_id)
        if not track:
            return None
        path = self.root / str(track.get("filename", ""))
        return path if path.is_file() else None
