#include "fnk0104b_lcd_display.h"

#include <esp_log.h>

namespace {
constexpr char kTag[] = "Fnk0104bDisplay";
constexpr int kReferenceWidth = 480;
constexpr int kReferenceHeight = 320;
constexpr int kScale = 170;  // LVGL scale base is 256; 170/256 ~= 66.4%.
constexpr int kTopInset = 14;
}  // namespace

Fnk0104bLcdDisplay::Fnk0104bLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_handle_t panel, int width, int height,
                                       int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                                       bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void Fnk0104bLcdDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    if (logical_canvas_ != nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    auto* screen = lv_screen_active();
    logical_canvas_ = lv_obj_create(screen);
    lv_obj_set_size(logical_canvas_, kReferenceWidth, kReferenceHeight);
    lv_obj_set_pos(logical_canvas_, 0, kTopInset);
    lv_obj_set_style_bg_opa(logical_canvas_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(logical_canvas_, 0, 0);
    lv_obj_set_style_pad_all(logical_canvas_, 0, 0);
    lv_obj_set_style_transform_pivot_x(logical_canvas_, 0, 0);
    lv_obj_set_style_transform_pivot_y(logical_canvas_, 0, 0);
    lv_obj_set_style_transform_scale(logical_canvas_, kScale, 0);
    lv_obj_add_flag(logical_canvas_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(logical_canvas_, LV_OBJ_FLAG_SCROLLABLE);

    // Reparent the shared screen objects under one transform root. LVGL also
    // applies the inverse transform during hit testing, so touch targets stay
    // aligned with the scaled controls.
    for (int32_t i = 0; i < lv_obj_get_child_cnt(screen);) {
        auto* child = lv_obj_get_child(screen, i);
        if (child == logical_canvas_) {
            ++i;
            continue;
        }
        lv_obj_set_parent(child, logical_canvas_);
    }

    ESP_LOGI(kTag, "UI_LAYOUT reference=%dx%d panel=%dx%d scale=%d/256", kReferenceWidth,
             kReferenceHeight, width_, height_, kScale);
}

void Fnk0104bLcdDisplay::ShowGeneratedDownloadProgress(const char* title, int percent) {
    SpiLcdDisplay::ShowGeneratedDownloadProgress(title, percent);

    DisplayLockGuard lock(this);
    if (logical_canvas_ == nullptr || generated_download_overlay_ == nullptr) {
        return;
    }

    lv_obj_set_parent(generated_download_overlay_, logical_canvas_);
    lv_obj_set_size(generated_download_overlay_, kReferenceWidth * 86 / 100, 92);
    lv_obj_center(generated_download_overlay_);
    lv_obj_set_width(generated_download_title_, kReferenceWidth * 78 / 100);
    lv_obj_set_width(generated_download_bar_, kReferenceWidth * 70 / 100);
}

void Fnk0104bLcdDisplay::ShowGeneratedReadyPrompt(const char* title,
                                                  std::function<void(bool)> callback) {
    SpiLcdDisplay::ShowGeneratedReadyPrompt(title, std::move(callback));

    DisplayLockGuard lock(this);
    if (logical_canvas_ == nullptr || generated_ready_popup_ == nullptr) {
        return;
    }

    lv_obj_set_parent(generated_ready_popup_, logical_canvas_);
    lv_obj_set_size(generated_ready_popup_, kReferenceWidth * 90 / 100, 128);
    lv_obj_center(generated_ready_popup_);
    lv_obj_set_width(generated_ready_title_, kReferenceWidth * 78 / 100);
}
