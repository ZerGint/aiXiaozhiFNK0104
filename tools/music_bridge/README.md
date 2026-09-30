# FNK Music Bridge

Host-side Windows bridge for generating YuE2 songs through an already-running local WanGP instance. It does not modify the ESP32 firmware and does not expose WanGP itself outside localhost.

## Prerequisites

- WanGP running at `http://127.0.0.1:7860`.
- A Python 3.10+ environment for the bridge.
- `ffmpeg` in `PATH`, or WanGP's `ffmpeg_bins` directory. The bridge does not download executables.

Install dependencies from the repository root:

```powershell
python -m venv tools\music_bridge\.venv
tools\music_bridge\.venv\Scripts\python.exe -m pip install -r tools\music_bridge\requirements.txt
```

Run:

```powershell
tools\music_bridge\run_bridge.bat
```

The bridge listens on `0.0.0.0:8765`. WanGP remains on `127.0.0.1:7860`.

## API

Health:

```powershell
Invoke-RestMethod http://127.0.0.1:8765/health
```

Start a job:

```powershell
$body = @{
  title = "Night City"
  style = "synthwave, female vocal, atmospheric"
  lyrics = "[Verse] ... [Chorus] ..."
  provider = "yue2"
} | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8765/generate -ContentType application/json -Body $body
```

Poll the returned job ID:

```powershell
Invoke-RestMethod http://127.0.0.1:8765/jobs/<job_id>
```

When the status is `ready`, download the MP3:

```powershell
Invoke-WebRequest http://127.0.0.1:8765/jobs/<job_id>/audio -OutFile .\song.mp3
```

Supported statuses are `queued`, `submitting`, `generating`, `converting`, `ready`, and `failed`.

## Architecture and safety

The bridge has one bounded worker queue and processes one YuE2 generation at a time. WanGP's Deepy settings and gallery are global, so serializing jobs prevents provider races and incorrect result attribution.

Before submitting, the bridge records existing YuE2 audio gallery IDs. It accepts a result only when a new gallery item has `kind == "audio"` and `summary.model_type == "yue2"`. A completed Gradio Job only means that Deepy accepted the request; the bridge continues polling WanGP until the new gallery item is available.

Because WanGP is running with HybridService, the bridge first calls `/deepy/deepy_api/settings` with `song_variant: YuE2`. Passing `default_song` through the Gradio endpoint alone is insufficient. The bridge also sets `separate_requests_with_empty_line` to `false`.

The WAV is streamed into `tools/music_bridge/generated/<job_id>.wav`, checked for a RIFF header, then converted with ffmpeg to a 48 kHz stereo MP3 at 160 kbps. The default is to remove the WAV after successful conversion. Set `keep_wav` to `true` in `config.json` to retain it.

Runtime files are intentionally ignored: `config.json`, `jobs.json`, `generated/`, `logs/`, and a bridge-local virtual environment.

## Configuration

Copy `config.example.json` to `config.json` only when overrides are needed. If it is absent, built-in defaults are used. The test configuration can set `audio_duration` to `10`; normal use defaults to 30 seconds.

The bridge is structured for a future PyInstaller build, for example:

```powershell
pyinstaller --onedir --name FNKMusicBridge tools\music_bridge\app.py
```
