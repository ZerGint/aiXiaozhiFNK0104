from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

from .job_manager import JobManager, utc_now
from .library import MusicLibrary, _atomic_json
from .models import JobRecord, JobStatus


class TestConfig:
    def __init__(self, root: Path):
        self.root = root
        self.wan_gp_url = "http://127.0.0.1:9"
        self.poll_interval_sec = 0.01
        self.generation_timeout_sec = 10
        self.audio_duration = 320
        self.mp3_bitrate = "160k"
        self.keep_wav = True
        self.generated_dir = root / "generated"
        self.library_dir = root / "library"
        self.jobs_path = root / "jobs.json"

    def find_ffmpeg(self) -> str | None:
        return "test-ffmpeg"


class SizeMetadataTests(unittest.TestCase):
    def test_library_repairs_index_and_sidecar_from_audio(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            filename = "track.mp3"
            audio_size = 1234
            (root / filename).write_bytes(b"x" * audio_size)
            stale = {"id": "job-1", "filename": filename, "title": "Track", "size": 2}
            _atomic_json(root / "track.json", stale)
            _atomic_json(root / "library.json", {"version": 1, "tracks": [stale]})

            library = MusicLibrary(root)

            self.assertEqual(library.get("job-1")["size"], audio_size)
            self.assertEqual(json.loads((root / "library.json").read_text())["tracks"][0]["size"], audio_size)
            self.assertEqual(json.loads((root / "track.json").read_text())["size"], audio_size)

    def test_job_restart_repairs_jobs_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = TestConfig(root)
            config.library_dir.mkdir()
            filename = "track.mp3"
            audio_path = config.library_dir / filename
            audio_path.write_bytes(b"x" * 2345)
            track = {"id": "job-2", "filename": filename, "title": "Track", "size": 3}
            _atomic_json(config.library_dir / "track.json", track)
            _atomic_json(config.library_dir / "library.json", {"version": 1, "tracks": [track]})
            job = JobRecord(
                job_id="job-2", title="Track", style="test", lyrics="lyrics",
                created_at=utc_now(), updated_at=utc_now(), status=JobStatus.READY,
                mp3_path=str(audio_path), mp3_size=3, filename=filename,
            )
            config.jobs_path.write_text(json.dumps({job.job_id: job.model_dump(mode="json")}))

            manager = JobManager(config)
            try:
                repaired = manager.get(job.job_id)
                self.assertIsNotNone(repaired)
                self.assertEqual(repaired.mp3_size, 2345)
                saved = json.loads(config.jobs_path.read_text())[job.job_id]
                self.assertEqual(saved["mp3_size"], 2345)
            finally:
                manager.shutdown()

    def test_final_size_is_taken_after_tagging(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = TestConfig(root)
            manager = JobManager(config)
            try:
                job = JobRecord(
                    job_id="job-3", title="Tagged", style="test", lyrics="lyrics",
                    created_at=utc_now(), updated_at=utc_now(), status=JobStatus.QUEUED,
                )
                manager._jobs[job.job_id] = job
                manager.client.submit_yue2 = Mock(return_value={"id": "gallery-3"})
                manager.client.download_audio = Mock(side_effect=lambda item, path: path.write_bytes(b"wav"))
                manager._convert = Mock(side_effect=lambda wav, mp3: mp3.write_bytes(b"mp3-before-tags"))

                def tag_and_inspect(_: JobRecord, path: Path) -> dict[str, float | int | None]:
                    with path.open("ab") as handle:
                        handle.write(b"id3-after-conversion")
                    return {"duration": 1.0, "sample_rate": 48000, "channels": 2, "bitrate": 160000}

                manager._tag_and_inspect = Mock(side_effect=tag_and_inspect)
                manager._process(job)

                expected = len(b"mp3-before-tags") + len(b"id3-after-conversion")
                self.assertEqual(job.status, JobStatus.READY)
                self.assertEqual(job.mp3_size, expected)
                self.assertEqual(manager.library.get(job.job_id)["size"], expected)
            finally:
                manager.shutdown()


if __name__ == "__main__":
    unittest.main()
