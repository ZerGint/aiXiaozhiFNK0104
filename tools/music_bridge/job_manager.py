from __future__ import annotations

import json
import queue
import subprocess
import threading
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from mutagen.id3 import COMM, ID3, TALB, TCON, TIT2, TPE1, TXXX, USLT
from mutagen.mp3 import MP3

from .config import BridgeConfig
from .library import MusicLibrary, track_filename
from .models import JobRecord, JobStatus
from .wangp_client import WanGPClient


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


DEFAULT_ARTIST = "Kira AI"
DEFAULT_ALBUM = "AI Generated"


class JobManager:
    def __init__(self, config: BridgeConfig):
        self.config = config
        self.client = WanGPClient(config.wan_gp_url, config.poll_interval_sec)
        self.library = MusicLibrary(config.library_dir)
        self._jobs: dict[str, JobRecord] = {}
        self._lock = threading.RLock()
        self._queue: queue.Queue[str | None] = queue.Queue()
        self._stop = threading.Event()
        self._load()
        self._repair_ready_job_sizes()
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

    def create(
        self,
        title: str,
        style: str,
        lyrics: str,
        provider: str,
        duration_seconds: int | None = None,
    ) -> JobRecord:
        if provider.lower() != "yue2":
            raise ValueError("Only provider 'yue2' is supported")
        now = utc_now()
        job = JobRecord(
            job_id=uuid.uuid4().hex,
            title=title,
            style=style,
            lyrics=lyrics,
            provider="yue2",
            duration_seconds=duration_seconds,
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
            job = self._jobs.get(job_id)
            if job is not None and self._repair_job_size_locked(job):
                self._save()
            return job

    def all(self) -> list[JobRecord]:
        with self._lock:
            changed = False
            for job in self._jobs.values():
                changed = self._repair_job_size_locked(job) or changed
            if changed:
                self._save()
            return list(self._jobs.values())

    def _repair_job_size_locked(self, job: JobRecord) -> bool:
        if job.status is not JobStatus.READY:
            return False
        path = Path(job.mp3_path) if job.mp3_path else None
        if path is None or not path.is_file():
            path = self.library.audio_path(job.job_id)
        if path is None:
            return False
        try:
            actual_size = path.stat().st_size
        except OSError:
            return False
        if job.mp3_size == actual_size:
            return False
        job.mp3_size = actual_size
        job.updated_at = utc_now()
        return True

    def _repair_ready_job_sizes(self) -> None:
        with self._lock:
            changed = False
            for job in self._jobs.values():
                changed = self._repair_job_size_locked(job) or changed
            if changed:
                self._save()

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
        if job.status is JobStatus.READY and self.audio_path(job.job_id) is not None:
            return
        try:
            duration_seconds = job.duration_seconds or self.config.audio_duration
            self._update(job, status=JobStatus.SUBMITTING, progress="Applying YuE2 settings")

            def progress(text: str) -> None:
                self._update(job, status=JobStatus.GENERATING, progress=text)

            item = self.client.submit_yue2(
                title=job.title,
                style=job.style,
                lyrics=job.lyrics,
                submission_id=f"bridge_{job.job_id}",
                duration_seconds=duration_seconds,
                timeout=self.config.generation_timeout_sec,
                callback=progress,
            )
            wav_path = self.config.generated_dir / f"{job.job_id}.wav"
            self.config.generated_dir.mkdir(parents=True, exist_ok=True)
            self._update(job, status=JobStatus.GENERATING, progress="Downloading WAV")
            self.client.download_audio(item, wav_path)
            self._update(job, wan_gp_gallery_id=str(item.get("id", "")), wav_path=str(wav_path))

            self._update(job, status=JobStatus.CONVERTING, progress="Converting WAV to MP3")
            # Keep the .mp3 suffix because ffmpeg infers the output muxer
            # from the final extension.  A name ending in .mp3.tmp is
            # treated as an unknown output format on Windows.
            temporary_mp3 = self.config.generated_dir / f".{job.job_id}.tmp.mp3"
            self._convert(wav_path, temporary_mp3)
            pre_tag_size = temporary_mp3.stat().st_size
            if pre_tag_size <= 0:
                raise RuntimeError("ffmpeg produced an empty MP3")
            technical = self._tag_and_inspect(job, temporary_mp3)
            filename = track_filename(job.title, job.job_id)
            final_path = self.config.library_dir / filename
            temporary_mp3.replace(final_path)
            final_size = final_path.stat().st_size
            if final_size <= 0:
                raise RuntimeError("final MP3 is empty")
            ready_at = utc_now()
            track = {
                "id": job.job_id,
                "title": job.title,
                "style": job.style,
                "lyrics": job.lyrics,
                "provider": job.provider,
                "duration_seconds": duration_seconds,
                "filename": filename,
                "format": "mp3",
                "size": final_size,
                "duration": technical["duration"],
                "sample_rate": technical["sample_rate"],
                "channels": technical["channels"],
                "bitrate": technical["bitrate"],
                "created_at": job.created_at,
                "ready_at": ready_at,
            }
            if self.config.keep_wav:
                track["source_wav"] = str(wav_path)
            self.library.upsert(track)
            if not self.config.keep_wav:
                wav_path.unlink(missing_ok=True)
            self._update(
                job,
                status=JobStatus.READY,
                progress="Ready",
                mp3_path=str(final_path),
                mp3_size=final_size,
                mp3_duration=technical["duration"],
                filename=filename,
                ready_at=ready_at,
                sidecar_path=str(self.config.library_dir / f"{Path(filename).stem}.json"),
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

    def _tag_and_inspect(self, job: JobRecord, mp3_path: Path) -> dict[str, float | int | None]:
        tags = ID3()
        tags.add(TIT2(encoding=3, text=job.title))
        tags.add(TPE1(encoding=3, text=DEFAULT_ARTIST))
        tags.add(TALB(encoding=3, text=DEFAULT_ALBUM))
        tags.add(TCON(encoding=3, text=DEFAULT_ALBUM))
        tags.add(COMM(encoding=3, lang="und", desc="", text="Generated with YuE2"))
        tags.add(TXXX(encoding=3, desc="JOB_ID", text=job.job_id))
        tags.add(TXXX(encoding=3, desc="PROVIDER", text=job.provider))
        tags.add(TXXX(encoding=3, desc="STYLE", text=job.style))
        if job.lyrics.strip():
            tags.add(USLT(encoding=3, lang="und", desc="", text=job.lyrics))
        tags.save(mp3_path)

        audio = MP3(mp3_path)
        info = audio.info
        return {
            "duration": float(info.length) if info.length is not None else None,
            "sample_rate": int(info.sample_rate) if info.sample_rate else 0,
            "channels": int(info.channels) if info.channels else 0,
            "bitrate": int(info.bitrate) if info.bitrate else 0,
        }

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
