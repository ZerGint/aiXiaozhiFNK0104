from __future__ import annotations

import json
import os
import queue
import re
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .config import BridgeConfig


_PROVIDER_ID = re.compile(r"^[a-z0-9][a-z0-9_-]{0,31}$")
ProgressCallback = Callable[[str], None]


@dataclass(frozen=True)
class ProviderSpec:
    provider_id: str
    name: str
    version: str
    directory: Path
    adapter: Path
    config_path: Path
    capabilities: dict[str, Any]

    def public(self, active: bool = False) -> dict[str, Any]:
        return {
            "id": self.provider_id,
            "name": self.name,
            "version": self.version,
            "active": active,
            "capabilities": self.capabilities,
        }


class ProviderRegistry:
    """Discover provider adapters kept beside the bridge executable."""

    def __init__(self, config: BridgeConfig):
        self.config = config
        self._lock = threading.RLock()
        self._providers: dict[str, ProviderSpec] = {}
        self._active_provider = getattr(config, "active_provider", "yue2")
        self.refresh()

    def refresh(self) -> list[ProviderSpec]:
        discovered: dict[str, ProviderSpec] = {}
        root = getattr(self.config, "providers_dir", None)
        if root is None:
            return []
        if root.is_dir():
            for directory in sorted(root.iterdir(), key=lambda item: item.name.lower()):
                if not directory.is_dir() or directory.name.startswith("_"):
                    continue
                manifest_path = directory / "provider.json"
                try:
                    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
                    provider_id = str(manifest.get("id", directory.name)).strip().lower()
                    if not _PROVIDER_ID.fullmatch(provider_id):
                        continue
                    adapter = directory / str(manifest.get("adapter", "adapter.exe"))
                    if not adapter.is_file():
                        continue
                    config_path = directory / str(manifest.get("config", "config.json"))
                    discovered[provider_id] = ProviderSpec(
                        provider_id=provider_id,
                        name=str(manifest.get("name", provider_id)),
                        version=str(manifest.get("version", "1")),
                        directory=directory,
                        adapter=adapter,
                        config_path=config_path,
                        capabilities=dict(manifest.get("capabilities") or {}),
                    )
                except (OSError, TypeError, ValueError, json.JSONDecodeError):
                    continue

        with self._lock:
            self._providers = discovered
            if self._active_provider not in discovered:
                configured = getattr(self.config, "active_provider", "yue2")
                self._active_provider = configured if configured in discovered else next(iter(discovered), "")
            return list(discovered.values())

    def all(self) -> list[ProviderSpec]:
        with self._lock:
            return list(self._providers.values())

    def get(self, provider_id: str) -> ProviderSpec | None:
        with self._lock:
            return self._providers.get(provider_id.strip().lower())

    @property
    def active_id(self) -> str:
        with self._lock:
            return self._active_provider

    def active(self) -> ProviderSpec | None:
        return self.get(self.active_id)

    def select(self, provider_id: str) -> ProviderSpec:
        normalized = provider_id.strip().lower()
        with self._lock:
            provider = self._providers.get(normalized)
            if provider is None:
                raise ValueError(f"Unknown provider: {provider_id}")
            self._active_provider = normalized
        self.config.save_active_provider(normalized)
        return provider

    def public(self) -> list[dict[str, Any]]:
        with self._lock:
            return [item.public(item.provider_id == self._active_provider) for item in self._providers.values()]

    @staticmethod
    def _settings(provider: ProviderSpec) -> dict[str, Any]:
        if not provider.config_path.is_file():
            return {}
        try:
            values = json.loads(provider.config_path.read_text(encoding="utf-8-sig"))
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            return {}
        return values if isinstance(values, dict) else {}

    def setting(self, provider: ProviderSpec, key: str, default: Any = None) -> Any:
        return self._settings(provider).get(key, default)

    def timeout_seconds(self, provider: ProviderSpec) -> int:
        try:
            return max(60, int(self.setting(provider, "generation_timeout_sec", 1800)))
        except (TypeError, ValueError):
            return 1800

    def default_duration(self, provider: ProviderSpec) -> int:
        try:
            return min(600, max(1, int(self.setting(provider, "audio_duration", 320))))
        except (TypeError, ValueError):
            return 320

    def resolve(self, requested: str | None) -> ProviderSpec:
        normalized = (requested or "default").strip().lower()
        if normalized in {"", "default", "auto"}:
            provider = self.active()
        else:
            provider = self.get(normalized)
        if provider is None:
            raise ValueError("No usable music provider is installed")
        return provider


class ProviderProcessError(RuntimeError):
    pass


class ProviderProcessClient:
    """Run one provider adapter using a line-delimited JSON protocol."""

    def __init__(self, registry: ProviderRegistry, config: BridgeConfig):
        self.registry = registry
        self.config = config

    def health(self, provider: ProviderSpec, timeout: int = 10) -> bool:
        request = {
            "protocol": 1,
            "operation": "health",
            "config_path": str(provider.config_path),
        }
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            process = subprocess.run(
                [str(provider.adapter)],
                cwd=str(provider.directory),
                input=json.dumps(request, ensure_ascii=False) + "\n",
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout,
                creationflags=creationflags,
            )
        except (OSError, subprocess.SubprocessError):
            return False
        if process.returncode != 0:
            return False
        for line in process.stdout.splitlines():
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get("event") == "result":
                return bool(event.get("healthy"))
        return False

    def generate(
        self,
        provider: ProviderSpec,
        *,
        title: str,
        style: str,
        lyrics: str,
        job_id: str,
        duration_seconds: int,
        timeout: int,
        output_path: Path,
        callback: ProgressCallback | None = None,
    ) -> dict[str, Any]:
        request = {
            "protocol": 1,
            "job_id": job_id,
            "title": title,
            "style": style,
            "lyrics": lyrics,
            "duration_seconds": duration_seconds,
            "timeout_seconds": timeout,
            "output_path": str(output_path),
            "config_path": str(provider.config_path),
        }
        self.config.logs_dir.mkdir(parents=True, exist_ok=True)
        log_path = self.config.logs_dir / f"provider_{job_id}.log"
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        with log_path.open("a", encoding="utf-8") as log_handle:
            process = subprocess.Popen(
                [str(provider.adapter)],
                cwd=str(provider.directory),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=log_handle,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=creationflags,
            )
            assert process.stdin is not None
            assert process.stdout is not None
            process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
            process.stdin.close()

            lines: queue.Queue[str | None] = queue.Queue()

            def read_output() -> None:
                try:
                    for line in process.stdout:
                        lines.put(line)
                finally:
                    lines.put(None)

            threading.Thread(target=read_output, name=f"provider-{job_id}", daemon=True).start()
            deadline = time.monotonic() + timeout
            result: dict[str, Any] | None = None
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.5)
                except queue.Empty:
                    if process.poll() is not None and lines.empty():
                        break
                    continue
                if line is None:
                    break
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue
                event_type = str(event.get("event", ""))
                if event_type == "progress" and callback:
                    callback(str(event.get("message") or event.get("status") or ""))
                elif event_type == "result":
                    result = event
                elif event_type == "error":
                    raise ProviderProcessError(str(event.get("message") or "Provider failed"))

            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
            if result is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"Provider {provider.provider_id} exceeded {timeout} seconds")
                raise ProviderProcessError(
                    f"Provider {provider.provider_id} exited without a result (code={process.returncode})"
                )
            if process.returncode not in (0, None):
                raise ProviderProcessError(
                    f"Provider {provider.provider_id} exited with code {process.returncode}"
                )
            actual_path = Path(str(result.get("audio_path") or output_path))
            if actual_path != output_path and actual_path.is_file():
                output_path.write_bytes(actual_path.read_bytes())
            if not output_path.is_file() or output_path.stat().st_size <= 0:
                raise ProviderProcessError("Provider returned no audio file")
            return result
