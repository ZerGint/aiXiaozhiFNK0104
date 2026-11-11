# FNK0104 custom firmware release 1.1.27

Built from branch `FINAL` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware-external-i2s.bin` | `freenove-fnk0104s`, `external_amp` | application image (OTA) |
| `fnk0104s-firmware-internal.bin` | `freenove-fnk0104s`, `internal_amp` | application image (OTA) |
| `fnk0104s-external-merged-binary.bin`, `fnk0104s-internal-merged-binary.bin` | S | full image at `0x0` (erases NVS) |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image (OTA / `0x20000`) |
| `fnk0104b-merged-binary.bin` and parts | `freenove-fnk0104b` | full image at `0x0` (erases NVS) |

## Changes since 1.1.26

- Assistant-started media (radio, SD) closes the conversation first (abort of
  the current speech, goodbye to the server), then starts the source, as
  before 1.1.20. 1.1.20-1.1.26 kept the conversation open until a final TTS
  stop the server did not always send: the device stayed in Speaking with the
  radio muted, and the station never reached the local catalog.
- One network user at a time: the AI channel opens only after the radio
  stream has stopped and a weather refresh has finished (bounded waits); the
  voice pause disconnects the radio in any active state; the radio waits for
  a running weather refresh; weather still refreshes only when idle.
- MQTT: if the server does not answer hello, reconnect once and retry.
- Safety timeout: an assistant media handoff left in Speaking without a TTS
  stop closes after 3 s of silence. MCP calls and replies are logged.

Status: FNK0104S external-amp: the handoff timeout and MQTT logging were
observed on hardware; the final 1.1.27 images have not been run. The
internal-amp S image and FNK0104B have not been validated on hardware.
