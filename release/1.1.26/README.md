# FNK0104 custom firmware release 1.1.26

Built from branch `FINAL` with `build_fnk.ps1` (PlatformIO ESP-IDF 6.1.0,
Xtensa GCC 15.2.0, target `esp32s3`). Checksums and build metadata are in
`firmware-info.json`. Not yet published to the update repository.

| File | Board | Flash |
|---|---|---|
| `fnk0104s-firmware-external-i2s.bin` | `freenove-fnk0104s`, external I2S amplifier (`external_amp`) | application image (OTA / app partition) |
| `fnk0104s-firmware-internal.bin` | `freenove-fnk0104s`, onboard amplifier (`internal_amp`) | application image (OTA / app partition) |
| `fnk0104b-firmware.bin` | `freenove-fnk0104b` | application image (OTA / `0x20000`) |
| `fnk0104b-*.bin` parts and `fnk0104b-merged-binary.bin` | `freenove-fnk0104b` | full flash, see 1.1.25 |

The internal-amp S image is built from the same commit with
`CONFIG_FNK_EXTERNAL_I2S_SPEAKER` disabled for the build (`dirty_worktree`).

## Changes since 1.1.25

- Memory: the LVGL heap is in PSRAM on FNK0104S/B (`LV_STDLIB_CUSTOM`), and
  with PSRAM the `MediaControl`, `audio_input` and `audio_output` stacks are
  in PSRAM. Measured on FNK0104S, idle after connecting: internal RAM free
  42.4 -> 119.2 KB, largest free block 17.4 -> 77.8 KB, minimum
  17.7 -> 33.5 KB. Details: `docs/MEMORY_RESEARCH.md` on branch
  `research/memory`.

Status: the same code (with a research probe) ran on FNK0104S external-amp
hardware through voice, radio, SD playback and Quick Settings use without a
reboot; the final 1.1.26 external-amp image is flashed on the test board.
The internal-amp S image and FNK0104B have not been validated on hardware.
