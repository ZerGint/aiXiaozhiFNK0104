# FNK0104 custom firmware release 1.1.23

Built from branch `FINAL` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware-external-i2s.bin` | `freenove-fnk0104s`, external I2S amplifier (`external_amp`) | application image (OTA / app partition) |
| `fnk0104s-firmware-internal.bin` | `freenove-fnk0104s`, onboard amplifier (`internal_amp`) | application image (OTA / app partition) |
| `fnk0104s-internal-merged-binary.bin` (also `releases/FNK0104S_1.1.23_internal_amp_full_0x0.bin`) | `freenove-fnk0104s`, `internal_amp` | full image at `0x0` (also erases NVS) |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image at `0x20000` |
| `fnk0104b-bootloader.bin`, `fnk0104b-partition-table.bin`, `fnk0104b-ota-data-initial.bin`, `fnk0104b-assets.bin` | `freenove-fnk0104b` | `0x0`, `0x8000`, `0xd000`, `0x920000` |
| `fnk0104b-merged-binary.bin` (also `releases/FNK0104B_1.1.23_full_0x0.bin`) | `freenove-fnk0104b` | full image at `0x0` (also erases NVS) |

The internal-amp S images are built from the same commit with
`CONFIG_FNK_EXTERNAL_I2S_SPEAKER` disabled for the build (`dirty_worktree`).
For S, bootloader, partition table and assets are unchanged since 1.1.18, so
the application image is the complete update.

## Changes since 1.1.22

- Weather: MET Norway (api.met.no, no key) is a fallback when Open-Meteo is
  unreachable (seen from Russia). The working provider is stored in NVS
  (`weather/provider`) and tried first after reboot; the other one is tried
  when it fails. If both fail the widget shows "Сервер недоступен" and keeps
  cached values. The met.no response is parsed while streaming with a
  fixed-size tokenizer in PSRAM (no body buffer, no cJSON tree). met.no has no
  precipitation probability, so the forecast card shows the expected amount.
  City search still uses the Open-Meteo geocoder.

Status: compiled for all images. On FNK0104S external-amp hardware a test
build parsed a real met.no forecast (36 KB, 84 entries); the final build ran
with Open-Meteo. Switching under a real Open-Meteo block, the internal-amp S
image and FNK0104B have not been validated on hardware.
