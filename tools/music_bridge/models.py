from __future__ import annotations

from enum import Enum
from typing import Any

from pydantic import BaseModel, Field


class JobStatus(str, Enum):
    QUEUED = "queued"
    SUBMITTING = "submitting"
    GENERATING = "generating"
    CONVERTING = "converting"
    READY = "ready"
    FAILED = "failed"


class GenerateRequest(BaseModel):
    title: str = Field(min_length=1, max_length=256)
    style: str = Field(min_length=1, max_length=4000)
    lyrics: str = Field(min_length=1, max_length=100_000)
    provider: str = Field(default="yue2", min_length=1, max_length=32)
    duration_seconds: int | None = Field(default=None, ge=1, le=600)


class JobRecord(BaseModel):
    job_id: str
    title: str
    style: str
    lyrics: str
    provider: str = "yue2"
    duration_seconds: int | None = None
    created_at: str
    updated_at: str
    status: JobStatus = JobStatus.QUEUED
    progress: str = ""
    wan_gp_gallery_id: str = ""
    wav_path: str = ""
    mp3_path: str = ""
    error: str = ""
    mp3_size: int = 0
    mp3_duration: float | None = None
    filename: str = ""
    ready_at: str = ""
    sidecar_path: str = ""

    def public(self, audio_url: str | None = None) -> dict[str, Any]:
        result: dict[str, Any] = {
            "job_id": self.job_id,
            "status": self.status.value,
            "title": self.title,
        }
        if self.progress:
            result["progress"] = self.progress
        if self.status is JobStatus.READY:
            result.update(
                {
                    "audio_url": audio_url or f"/jobs/{self.job_id}/audio",
                    "format": "mp3",
                    "size": self.mp3_size,
                }
            )
            if self.filename:
                result["filename"] = self.filename
            if self.mp3_duration is not None:
                result["duration"] = self.mp3_duration
        if self.status is JobStatus.FAILED:
            result["error"] = self.error or "generation failed"
        return result
