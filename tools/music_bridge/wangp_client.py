from __future__ import annotations

import time
from collections.abc import Callable
from pathlib import Path
from typing import Any
from urllib.parse import urljoin

import requests
from gradio_client import Client


ProgressCallback = Callable[[str], None]


class WanGPError(RuntimeError):
    pass


class WanGPClient:
    """Small adapter around the verified local WanGP/Deepy API."""

    def __init__(self, base_url: str, poll_interval: float = 2.0):
        self.base_url = base_url.rstrip("/")
        self.poll_interval = poll_interval
        self.http = requests.Session()

    def health(self) -> bool:
        try:
            response = self.http.get(self.base_url + "/config", timeout=5)
            return response.ok
        except requests.RequestException:
            return False

    def state(self) -> dict[str, Any]:
        response = self.http.get(self.base_url + "/deepy/deepy_api/state", timeout=30)
        response.raise_for_status()
        return response.json()

    def set_yue2(self, duration_seconds: int) -> None:
        response = self.http.post(
            self.base_url + "/deepy/deepy_api/settings",
            json={
                "song_variant": "YuE2",
                "audio_duration": duration_seconds,
                "use_template_properties": True,
                "seed": -1,
            },
            timeout=30,
        )
        response.raise_for_status()
        values = response.json().get("values", {})
        if values.get("song_variant") != "YuE2":
            raise WanGPError(f"WanGP did not select YuE2: {values!r}")

    def reset_deepy_session(self) -> None:
        """Start the next bridge request with an empty Deepy conversation.

        HybridService keeps the assistant transcript globally.  Without an
        explicit reset, every bridge request adds the previous lyrics and
        tool traces to the next model context until Deepy rejects the turn
        because the context window is full.  The reset affects chat/session
        state only; the global media gallery remains available for result
        attribution.
        """
        response = self.http.post(
            self.base_url + "/deepy/deepy_api/control",
            json={"action": "reset", "payload": {}},
            timeout=120,
        )
        response.raise_for_status()

    @staticmethod
    def _args(request_text: str, submission_id: str, duration_seconds: int) -> list[Any]:
        # This is the one place where the Gradio argument ordering is kept.
        return [
            [], -1, "[]", -1, request_text, submission_id,
            True, False, True, "fast", "smaller",
            720, 1280, 81, duration_seconds, -1,
            "LTX-2 2.5 Distilled",
            "LTX-2.5 Distilled With Sound",
            "Krea 2 Turbo (8 Steps)",
            "Flux Klein 9B",
            "YuE2",
            "MiniMax H3 Ref2VA Pruned Turbo Lightx2v 8 Steps",
            "Qwen3 1.7B",
            "Index TTS 2",
        ]

    def _poll_events(self, cursor: int, callback: ProgressCallback | None) -> int:
        try:
            response = self.http.get(
                self.base_url + f"/deepy/deepy_api/events/poll?after={cursor}",
                timeout=10,
            )
            response.raise_for_status()
            events = response.json()
        except requests.RequestException:
            return cursor
        for event in events:
            cursor = max(cursor, int(event.get("id", cursor)))
            if event.get("type") != "progress":
                continue
            data = event.get("data") or {}
            description = data.get("description") if isinstance(data, dict) else ""
            if description and callback:
                callback(str(description))
        return cursor

    def submit_yue2(
        self,
        *,
        title: str,
        style: str,
        lyrics: str,
        submission_id: str,
        duration_seconds: int,
        timeout: int,
        callback: ProgressCallback | None = None,
    ) -> dict[str, Any]:
        # Keep each device request independent.  The bridge and the browser
        # share HybridService's Deepy transcript, so retaining old turns can
        # exhaust the assistant context window before YuE2 is called.
        self.reset_deepy_session()
        before = self.state()
        previous_ids = {
            str(item.get("id"))
            for item in before.get("gallery", [])
            if item.get("kind") == "audio"
            and item.get("summary", {}).get("model_type") == "yue2"
        }
        cursor = int(before.get("cursor", 0))

        self.set_yue2(duration_seconds)
        # HybridService accepts the Gradio request but does not apply the
        # per-call separate_requests_with_empty_line flag. Its persisted
        # default is true, so blank lines would turn one bridge request into
        # multiple queued assistant turns and multiple songs. Keep the
        # complete lyrics in one non-empty-line request instead.
        single_request_lyrics = "\n".join(
            line.strip() for line in str(lyrics or "").splitlines() if line.strip()
        )
        text = (
            f"Generate one vocal song with the YuE2 backend. Use YuE2 "
            f"melody-and-chords vocal composition (model_mode=0), not YuE2 "
            f"Instrumental. Sing the supplied lyrics exactly in their original "
            f"language; do not return an instrumental-only track. Title: {title}. "
            f"Lyrics: {single_request_lyrics}. Music style: {style}. "
            f"Use a maximum duration of {duration_seconds} seconds and return the generated audio."
        )
        gradio = Client(self.base_url + "/")
        job = gradio.submit(
            *self._args(text, submission_id, duration_seconds),
            api_name="/ask_ai_with_ui_settings",
        )
        # This only waits for request acceptance. Final completion is below.
        job.result()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            cursor = self._poll_events(cursor, callback)
            state = self.state()
            for item in state.get("gallery", []):
                summary = item.get("summary") or {}
                if (
                    item.get("kind") == "audio"
                    and summary.get("model_type") == "yue2"
                    and str(item.get("id")) not in previous_ids
                ):
                    if callback:
                        callback("YuE2 audio ready")
                    return item
            if not state.get("busy"):
                raise WanGPError("WanGP finished without a new YuE2 audio gallery item")
            time.sleep(self.poll_interval)
        raise TimeoutError(f"YuE2 generation exceeded {timeout} seconds")

    def download_audio(self, item: dict[str, Any], destination: Path) -> None:
        relative = str(item.get("url", "")).lstrip("/")
        if not relative:
            raise WanGPError("Gallery item has no media URL")
        url = urljoin(self.base_url + "/", "deepy/" + relative)
        partial = destination.with_suffix(destination.suffix + ".part")
        partial.unlink(missing_ok=True)
        try:
            with self.http.get(url, stream=True, timeout=60) as response:
                response.raise_for_status()
                with partial.open("wb") as handle:
                    for chunk in response.iter_content(chunk_size=1024 * 1024):
                        if chunk:
                            handle.write(chunk)
            if partial.stat().st_size <= 0:
                raise WanGPError("Downloaded audio file is empty")
            with partial.open("rb") as handle:
                if handle.read(12)[:4] != b"RIFF":
                    raise WanGPError("Downloaded result is not a WAV/RIFF file")
            partial.replace(destination)
        finally:
            partial.unlink(missing_ok=True)
