#include "fnk0104b_lcd_display.h"

#include <esp_log.h>

namespace {
constexpr char kTag[] = "Fnk0104bDisplay";
}  // namespace

Fnk0104bLcdDisplay::Fnk0104bLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_handle_t panel, int width, int height,
                                       int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                                       bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    ESP_LOGI(kTag, "UI_LAYOUT logical=480x320 panel=%dx%d mode=native", width, height);
}
