# FNK0104 custom firmware release 1.1.24

Built from branch `FINAL` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware-external-i2s.bin` | `freenove-fnk0104s`, external I2S amplifier (`external_amp`) | application image (OTA / app partition) |
| `fnk0104s-firmware-internal.bin` | `freenove-fnk0104s`, onboard amplifier (`internal_amp`) | application image (OTA / app partition) |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image at `0x20000` |
| `fnk0104b-bootloader.bin`, `fnk0104b-partition-table.bin`, `fnk0104b-ota-data-initial.bin`, `fnk0104b-assets.bin` | `freenove-fnk0104b` | `0x0`, `0x8000`, `0xd000`, `0x920000` |
| `fnk0104b-merged-binary.bin` (also `releases/FNK0104B_1.1.24_full_0x0.bin`) | `freenove-fnk0104b` | full image at `0x0` (also erases NVS) |

The internal-amp S image is built from the same commit with
`CONFIG_FNK_EXTERNAL_I2S_SPEAKER` disabled for the build (`dirty_worktree`).
For S, bootloader, partition table and assets are unchanged since 1.1.18, so
the application image is the complete update.

## Changes since 1.1.23

- Weather at night: the current conditions use `is_day`. A clear night shows
  "Ясно" with a crescent moon instead of "Солнечно" with a sun, and partly
  cloudy shows the crescent behind the cloud. The assistant gets the same
  wording in `condition`.
- Weather MCP: when every source fails the error is
  `weather_sources_unavailable`; the tool description names Open-Meteo and the
  MET Norway fallback and the fields a source may omit.

Status: compiled for all images. FNK0104S external-amp boots on hardware with
an equivalent pre-release build; the night icon was checked only as a geometry
render, not on the device screen. The internal-amp S image and FNK0104B have
not been validated on hardware.
