from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse

from .config import BridgeConfig
from .job_manager import JobManager
from .mdns_advertiser import MdnsAdvertiser
from .models import GenerateRequest, JobStatus
from .providers import ProviderRegistry
from .user_library import UserMusicLibrary


config = BridgeConfig.load()
manager: JobManager | None = None
mdns: MdnsAdvertiser | None = None
provider_registry = ProviderRegistry(config)
user_library = UserMusicLibrary(config.user_music_dir)


@asynccontextmanager
async def lifespan(_: FastAPI):
    global manager, mdns
    provider_registry.refresh()
    manager = JobManager(config, provider_registry)
    mdns = MdnsAdvertiser(config.listen_port)
    await asyncio.to_thread(mdns.start)
    yield
    if mdns is not None:
        await asyncio.to_thread(mdns.stop)
        mdns = None
    manager.shutdown()
    manager = None


app = FastAPI(title="FNK Music Bridge", version="0.1.0", lifespan=lifespan)


def get_manager() -> JobManager:
    if manager is None:
        raise HTTPException(503, "Music bridge is starting")
    return manager


@app.get("/health")
def health() -> dict:
    current = get_manager()
    active_provider = current.providers.active()
    connected = active_provider is not None and current.provider_client.health(active_provider)
    provider_url = (
        current.providers.setting(active_provider, "wan_gp_url", config.wan_gp_url)
        if active_provider is not None
        else config.wan_gp_url
    )
    return {
        "ok": connected,
        "bridge": "ready" if connected else "no_provider",
        "wan_gp": "connected" if connected else "unavailable",
        "wan_gp_url": provider_url,
        "active_provider": provider_registry.active_id,
        "active_jobs": sum(
            1 for job in current.all()
            if job.status in {JobStatus.SUBMITTING, JobStatus.GENERATING, JobStatus.CONVERTING}
        ),
    } if connected else {
        "ok": False,
        "bridge": "no_provider",
        "wan_gp": "unavailable",
        "active_provider": provider_registry.active_id,
    }


@app.get("/providers")
def providers() -> list[dict]:
    provider_registry.refresh()
    return provider_registry.public()


@app.post("/providers/{provider_id}/select")
def select_provider(provider_id: str) -> dict:
    try:
        selected = provider_registry.select(provider_id)
    except ValueError as error:
        raise HTTPException(404, str(error)) from error
    return {"selected": selected.public(active=True)}


@app.post("/generate", status_code=202)
def generate(request: GenerateRequest) -> dict:
    current = get_manager()
    try:
        job = current.create(
            request.title,
            request.style,
            request.lyrics,
            request.provider,
            request.duration_seconds,
        )
    except ValueError as error:
        raise HTTPException(422, str(error)) from error
    return {"accepted": True, "job_id": job.job_id, "status": job.status.value}


@app.get("/jobs")
def jobs() -> list[dict]:
    current = get_manager()
    return [job.public() for job in current.all()]


@app.get("/jobs/{job_id}")
def job_status(job_id: str) -> dict:
    current = get_manager()
    job = current.get(job_id)
    if job is None:
        raise HTTPException(404, "Unknown job")
    audio_url = f"/jobs/{job_id}/audio" if job.status is JobStatus.READY else None
    return job.public(audio_url)


@app.get("/jobs/{job_id}/audio")
def job_audio(job_id: str) -> FileResponse:
    current = get_manager()
    job = current.get(job_id)
    if job is None:
        raise HTTPException(404, "Unknown job")
    path = current.audio_path(job_id)
    if path is None:
        if job.status is JobStatus.FAILED:
            raise HTTPException(500, job.error or "Generation failed")
        raise HTTPException(409, "Audio is not ready")
    return FileResponse(path, media_type="audio/mpeg", filename=job.filename or path.name)


@app.get("/library")
def library() -> list[dict]:
    # Keep the collection response bounded for clients running on the device.
    return get_manager().library.list_tracks()[:200]


@app.get("/library/{track_id}")
def library_track(track_id: str) -> dict:
    track = get_manager().library.get(track_id)
    if track is None:
        raise HTTPException(404, "Unknown library track")
    return track


@app.get("/library/{track_id}/audio")
def library_audio(track_id: str) -> FileResponse:
    current = get_manager()
    track = current.library.get(track_id)
    path = current.library.audio_path(track_id)
    if track is None or path is None:
        raise HTTPException(404, "Unknown library track")
    return FileResponse(path, media_type="audio/mpeg", filename=str(track.get("filename", path.name)))


@app.get("/user-library")
def user_music_library(query: str = "", limit: int = 200) -> list[dict]:
    """List MP3 files supplied by the user in data/music_library."""
    return user_library.list_tracks(query=query, limit=limit)


@app.get("/user-library/{track_id}")
def user_music_library_track(track_id: str) -> dict:
    track = user_library.get(track_id)
    if track is None:
        raise HTTPException(404, "Unknown user library track")
    return track


@app.get("/user-library/{track_id}/audio")
def user_music_library_audio(track_id: str) -> FileResponse:
    track = user_library.get(track_id)
    path = user_library.audio_path(track_id)
    if track is None or path is None:
        raise HTTPException(404, "Unknown user library track")
    return FileResponse(path, media_type="audio/mpeg", filename=Path(str(track.get("filename", path.name))).name)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host=config.listen_host, port=config.listen_port)
