# Music Bridge integration (FNK0104S stage 1)

The FNK0104S firmware talks to the local `tools/music_bridge` REST service. It
does not call WanGP directly, download audio, write `/sdcard/generated_music`,
start playback, or change the media UI.

## Configure the bridge

The bridge advertises `_fnk-music._tcp.local.` on the LAN. The firmware uses
the official ESP-IDF `espressif/mdns` component to resolve that service and
then verifies `GET /health` before submitting a job. The URL is stored
persistently in NVS under namespace `music`, key `bridge_url`; the default is
empty and means automatic discovery. A valid endpoint is cached only in RAM.

The user-only MCP tool remains available for a manual diagnostic override, for
example:

```text
music.set_bridge_url({"url":"http://192.168.1.50:8765"})
```

The URL must be an `http://` or `https://` base URL and is limited to 256
characters. Passing an empty URL clears the override and returns to automatic
discovery. The setting is not a secret and contains no audio or lyrics.

Normal `music.generate` does not ask the AI or the user for an IP address. If
the manual URL is unavailable, the firmware invalidates it for the current
request and tries mDNS. Failed discovery is rate-limited to 30 seconds; no IP
range scan is performed.

## Generate and inspect a job

`music.generate` accepts `title`, `style`, `lyrics`, an optional `provider`
(`yue2` is the default), and an optional `duration_seconds` from 1 through
600. When omitted, the bridge uses its 320-second default. It validates
bounded fields, submits one short `POST /generate` request, stores only job
metadata, and returns the bridge `job_id` immediately. The long generation
runs in the bridge.

`music.generation_status` accepts an optional `job_id`. It returns the cached
status and progress; a ready job exposes `audio_url`, `format`, `size`, and
`downloaded:false`. The firmware never downloads that URL.

The bridge sends lyrics as one non-empty-line request to WanGP. HybridService
does not apply the per-request Gradio separator flag, and its persisted default
can split blank-line-separated text into multiple assistant turns. Removing
empty separator lines keeps one `music.generate` call as one WanGP job.

Pending metadata is stored in NVS key `jobs` in the `music` namespace. At most
four jobs are retained. Only job id, title, status, progress, timestamps,
error, and ready-file metadata are persisted; lyrics and style are not.

The background task polls pending jobs every 20 seconds, only while a
non-terminal job exists. Ready and failed jobs stop being polled.
