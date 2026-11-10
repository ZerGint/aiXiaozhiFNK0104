# FNK0104 custom firmware release 1.1.21

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

## Changes since 1.1.20

- Microphone sensitivity (FNK0104S and FNK0104B): ES8311 digital ADC gain
  30 -> 36 dB plus a 3 dB REG17 boost (the analog PGA was already at its 30 dB
  maximum), and WebRTC AGC on the uplink with up to 18 dB gain. Measured on
  FNK0104S, speech from 1.5 m previously reached the codec near -60 dBFS
  (noise floor -75 dBFS); with the new settings the uplink carries distant
  speech at about -29..-46 dBFS without clipping.
- FNK0104B: full FNK0104S interface on the 320x240 panel (anime face,
  weather, Home Assistant portal, quick settings, smaller fonts), S partition
  layout, custom update channel disabled.

Status: compiled for all three images. FNK0104S external-amp: the microphone
settings were tested on hardware with an equivalent pre-release build
(wake word and phrases at 0.3, 1.5 and 2-3 m); the final 1.1.21 images have not
been run on hardware. FNK0104B not yet validated on hardware.
