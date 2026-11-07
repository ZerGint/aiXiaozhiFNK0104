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

The B display uses a separate `Fnk0104bLcdDisplay` implementation. It lays out
the shared controls on a 480x320 logical canvas and scales the canvas uniformly
to the 320x240 panel, retaining the original positions and proportions. Touch
input uses the same transform through LVGL hit testing. Hardware rendering and
touch orientation still need confirmation on a physical FNK0104B.

Build with:

```powershell
python scripts/build.py freenove-fnk0104b --name freenove-fnk0104b
```
