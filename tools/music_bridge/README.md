# FNK Music Bridge

Host-side Windows bridge for generating songs through external provider
adapters. The default `yue2` provider talks to an already-running local WanGP
instance. The bridge does not expose WanGP itself outside localhost.

## Prerequisites

- WanGP running at `http://127.0.0.1:7860`.
- A Python 3.10+ environment for the bridge.
- For a source checkout, `ffmpeg` in `PATH` or WanGP's `ffmpeg_bins` directory is enough.
  The distributable `FNKMusicBridge` folder includes its own `ffmpeg/ffmpeg.exe`.

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
At startup it advertises `_fnk-music._tcp.local.` through Python `zeroconf`
with the instance `FNK Music Bridge` and small `version`, `api`, and `path`
TXT properties. The advertisement is removed on clean shutdown. FNK still
verifies `/health` before using a discovered endpoint.

## Desktop player

`run_desktop.bat` starts the same bridge API together with a small Windows
player window. The window lists ready MP3 files from `data/library/` and provides
Play, Stop, and Refresh buttons. The right panel shows the latest generation
request (title, lyrics, style, duration, status, and current progress) and
refreshes while the bridge worker runs. Minimizing the window hides it in the
notification area; the tray menu restores it, stops playback, or exits the
bridge cleanly.

Selecting a row updates the right panel with that track's metadata and
sidecar text. The player also has a seek bar and a volume slider; the volume
is saved in `config/config.json` as `volume_percent`. The desktop shell uses
the existing bridge library and API. It does not add another generation queue
or change the ESP32 protocol. Build a standalone
Windows executable from a full CPython installation with Tcl/Tk support:

```powershell
powershell -ExecutionPolicy Bypass -File tools\music_bridge\build_desktop_exe.ps1
```

The resulting `dist\FNKMusicBridge.exe` is a portable one-file executable.

## Providers

Provider adapters are external executables under `providers/<id>/`. The bridge
discovers folders containing a valid `provider.json` and an existing adapter
executable. The desktop window exposes discovered providers in a dropdown; the
selected provider is stored as `active_provider` in `config/config.json`.

The included `providers/yue2/Yue2Provider.exe` contains the WanGP YuE2
integration. Its WanGP URL and polling settings are in that provider's own
`config.json`. A new provider can be installed by copying its complete folder;
the main bridge EXE does not need to be rebuilt.

`providers/other_provider/README_RU.md` documents the adapter protocol and the
required manifest fields. An adapter receives one JSON request on stdin and
returns line-delimited JSON progress/result events on stdout. It must write a
WAV file to the requested output path.

The distributable package also contains `ffmpeg/ffmpeg.exe`. The bridge checks
this bundled copy before PATH and the legacy WanGP `ffmpeg_bins` location.

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
  # Optional override; omit to use the 320-second default.
  duration_seconds = 90
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

The bridge keeps the finished tracks in `data/library/`. The
MP3 filename is `<sanitized AI title> [<first 8 characters of job_id>].mp3`.
The title is preserved exactly in `TIT2`; the default artist is `Kira AI`, the
album and genre are `AI Generated`, and the comment is `Generated with YuE2`.
The bridge also writes `TXXX:JOB_ID`, `TXXX:PROVIDER`, `TXXX:STYLE`, and the
original lyrics in `USLT` when lyrics are present. Illegal Windows/FAT
filename characters are removed while Unicode and Cyrillic are preserved.
Identical titles remain separate because the job ID suffix is unique.

Each READY track has a matching UTF-8 JSON sidecar with full bridge metadata,
including lyrics and technical audio information. `library.json` in the same
directory is a lightweight index. Sidecar and index updates use a temporary
file followed by an atomic replace. Repeating an update for the same job ID
replaces the existing entry instead of adding a duplicate.

Library read endpoints:

```text
GET /library
GET /library/<job_id>
GET /library/<job_id>/audio
```

The existing `GET /jobs/<job_id>/audio` remains job-ID based and serves the
same final library MP3. The bridge does not automatically rename old files in
`generated/`; legacy migration can use the persisted `jobs.json` title and job
ID, then apply the same tagging and sidecar flow. Existing legacy files are
left untouched unless migrated explicitly. WAV files are deleted after a
successful MP3 conversion by default; set `keep_wav` to `true` for diagnostics.

Supported statuses are `queued`, `submitting`, `generating`, `converting`, `ready`, and `failed`.

## Architecture and safety

The bridge has one bounded worker queue and processes one YuE2 generation at a time. WanGP's Deepy settings and gallery are global, so serializing jobs prevents provider races and incorrect result attribution.

Before submitting, the bridge records existing YuE2 audio gallery IDs. It accepts a result only when a new gallery item has `kind == "audio"` and `summary.model_type == "yue2"`. A completed Gradio Job only means that Deepy accepted the request; the bridge continues polling WanGP until the new gallery item is available.

Because WanGP is running with HybridService, the bridge first calls `/deepy/deepy_api/settings` with `song_variant: YuE2`. Passing `default_song` through the Gradio endpoint alone is insufficient. The bridge also sets `separate_requests_with_empty_line` to `false`.

Before submitting a bridge request it resets the active Deepy conversation
through `/deepy/deepy_api/control`. HybridService keeps the transcript global;
without this reset, old lyrics and tool traces eventually fill the assistant
context window and the generation fails before YuE2 starts. The reset clears
chat context but leaves WanGP's media gallery intact, so the bridge can still
identify the newly generated audio item.

The WAV is streamed into `data/generated/<job_id>.wav`, checked for a RIFF header, then converted with ffmpeg to a 48 kHz stereo MP3 at 160 kbps. The default is to remove the WAV after successful conversion. Set `keep_wav` to `true` in `config/config.json` to retain it.

Runtime files are intentionally ignored: `config/config.json`, `data/jobs.json`,
`data/generated/`, `data/library/`, `data/logs/`, and a bridge-local virtual
environment. The source Python scripts remain in this directory.

## Configuration

Copy `config.example.json` to `config/config.json` only when bridge overrides are needed. Provider connection and generation defaults belong to `providers/<id>/config.json`; for Yue2 these are `wan_gp_url`, `generation_timeout_sec`, `poll_interval_sec`, and `audio_duration`. A request may override the duration with `duration_seconds` from 1 through 600. YuE2 treats this as a maximum, so a short value can end during the instrumental intro before vocals begin.

The bridge is structured for a future PyInstaller build, for example:

```powershell
pyinstaller --onedir --name FNKMusicBridge tools\music_bridge\app.py
```
