# FNK0104 custom firmware release 1.1.25

Built from branch `FINAL` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware-external-i2s.bin` | `freenove-fnk0104s`, external I2S amplifier (`external_amp`) | application image (OTA / app partition) |
| `fnk0104s-firmware-internal.bin` | `freenove-fnk0104s`, onboard amplifier (`internal_amp`) | application image (OTA / app partition) |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image (OTA / `0x20000`) |
| `fnk0104b-bootloader.bin`, `fnk0104b-partition-table.bin`, `fnk0104b-ota-data-initial.bin`, `fnk0104b-assets.bin` | `freenove-fnk0104b` | `0x0`, `0x8000`, `0xd000`, `0x920000` |
| `fnk0104b-merged-binary.bin` (also `releases/FNK0104B_1.1.25_full_0x0.bin`) | `freenove-fnk0104b` | full image at `0x0` (also erases NVS) |

The internal-amp S image is built from the same commit with
`CONFIG_FNK_EXTERNAL_I2S_SPEAKER` disabled for the build (`dirty_worktree`).
For S, bootloader, partition table and assets are unchanged since 1.1.18, so
the application image is the complete update.

## Changes since 1.1.24

- FNK0104B gets its own stable update channel, `ota/stable-fnk0104b.json` in
  the update repository (schema 2, one `internal_amp` entry with
  `board: freenove-fnk0104b`). FNK0104S keeps `ota/stable.json`. The board
  marker is checked in the manifest, in the install request and before
  installing the staged image. Quick Settings on B regains "Проверить" /
  "Обновить" and OTA 0 / OTA 1.
- B boards on 1.1.24 or earlier cannot check for updates and need this
  release once over USB (`releases/FNK0104B_1.1.25_full_0x0.bin`, or the
  parts above to keep NVS).

Status: compiled for all images. The B update channel has not been exercised
on hardware; the first over-the-air B update can be tested once a newer B
release is published. The internal-amp S image and FNK0104B have not been
validated on hardware.
