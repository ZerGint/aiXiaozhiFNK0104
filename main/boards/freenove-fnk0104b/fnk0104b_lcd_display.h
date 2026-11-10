#pragma once

#include "display/lcd_display.h"

// FNK0104B shows the FNK0104S interface on a 320x240 panel. The shared layout
// code works on the S 480x320 canvas and LcdDisplay converts each coordinate
// to the panel when the object is configured (see lcd_display.cc), so this
// class only reports that logical canvas.
class Fnk0104bLcdDisplay final : public SpiLcdDisplay {
public:
    Fnk0104bLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                       bool swap_xy);

protected:
    lv_coord_t GetLayoutWidth() const override { return 480; }
    lv_coord_t GetLayoutHeight() const override { return 320; }
};
