from __future__ import annotations

import json
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def _bridge_dir() -> Path:
    """Select persistent bridge data when running from a PyInstaller EXE."""
    source_dir = Path(__file__).resolve().parent
    if not getattr(sys, "frozen", False):
        return source_dir

    executable_dir = Path(sys.executable).resolve().parent
    candidates = (
        executable_dir,
        executable_dir.parent / "tools" / "music_bridge",
    )
    for candidate in candidates:
        if (
            (candidate / "data" / "library").is_dir()
            or (candidate / "config" / "config.json").is_file()
            or (candidate / "library").is_dir()
            or (candidate / "config.json").is_file()
        ):
            return candidate
    return executable_dir


BRIDGE_DIR = _bridge_dir()
CONFIG_DIR = BRIDGE_DIR / "config" if (BRIDGE_DIR / "config").is_dir() else BRIDGE_DIR
DATA_DIR = BRIDGE_DIR / "data" if (BRIDGE_DIR / "data").is_dir() else BRIDGE_DIR
DEFAULT_WANGP_URL = "http://127.0.0.1:7860"
DEFAULT_WANGP_ROOT = Path(
    r"F:\games setup\StabilityMatrix-win-x64\Data\Packages\Wan2GP"
)


@dataclass(frozen=True)
class BridgeConfig:
    wan_gp_url: str = DEFAULT_WANGP_URL
    listen_host: str = "0.0.0.0"
    listen_port: int = 8765
    generation_timeout_sec: int = 1800
    poll_interval_sec: float = 2.0
    # YuE2 treats this as an upper bound.  A 30-second cap can end during
    # the instrumental intro before the first sung section.
    audio_duration: int = 320
    mp3_bitrate: str = "160k"
    keep_wav: bool = False
    wan_gp_root: Path = DEFAULT_WANGP_ROOT
    active_provider: str = "yue2"
    volume_percent: int = 70

    @property
    def generated_dir(self) -> Path:
        return DATA_DIR / "generated"

    @property
    def library_dir(self) -> Path:
        return DATA_DIR / "library"

    @property
    def user_music_dir(self) -> Path:
        return DATA_DIR / "music_library"

    @property
    def logs_dir(self) -> Path:
        return DATA_DIR / "logs"

    @property
    def jobs_path(self) -> Path:
        return DATA_DIR / "jobs.json"

    @property
    def providers_dir(self) -> Path:
        return BRIDGE_DIR / "providers"

    @property
    def config_path(self) -> Path:
        return CONFIG_DIR / "config.json"

    @classmethod
    def load(cls) -> "BridgeConfig":
        values: dict[str, Any] = {}
        config_path = CONFIG_DIR / "config.json"
        if config_path.is_file():
            # PowerShell 5 writes UTF-8 JSON with a BOM by default.
            with config_path.open("r", encoding="utf-8-sig") as handle:
                values = json.load(handle)
        root = Path(values.get("wan_gp_root", str(DEFAULT_WANGP_ROOT)))
        return cls(
            wan_gp_url=str(values.get("wan_gp_url", DEFAULT_WANGP_URL)).rstrip("/"),
            listen_host=str(values.get("listen_host", "0.0.0.0")),
            listen_port=int(values.get("listen_port", 8765)),
            generation_timeout_sec=int(values.get("generation_timeout_sec", 1800)),
            poll_interval_sec=float(values.get("poll_interval_sec", 2.0)),
            audio_duration=int(values.get("audio_duration", 320)),
            mp3_bitrate=str(values.get("mp3_bitrate", "160k")),
            keep_wav=bool(values.get("keep_wav", False)),
            wan_gp_root=root,
            active_provider=str(values.get("active_provider", "yue2")).strip().lower() or "yue2",
            volume_percent=max(0, min(100, int(values.get("volume_percent", 70)))),
        )

    def save_active_provider(self, provider_id: str) -> None:
        """Persist the provider selected in the desktop window."""
        values: dict[str, Any] = {}
        if self.config_path.is_file():
            with self.config_path.open("r", encoding="utf-8-sig") as handle:
                values = json.load(handle)
        values["active_provider"] = provider_id
        self.config_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.config_path.with_suffix(".json.tmp")
        temporary.write_text(
            json.dumps(values, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        temporary.replace(self.config_path)

    def save_volume_percent(self, volume_percent: int) -> None:
        """Persist the desktop player's volume without changing other settings."""
        values: dict[str, Any] = {}
        if self.config_path.is_file():
            with self.config_path.open("r", encoding="utf-8-sig") as handle:
                values = json.load(handle)
        values["volume_percent"] = max(0, min(100, int(volume_percent)))
        self.config_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.config_path.with_suffix(".json.tmp")
        temporary.write_text(
            json.dumps(values, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        temporary.replace(self.config_path)

    def find_ffmpeg(self) -> str | None:
        configured = os.environ.get("MUSIC_BRIDGE_FFMPEG")
        candidates = [configured] if configured else []
        candidates += [
            str(BRIDGE_DIR / "ffmpeg" / "ffmpeg.exe"),
            str(BRIDGE_DIR / "ffmpeg" / "ffmpeg"),
            shutil.which("ffmpeg"),
            str(self.wan_gp_root / "ffmpeg_bins" / "ffmpeg.exe"),
            str(self.wan_gp_root / "ffmpeg_bins" / "ffmpeg"),
        ]
        for candidate in candidates:
            if candidate and Path(candidate).is_file():
                return candidate
        return None
