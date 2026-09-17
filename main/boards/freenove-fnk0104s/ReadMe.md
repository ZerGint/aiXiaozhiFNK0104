# Freenove FNK0104S (ESP32-S3 4.0" IPS Display)

## Hardware Overview
- **MCU**: ESP32-S3 (8MB OPI PSRAM)
- **Display**: 4.0" IPS 320x480 (ST7796 SPI Controller)
- **Touchscreen**: FT6336 I2C Controller
- **Audio Codec**: ES8311 (I2S + I2C)
- **Storage**: MicroSD Card Slot
- **Connectivity**: Wi-Fi & Bluetooth LE

## Build Command
```powershell
python scripts/build.py freenove-fnk0104s
```

## Optional external I2S speaker

Enable `CONFIG_FNK_EXTERNAL_I2S_SPEAKER` to route speaker audio to a
MAX98357A-compatible amplifier while retaining the ES8311 microphone input.

For temporary external-output diagnosis, enable
`CONFIG_FNK_EXTERNAL_I2S_DIAGNOSTICS` to log one-second PCM level summaries.
Disable diagnostics for normal firmware.
The default is disabled, which preserves the built-in ES8311/FM8002E output.

| Amplifier signal | FNK0104S pin |
| --- | --- |
| BCLK | GPIO2 |
| WS / LRCLK | GPIO21 |
| DIN | GPIO14 |

The external interface uses I2S1 at 48 kHz, 16-bit stereo. Media PCM is mixed
down to mono before a persistent `esp_ae_rate_cvt` converter. Native 48 kHz
media bypasses conversion; 44.1, 24, and 16 kHz sources are converted to 48 kHz
without resetting fractional/filter state at decoded-block boundaries. The firmware
duplicates each mono PCM sample into the left and right slots; MCLK is unused.
