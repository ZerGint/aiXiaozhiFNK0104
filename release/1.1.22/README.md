# FNK0104 custom firmware release 1.1.22

Built from branch `FINAL` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware-external-i2s.bin` | `freenove-fnk0104s`, external I2S amplifier (`external_amp`) | application image (OTA / app partition) |
| `fnk0104s-firmware-internal.bin` | `freenove-fnk0104s`, onboard amplifier (`internal_amp`) | application image (OTA / app partition) |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image at `0x20000` |
| `fnk0104b-bootloader.bin`, `fnk0104b-partition-table.bin`, `fnk0104b-ota-data-initial.bin`, `fnk0104b-assets.bin` | `freenove-fnk0104b` | `0x0`, `0x8000`, `0xd000`, `0x920000` |
| `fnk0104b-merged-binary.bin` | `freenove-fnk0104b` | full image at `0x0` (also erases NVS) |

The internal-amp S image is built from the same commit with
`CONFIG_FNK_EXTERNAL_I2S_SPEAKER` disabled for the build (`dirty_worktree`).
For S, bootloader, partition table and assets are unchanged since 1.1.18, so
the application image is the complete update.

## Changes since 1.1.21

- Quick Settings header: clock correction `[-] Время N ч [+]` from -2 to +2
  hours, stored in NVS (`display/time_offset_h`). It shifts the shown clock,
  the weather date and the weather update time; system time is unchanged.
- The top-bar clock is redrawn only when the minute changes (was 4 times a
  second), and the weather date follows midnight immediately.

Status: compiled for all three images. FNK0104S external-amp: an equivalent
pre-release build (same code, version 1.1.21) boots on hardware; the time
correction UI has not been checked on screen. FNK0104B not validated on
hardware.
