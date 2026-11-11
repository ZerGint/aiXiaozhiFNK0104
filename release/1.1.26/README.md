# FNK0104 custom firmware release 1.1.26

Built from branch `FINAL` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware-external-i2s.bin` | `freenove-fnk0104s`, external I2S amplifier (`external_amp`) | application image (OTA / app partition) |
| `fnk0104s-firmware-internal.bin` | `freenove-fnk0104s`, onboard amplifier (`internal_amp`) | application image (OTA / app partition) |
| `fnk0104s-external-merged-binary.bin` (also `releases/FNK0104S_1.1.26_external_amp_full_0x0.bin`) | S, `external_amp` | full image at `0x0` (also erases NVS) |
| `fnk0104s-internal-merged-binary.bin` (also `releases/FNK0104S_1.1.26_internal_amp_full_0x0.bin`) | S, `internal_amp` | full image at `0x0` (also erases NVS) |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image (OTA / `0x20000`) |
| `fnk0104b-merged-binary.bin` (also `releases/FNK0104B_1.1.26_full_0x0.bin`) | `freenove-fnk0104b` | full image at `0x0` (also erases NVS) |

The internal-amp S images are built from the same commit with
`CONFIG_FNK_EXTERNAL_I2S_SPEAKER` disabled for the build (`dirty_worktree`).

## Changes since 1.1.25

- Memory: the LVGL heap is in PSRAM on FNK0104S/B (`LV_STDLIB_CUSTOM`).
  Idle internal RAM after connecting on FNK0104S: about 42 KB -> 101 KB free.
  This removes the 1.1.25 reboot during HTTPS radio (internal RAM exhausted:
  `esp-aes: Failed to allocate memory`, then a heap assert on reconnect).
- Weather fix: since 1.1.23 the weather worker (PSRAM stack) read and wrote
  the preferred provider in NVS, which asserts in the flash driver. Where
  Open-Meteo is unreachable every refresh ended in a reboot before the widget
  was updated. The provider is now read and saved on the main task.
- City search falls back to OpenStreetMap Nominatim when the Open-Meteo
  geocoder is unreachable; if both fail, the assistant may supply coordinates
  through the new `manual_city` action. The assistant is told not to guess
  weather values when the service fails.

Status: FNK0104S external-amp verified on hardware with Open-Meteo forecast
and geocoder made unreachable (met.no forecast, Nominatim search, provider
persisted across reboot) and 13 minutes of HTTPS radio without errors on the
memory change. The internal-amp S image and FNK0104B have not been validated
on hardware.
