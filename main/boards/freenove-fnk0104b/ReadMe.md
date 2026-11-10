# Freenove FNK0104B (ESP32-S3 2.8-inch ILI9341 display)

This is a separate board identity for the FNK0104B hardware variant.  It is
kept separate from `freenove-fnk0104s` so the two boards cannot share an OTA
compatibility identity accidentally.

Current hardware configuration:

- ILI9341 SPI display, 320x240 landscape;
- FT6336 touch controller on I2C address `0x38`;
- onboard ES8311 audio path (`internal_amp` only);
- 1-bit SDMMC storage;
- ESP32-S3 target.

## User interface

FNK0104B runs the same interface as FNK0104S: anime face, weather panel,
radio/SD player, quick settings, Home Assistant web portal and status, boot
animation and the minimal updater screen.

The shared layout code is written for the S 480x320 canvas. For B,
`lcd_display.cc` converts every coordinate when an object is configured
(width x2/3, height x3/4; square sizes and radii use the width factor), so LVGL
renders natively on the 320x240 panel and touch hit-testing needs no
transform. Fonts are replaced at compile time with smaller faces:

| FNK0104S | FNK0104B |
|---|---|
| `font_noto_sans_basic_20_4` (theme text) | `font_noto_sans_basic_16_4` |
| `font_material_symbols_20_4` (icons) | `font_material_symbols_16_4` |
| `font_noto_sans_radio_16_4` | `font_noto_sans_radio_12_4` |
| Montserrat 14 (`LV_FONT_DEFAULT`) | Montserrat 12 (IP address: 10) |
| `font_noto_sans_basic_30_4` | `font_noto_sans_basic_20_4` |

The 240 px anime face artwork is scaled to 160 px with one LVGL transform on
the face root only.

## Updates

The custom stable-update channel is disabled for B until an update source for
this board exists: Quick Settings has no update or OTA-slot buttons, and
`CustomOtaPolicy` neither checks nor requests an install. Flash new images over
USB.

Build with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build_fnk.ps1 -Board freenove-fnk0104b
```
