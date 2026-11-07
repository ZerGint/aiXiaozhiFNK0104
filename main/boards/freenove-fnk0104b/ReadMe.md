# Freenove FNK0104B (ESP32-S3 2.8-inch ILI9341 display)

This is a separate board identity for the FNK0104B hardware variant.  It is
kept separate from `freenove-fnk0104s` so the two boards cannot share an OTA
compatibility identity accidentally.

Current hardware configuration:

- ILI9341 SPI display, 320x240 landscape;
- FT6336 touch controller on I2C address `0x38`;
- onboard ES8311 audio path;
- 1-bit SDMMC storage;
- ESP32-S3 target.

The UI is intentionally not scaled or redesigned in this initial board
variant.  The next change will adapt the FNK0104S layout to 320x240 while
preserving the existing element proportions and relative placement.

Build with:

```powershell
python scripts/build.py freenove-fnk0104b --name freenove-fnk0104b
```
