from __future__ import annotations

import json
import queue
import subprocess
import threading
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from .config import BridgeConfig
from .models import JobRecord, JobStatus
from .wangp_client import WanGPClient


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


class JobManager:
    def __init__(self, config: BridgeConfig):
        self.config = config
        self.client = WanGPClient(config.wan_gp_url, config.poll_interval_sec)
        self._jobs: dict[str, JobRecord] = {}
        self._lock = threading.RLock()
        self._queue: queue.Queue[str | None] = queue.Queue()
        self._stop = threading.Event()
        self._load()
        self._worker = threading.Thread(target=self._run, name="music-bridge-worker", daemon=True)
        self._worker.start()

    def _load(self) -> None:
        if not self.config.jobs_path.is_file():
            return
        try:
            raw = json.loads(self.config.jobs_path.read_text(encoding="utf-8"))
            for job_id, data in raw.items():
                job = JobRecord.model_validate(data)
                if job.status not in {JobStatus.READY, JobStatus.FAILED}:
                    job.status = JobStatus.FAILED
                    job.error = "Bridge restarted while job was active"
                    job.updated_at = utc_now()
                self._jobs[job_id] = job
        except (OSError, ValueError):
            # A malformed persistence file must not prevent health from working.
            self._jobs = {}

    def _save(self) -> None:
        self.config.jobs_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.config.jobs_path.with_suffix(".json.tmp")
        payload = {job_id: job.model_dump(mode="json") for job_id, job in self._jobs.items()}
        temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(self.config.jobs_path)

    def create(self, title: str, style: str, lyrics: str, provider: str) -> JobRecord:
        if provider.lower() != "yue2":
            raise ValueError("Only provider 'yue2' is supported")
        now = utc_now()
        job = JobRecord(
            job_id=uuid.uuid4().hex,
            title=title,
            style=style,
            lyrics=lyrics,
            provider="yue2",
            created_at=now,
            updated_at=now,
        )
        with self._lock:
            self._jobs[job.job_id] = job
            self._save()
        self._queue.put(job.job_id)
        return job

    def get(self, job_id: str) -> JobRecord | None:
        with self._lock:
            return self._jobs.get(job_id)

    def all(self) -> list[JobRecord]:
        with self._lock:
            return list(self._jobs.values())

    def _update(self, job: JobRecord, **changes: object) -> None:
        with self._lock:
            for key, value in changes.items():
                setattr(job, key, value)
            job.updated_at = utc_now()
            self._save()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                job_id = self._queue.get(timeout=0.5)
            except queue.Empty:
                continue
            if job_id is None:
                self._queue.task_done()
                break
            job = self.get(job_id)
            if job is not None:
                self._process(job)
            self._queue.task_done()

    def _process(self, job: JobRecord) -> None:
        try:
            self._update(job, status=JobStatus.SUBMITTING, progress="Applying YuE2 settings")

            def progress(text: str) -> None:
                self._update(job, status=JobStatus.GENERATING, progress=text)

            item = self.client.submit_yue2(
                title=job.title,
                style=job.style,
                lyrics=job.lyrics,
                submission_id=f"bridge_{job.job_id}",
                duration_seconds=self.config.audio_duration,
                timeout=self.config.generation_timeout_sec,
                callback=progress,
            )
            wav_path = self.config.generated_dir / f"{job.job_id}.wav"
            self.config.generated_dir.mkdir(parents=True, exist_ok=True)
            self._update(job, status=JobStatus.GENERATING, progress="Downloading WAV")
            self.client.download_audio(item, wav_path)
            self._update(job, wan_gp_gallery_id=str(item.get("id", "")), wav_path=str(wav_path))

            self._update(job, status=JobStatus.CONVERTING, progress="Converting WAV to MP3")
            mp3_path = self.config.generated_dir / f"{job.job_id}.mp3"
            self._convert(wav_path, mp3_path)
            size = mp3_path.stat().st_size
            if size <= 0:
                raise RuntimeError("ffmpeg produced an empty MP3")
            if not self.config.keep_wav:
                wav_path.unlink(missing_ok=True)
            self._update(
                job,
                status=JobStatus.READY,
                progress="Ready",
                mp3_path=str(mp3_path),
                mp3_size=size,
            )
        except Exception as error:  # worker must keep serving the API
            self._update(job, status=JobStatus.FAILED, progress="", error=str(error))

    def _convert(self, wav_path: Path, mp3_path: Path) -> None:
        ffmpeg = self.config.find_ffmpeg()
        if not ffmpeg:
            raise RuntimeError("ffmpeg not found in PATH or WanGP ffmpeg_bins")
        command = [
            ffmpeg, "-y", "-i", str(wav_path),
            "-ac", "2", "-ar", "48000", "-b:a", self.config.mp3_bitrate,
            str(mp3_path),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        if result.returncode != 0:
            raise RuntimeError(f"ffmpeg failed: {result.stderr[-1000:]}")

    def shutdown(self) -> None:
        self._stop.set()
        self._queue.put(None)
        self._worker.join(timeout=5)

    def audio_path(self, job_id: str) -> Path | None:
        job = self.get(job_id)
        if not job or job.status is not JobStatus.READY or not job.mp3_path:
            return None
        path = Path(job.mp3_path)
        return path if path.is_file() else None
