from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from tools.music_bridge.wangp_client import WanGPClient


def _configure_stdio() -> None:
    """Keep the provider JSON protocol safe on Windows console encodings."""
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if stream is None or not hasattr(stream, "reconfigure"):
            continue
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
        except (OSError, ValueError):
            # The provider must still work with redirected or PyInstaller
            # streams that do not allow reconfiguration.
            pass


def emit(event: str, **values: Any) -> None:
    payload = {"event": event, **values}
    # JSON escaping keeps the line protocol ASCII-only even if WanGP returns
    # Cyrillic text or Unicode progress symbols.
    print(json.dumps(payload, ensure_ascii=True), flush=True)


def load_config(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main() -> int:
    _configure_stdio()
    raw = sys.stdin.readline()
    if not raw:
        emit("error", message="No generation request received")
        return 2
    try:
        request = json.loads(raw)
        config = load_config(Path(str(request.get("config_path", "config.json"))))
        url = str(config.get("wan_gp_url", "http://127.0.0.1:7860")).rstrip("/")
        poll_interval = float(config.get("poll_interval_sec", 2.0))
        timeout = int(request.get("timeout_seconds") or config.get("generation_timeout_sec", 1800))
        client = WanGPClient(url, poll_interval)

        if request.get("operation") == "health":
            if client.health():
                emit("result", healthy=True)
                return 0
            emit("error", message="WanGP is unavailable")
            return 1

        output_path = Path(str(request["output_path"]))
        output_path.parent.mkdir(parents=True, exist_ok=True)

        def progress(message: str) -> None:
            emit("progress", status="generating", message=message)

        emit("progress", status="submitting", message="Applying YuE2 settings")
        item = client.submit_yue2(
            title=str(request.get("title", "")),
            style=str(request.get("style", "")),
            lyrics=str(request.get("lyrics", "")),
            submission_id=f"bridge_{request.get('job_id', 'unknown')}",
            duration_seconds=int(request.get("duration_seconds") or config.get("audio_duration", 320)),
            timeout=timeout,
            callback=progress,
        )
        emit("progress", status="downloading", message="Downloading provider audio")
        client.download_audio(item, output_path)
        emit(
            "result",
            external_id=str(item.get("id", "")),
            audio_path=str(output_path),
        )
        return 0
    except Exception as error:  # adapter errors are reported to the bridge
        emit("error", message=str(error))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
