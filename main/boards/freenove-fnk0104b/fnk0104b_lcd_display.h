#pragma once

#include "display/lcd_display.h"

#include <utility>

class Fnk0104bLcdDisplay final : public SpiLcdDisplay {
public:
    Fnk0104bLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                       bool swap_xy);

    void SetupUI() override;
    void ShowGeneratedDownloadProgress(const char* title, int percent) override;
    void ShowGeneratedReadyPrompt(const char* title, std::function<void(bool)> callback) override;

protected:
    lv_coord_t GetLayoutWidth() const override { return 480; }
    lv_coord_t GetLayoutHeight() const override { return 320; }

private:
    lv_obj_t* logical_canvas_ = nullptr;
};
