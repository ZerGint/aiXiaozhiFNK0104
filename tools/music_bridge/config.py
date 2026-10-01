from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any


BRIDGE_DIR = Path(__file__).resolve().parent
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

    @property
    def generated_dir(self) -> Path:
        return BRIDGE_DIR / "generated"

    @property
    def library_dir(self) -> Path:
        return BRIDGE_DIR / "library"

    @property
    def logs_dir(self) -> Path:
        return BRIDGE_DIR / "logs"

    @property
    def jobs_path(self) -> Path:
        return BRIDGE_DIR / "jobs.json"

    @classmethod
    def load(cls) -> "BridgeConfig":
        values: dict[str, Any] = {}
        config_path = BRIDGE_DIR / "config.json"
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
        )

    def find_ffmpeg(self) -> str | None:
        configured = os.environ.get("MUSIC_BRIDGE_FFMPEG")
        candidates = [configured] if configured else []
        candidates += [
            shutil.which("ffmpeg"),
            str(self.wan_gp_root / "ffmpeg_bins" / "ffmpeg.exe"),
            str(self.wan_gp_root / "ffmpeg_bins" / "ffmpeg"),
        ]
        for candidate in candidates:
            if candidate and Path(candidate).is_file():
                return candidate
        return None
