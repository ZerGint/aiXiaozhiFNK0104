# FNK0104 custom firmware release 1.1.20

Built from branch `feature/fnk0104b` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware.bin` | `freenove-fnk0104s` | application image (OTA / app partition) |
| `fnk0104s-merged-binary.bin` | `freenove-fnk0104s` | full image at `0x0` |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image (OTA / app partition) |
| `fnk0104b-merged-binary.bin` | `freenove-fnk0104b` | full image at `0x0` |

Board identity differs between S and B; flash only the image built for the
connected board.

## Changes since 1.1.18 (radio, player and assistant interaction)

- Stopping media no longer flushes queued TTS or the wake popup.
- Media is paused for server notifications and server-initiated speech.
- Media starts never close a voice session started after the request; the
  automatic resume after a conversation runs in place.
- Assistant-initiated media lets the spoken acknowledgement finish; stop
  commands no longer end the conversation.
- Blocking MCP tools (Radio Browser, music bridge, Home Assistant, SD scans)
  run on a background worker instead of the main loop.
- Explicit `media.pause` / new `media.resume`; next/previous keep a radio
  station parked for the conversation.
- Radio: startup timeout, reconnect backoff with Wi-Fi wait, PSRAM buffers.
- SD player: bounded skipping of unplayable files, no lost track changes.
- Thread-safety fixes (radio selection/mirrors, audio focus), 64 KiB Radio
  Browser response cap, UI no longer scans SD on the LVGL task, wake word
  stays enabled during a music download.

Status: compiled for both boards; not yet validated on hardware.
