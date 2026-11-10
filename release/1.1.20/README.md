# FNK0104 custom firmware release 1.1.20

Built from branch `FINAL` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware-external-i2s.bin` | `freenove-fnk0104s`, external I2S amplifier (`external_amp`) | application image (OTA / app partition) |
| `fnk0104s-firmware-internal.bin` | `freenove-fnk0104s`, onboard amplifier (`internal_amp`) | application image (OTA / app partition) |
| `fnk0104s-merged-binary.bin` | `freenove-fnk0104s`, `external_amp` | full image at `0x0` |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image at `0x20000` |
| `fnk0104b-bootloader.bin`, `fnk0104b-partition-table.bin`, `fnk0104b-ota-data-initial.bin`, `fnk0104b-assets.bin` | `freenove-fnk0104b` | `0x0`, `0x8000`, `0xd000`, `0x920000` |
| `fnk0104b-merged-binary.bin` | `freenove-fnk0104b` | full image at `0x0` (also erases NVS) |

Board identity differs between S and B; flash only the image built for the
connected board. The S OTA updater also checks the amplifier variant, so an
S device only accepts the image of its own variant.

Bootloader, partition table and the `assets` partition are unchanged since
1.1.18 (the bootloader differs only in its embedded build date), so an
application-image update is sufficient for S devices already on 1.1.18.

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

## FNK0104B rebuild

The B image was rebuilt from `FINAL` with the full FNK0104S interface adapted
to the 320x240 panel (anime face, weather, Home Assistant portal, quick
settings, smaller fonts). B now uses the S partition table
`partitions/v2/16m_fnk0104s.csv` (two 4.5 MiB app slots, assets at
`0x920000`), so a B board flashed with an earlier layout needs the bootloader,
partition table, otadata, app and assets parts written once. Writing these
parts separately keeps NVS (Wi-Fi and settings) at `0x9000`; the merged image
fills the NVS range with `0xFF`. The custom update channel is disabled for B.

Status: compiled for both boards; FNK0104S external-amp boot verified on
hardware; FNK0104B not yet validated on hardware.
