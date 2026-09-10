#ifndef LV_USE_KEYBOARD
#define LV_USE_KEYBOARD 1
#endif
#include "lcd_display.h"
#include "media/media_player.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include "home_assistant.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <sdkconfig.h>

#ifndef LV_USE_KEYBOARD
#define LV_USE_KEYBOARD 1
#endif
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "board.h"
#include "application.h"
#include "audio/audio_codec.h"
#include "media/radio_browser.h"
#include "media/sd_music_player.h"

#define TAG "LcdDisplay"

namespace {

static void LogUiMemory(const char* marker) {
    ESP_LOGI(TAG, "%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u spiram_free=%u spiram_largest=%u", marker,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

const lv_color_t kBg = lv_color_hex(0x071923);
const lv_color_t kTop = lv_color_hex(0x0B2433);
const lv_color_t kNav = lv_color_hex(0x0A202D);

const lv_color_t kPanel = lv_color_hex(0x0D2734);
const lv_color_t kPanel2 = lv_color_hex(0x0C2230);

const lv_color_t kCard = lv_color_hex(0x102C39);
const lv_color_t kCardHi = lv_color_hex(0x153D4B);

const lv_color_t kAccent = lv_color_hex(0x16D99A);
const lv_color_t kBorder = lv_color_hex(0x163B4A);

const lv_color_t kText = lv_color_hex(0xF4FBFC);
const lv_color_t kMuted = lv_color_hex(0x9CB8C2);




static void DisableScroll(lv_obj_t* obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static lv_obj_t* MakePanel(lv_obj_t* parent, int w, int h)
{
    auto p = lv_obj_create(parent);

    lv_obj_set_size(p, w, h);

    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_style_bg_color(p, kPanel, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, kBorder, 0);

    DisableScroll(p);

    return p;
}

static lv_obj_t* MakeButton(lv_obj_t* parent,
                            const char* text,
                            int w,
                            int h,
                            lv_color_t bg)
{
    auto b = lv_btn_create(parent);

    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    DisableScroll(b);

    auto l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, kText, 0);
    lv_obj_center(l);

    return b;
}

}

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}



LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    settings.SetString("theme", "dark");
    std::string theme_name = "dark";
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}


SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    if (service_timer_) lv_timer_delete(service_timer_);
    if (robo_eyes_timer_) {
        lv_timer_delete(robo_eyes_timer_);
        robo_eyes_timer_ = nullptr;
    }
    if (robo_eyes_buf_) {
        heap_caps_free(robo_eyes_buf_);
        robo_eyes_buf_ = nullptr;
    }

    SetPreviewImage(nullptr);

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    LogUiMemory("UI_MEM_BEFORE_SETUP");
    LogUiMemory("UI_MEM_BEFORE_SETUP");
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LogUiMemory("UI_MEM_BEFORE_COMMON_SHELL");

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, MATERIAL_SYMBOLS_WIFI);
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(right_icons, 100);
    lv_obj_set_height(right_icons, 24);
    lv_obj_remove_flag(right_icons, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scrollbar_mode(right_icons, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    auto initial_codec = Board::GetInstance().GetAudioCodec();
    lv_label_set_text(mute_label_, MATERIAL_SYMBOLS_VOLUME_UP);
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    top_volume_value_label_ = lv_label_create(right_icons);
    lv_label_set_text_fmt(top_volume_value_label_, "%d%%", initial_codec ? initial_codec->output_volume() : 0);
    lv_obj_set_style_text_font(top_volume_value_label_, text_font, 0);
    lv_obj_set_style_text_color(top_volume_value_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(top_volume_value_label_, 3, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);
    top_battery_value_label_ = lv_label_create(right_icons);
    lv_label_set_text(top_battery_value_label_, "--%");
    lv_obj_set_style_text_font(top_battery_value_label_, text_font, 0);
    lv_obj_set_style_text_color(top_battery_value_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(top_battery_value_label_, 3, 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);

    // Make top_bar_ and status_bar_ clickable to toggle Quick Settings drop-down panel
    auto top_bar_cb = [](lv_event_t* e) {
        auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        lv_event_code_t code = lv_event_get_code(e);
        ESP_LOGI(TAG, "TOP BAR EVENT DETECTED: code=%d", (int)code);
        if (code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED) {
            if (display) display->ToggleQuickSettings();
        }
    };

    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(top_bar_, top_bar_cb, LV_EVENT_ALL, this);

    lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status_bar_, top_bar_cb, LV_EVENT_ALL, this);

    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status_label_, top_bar_cb, LV_EVENT_ALL, this);

    // Initialize tabs & quick settings overlay
    panel_roboeyes_ = content_;
    SetupQuickSettingsOverlay(screen);
    lv_obj_move_foreground(top_bar_);
    lv_obj_move_foreground(status_bar_);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    DisableScroll(emoji_box_);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* RoboEyes Canvas Initialization */
    robo_eyes_canvas_ = lv_canvas_create(emoji_box_);
    size_t robo_buf_size = 240 * 120 * 2;
    robo_eyes_buf_ = heap_caps_malloc(robo_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (robo_eyes_buf_ == nullptr) {
        robo_eyes_buf_ = heap_caps_malloc(robo_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (robo_eyes_buf_ != nullptr) {
        lv_canvas_set_buffer(robo_eyes_canvas_, robo_eyes_buf_, 240, 120, LV_COLOR_FORMAT_RGB565);
        robo_eyes_.begin(robo_eyes_canvas_, 240, 120, 30);
        robo_eyes_.adapter.setColors(lvgl_theme->background_color(), lv_color_hex(0x00F0FF));
        robo_eyes_timer_ = lv_timer_create([](lv_timer_t* t) {
            RoboEyes* re = static_cast<RoboEyes*>(lv_timer_get_user_data(t));
            if (re) re->update();
        }, 33, &robo_eyes_);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(robo_eyes_canvas_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(robo_eyes_canvas_, [](lv_event_t* e) {
            Application::GetInstance().ToggleChatState();
        }, LV_EVENT_CLICKED, nullptr);
    }


    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons & Quick Settings touch trigger */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 45);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_60, 0);  // 60% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(top_bar_);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, MATERIAL_SYMBOLS_WIFI);
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(right_icons, 100);
    lv_obj_set_height(right_icons, 24);
    lv_obj_remove_flag(right_icons, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scrollbar_mode(right_icons, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    auto initial_codec = Board::GetInstance().GetAudioCodec();
    lv_label_set_text(mute_label_, MATERIAL_SYMBOLS_VOLUME_UP);
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    LogUiMemory("UI_MEM_AFTER_COMMON_SHELL");
    LogUiMemory("UI_MEM_BEFORE_QUICK_SETTINGS");
    panel_roboeyes_ = emoji_box_;
    SetupQuickSettingsOverlay(screen);
    LogUiMemory("UI_MEM_AFTER_QUICK_SETTINGS");

    // Fixed landscape shell. View selection is independent of service state.
    DisableScroll(screen);
    DisableScroll(container_);
    lv_obj_set_style_bg_color(container_, kBg, 0);
    lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 34);
    lv_obj_set_style_bg_color(top_bar_, kTop, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    DisableScroll(top_bar_);
    if (!top_volume_value_label_) {
        auto icons = lv_obj_get_parent(battery_label_);
        top_volume_value_label_ = lv_label_create(icons);
        lv_label_set_text(top_volume_value_label_, "0%");
        lv_obj_set_style_text_font(top_volume_value_label_, LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_color(top_volume_value_label_, kText, 0);
        lv_obj_set_style_margin_left(top_volume_value_label_, 3, 0);
    }
    if (!top_battery_value_label_) {
        auto icons = lv_obj_get_parent(battery_label_);
        top_battery_value_label_ = lv_label_create(icons);
        lv_label_set_text(top_battery_value_label_, "--%");
        lv_obj_set_style_text_font(top_battery_value_label_, LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_color(top_battery_value_label_, kText, 0);
        lv_obj_set_style_margin_left(top_battery_value_label_, 3, 0);
    }
    auto label = [](lv_obj_t* parent, const char* text, int x, int y, int w,
                    lv_color_t color) {
        auto obj = lv_label_create(parent);
        lv_label_set_text(obj, text);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_width(obj, w);
        lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(obj, LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_color(obj, color, 0);
        lv_obj_set_style_text_opa(obj, LV_OPA_COVER, 0);
        return obj;
    };
    auto panel = [](lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color) {
        auto obj = MakePanel(parent, w, h);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_bg_color(obj, color, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        return obj;
    };
    auto button = [&](lv_obj_t* parent, const char* text, int x, int y, int w, int h,
                      bool primary = false) {
        auto obj = MakeButton(parent, text, w, h, primary ? kAccent : kCard);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_style_shadow_width(obj, 0, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        auto caption = lv_obj_get_child(obj, 0);
        lv_obj_set_style_text_font(caption, LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_color(caption, primary ? kBg : kText, 0);
        return obj;
    };

    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    top_time_label_ = label(top_bar_, "--:--", 200, 8, 80, kText);
    lv_obj_set_style_text_align(top_time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_layout(top_bar_, LV_LAYOUT_NONE);
    lv_obj_set_pos(network_label_, 12, 7);
    lv_obj_set_pos(top_time_label_, 200, 7);
    auto top_status_group = lv_obj_get_parent(battery_label_);
    lv_obj_set_width(top_status_group, 100);
    lv_obj_set_height(top_status_group, 24);
    lv_obj_set_pos(top_status_group, 370, 5);
    lv_obj_remove_flag(top_status_group, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scrollbar_mode(top_status_group, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_layout(top_status_group, LV_LAYOUT_NONE);
    lv_obj_set_pos(mute_label_, 0, 2);
    lv_obj_set_pos(top_volume_value_label_, 30, 2);
    lv_obj_set_pos(battery_label_, 62, 2);
    lv_obj_set_pos(top_battery_value_label_, 82, 2);
    auto nav = panel(screen, 0, 38, 64, 276, kNav);
    LogUiMemory("UI_MEM_AFTER_COMMON_SHELL");
    const char* names[] = {"AI", "", ""};
    for (int i = 0; i < 3; ++i) {
        auto obj = button(nav, names[i], 4, 4 + i * 78, 56, 72);
        nav_buttons_[i] = obj;
        // Code-native icons avoid depending on missing font glyphs.
        auto stroke = [&](int x, int y, int w, int h, int radius = 0) {
            auto part = panel(obj, x, y, w, h, kText);
            lv_obj_set_style_radius(part, radius, 0);
            lv_obj_remove_flag(part, LV_OBJ_FLAG_CLICKABLE);
        };
        if (i == 2) { // Double musical note.
            stroke(17, 10, 3, 22);
            stroke(33, 10, 3, 22);
            stroke(17, 10, 19, 4);
            stroke(10, 28, 10, 7, 4);
            stroke(26, 28, 10, 7, 4);
        } else if (i == 1) { // Radio receiver and aerial.
            auto receiver = panel(obj, 12, 17, 32, 21, kCard);
            lv_obj_set_style_border_width(receiver, 2, 0);
            lv_obj_set_style_border_color(receiver, kText, 0);
            lv_obj_set_style_radius(receiver, 4, 0);
            lv_obj_remove_flag(receiver, LV_OBJ_FLAG_CLICKABLE);
            stroke(17, 7, 3, 10);
            stroke(17, 22, 8, 10, 4);
            stroke(29, 22, 10, 2);
            stroke(29, 28, 10, 2);
        }

        lv_obj_align(lv_obj_get_child(obj, 0), LV_ALIGN_TOP_MID, 0, 8);
        nav_status_[i] = label(obj, "", 2, 44, 50, kMuted);
        lv_obj_set_style_text_align(nav_status_[i], LV_TEXT_ALIGN_CENTER, 0);
        if (i > 0) {
            nav_activity_[i] = lv_obj_create(obj);
            lv_obj_set_size(nav_activity_[i], 8, 8);
            lv_obj_set_style_radius(nav_activity_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(nav_activity_[i], kAccent, 0);
            lv_obj_set_style_bg_opa(nav_activity_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(nav_activity_[i], 0, 0);
            lv_obj_align(nav_activity_[i], LV_ALIGN_BOTTOM_MID, 0, -4);
            lv_obj_add_flag(nav_activity_[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_user_data(obj, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(obj, [](lv_event_t* e) {
            auto self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            auto target = static_cast<lv_obj_t*>(lv_event_get_target(e));
            int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
            self->SwitchTab(index == 0 ? 0 : index == 1 ? 2 : 1);
        }, LV_EVENT_CLICKED, this);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, i == 0 ? kAccent : kBorder, 0);
    }
    ai_view_ = panel(screen, 66, 38, 408, 276, kBg);
    lv_obj_set_style_border_width(ai_view_, 0, 0);
    auto ai_center = panel(ai_view_, 0, 0, 258, 276, lv_color_black());
    lv_obj_set_style_border_width(ai_center, 2, 0);
    lv_obj_set_style_border_color(ai_center, kPanel2, 0);
    lv_obj_set_parent(emoji_box_, ai_center);
    lv_obj_set_size(emoji_box_, 240, 120);
    lv_obj_center(emoji_box_);
    robo_eyes_.adapter.setColors(lv_color_black(), lv_color_hex(0x00F0FF));
    DisableScroll(emoji_box_);
    lv_obj_set_parent(bottom_bar_, ai_center);
    lv_obj_set_size(bottom_bar_, 248, 64);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_set_style_pad_all(bottom_bar_, 4, 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    DisableScroll(bottom_bar_);
    lv_obj_set_width(chat_message_label_, 238);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(chat_message_label_, kText, 0);
    auto weather = panel(ai_view_, 264, 0, 144, 276, kPanel2);
    label(weather, "Weather", 10, 12, 124, kText);
    label(weather, "--", 10, 68, 124, kText);
    label(weather, "No weather data", 10, 104, 124, kMuted);
    LogUiMemory("UI_MEM_AFTER_AI_VIEW");

    LogUiMemory("UI_MEM_BEFORE_MEDIA_PAGE");
    auto media_root = panel(screen, 66, 38, 408, 276, kBg);
    lv_obj_set_style_border_width(media_root, 0, 0);
    panel_player_ = media_root;
    radio_view_ = media_root;
    auto media_center = panel(media_root, 0, 0, 258, 276, kPanel);
    auto media_right = panel(media_root, 264, 0, 144, 276, kPanel2);
    auto media_art = panel(media_center, 12, 12, 96, 90, kCard);
    media_art_label_ = label(media_art, "MUSIC", 6, 36, 84, kAccent);
    auto media_icon = media_art_label_;
    lv_obj_set_style_text_align(media_icon, LV_TEXT_ALIGN_CENTER, 0);
    label(media_center, "Now Playing", 120, 14, 124, kMuted);
    media_title_label_ = label(media_center, "No track", 120, 42, 124, kText);
    auto media_prev = button(media_center, "<<", 30, 130, 48, 40);
    media_play_button_ = button(media_center, "Play", 101, 124, 54, 52, true);
    auto media_play = media_play_button_;
    auto media_next = button(media_center, ">>", 178, 130, 48, 40);
    lv_obj_add_event_cb(media_prev, [](lv_event_t*) { MediaPlayer::GetInstance().Prev(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(media_play, [](lv_event_t*) { MediaPlayer::GetInstance().TogglePlayPause(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(media_next, [](lv_event_t*) { MediaPlayer::GetInstance().Next(); }, LV_EVENT_CLICKED, nullptr);
    media_shuffle_button_ = button(media_center, "Shuffle", 42, 202, 76, 30);
    media_repeat_button_ = button(media_center, "Repeat", 142, 202, 76, 30);
    auto volume_label = label(media_center, MATERIAL_SYMBOLS_VOLUME_UP, 12, 242, 30, kMuted);
    lv_obj_set_style_text_font(volume_label, &BUILTIN_ICON_FONT, 0);
    auto media_volume = lv_slider_create(media_center);
    lv_obj_set_pos(media_volume, 54, 247);
    lv_obj_set_size(media_volume, 182, 6);
    lv_slider_set_value(media_volume, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(media_volume, kBorder, LV_PART_MAIN);
    lv_obj_set_style_bg_color(media_volume, kAccent, LV_PART_INDICATOR);
    DisableScroll(media_volume);
    label(media_right, "Media", 10, 12, 124, kText);
    for (int row = 0; row < 4; ++row) {
        auto card = panel(media_right, 8, 40 + row * 46, 128, 42, row == 0 ? kCardHi : kCard);
        lv_obj_set_style_border_width(card, 0, 0);
        label(card, "Track", 8, 12, 112, kText);
    }
    button(media_right, "<", 6, 238, 32, 32);
    label(media_right, "1 / 3", 48, 246, 48, kText);
    button(media_right, ">", 106, 238, 32, 32);
    lv_obj_add_flag(media_root, LV_OBJ_FLAG_HIDDEN);
    LogUiMemory("UI_MEM_AFTER_MEDIA_PAGE");
    LogUiMemory("UI_MEM_BEFORE_SERVICE_TIMER");
    service_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
        self->UpdateServiceIndicators();
    }, 250, this);
    LogUiMemory("UI_MEM_AFTER_SERVICE_TIMER");
    UpdateServiceIndicators();
    LogUiMemory("UI_MEM_AFTER_ACTIVE_PAGE");
    LogUiMemory("UI_MEM_AFTER_SETUP_COMPLETE");
    lv_obj_move_foreground(top_bar_);

}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() "
                     "was called but label not created)",
                     role, content);
        }
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, content);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    }
#endif
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
    if (robo_eyes_canvas_ != nullptr && emotion != nullptr) {
        std::string emo(emotion);
        DisplayLockGuard lock(this);
        robo_eyes_.setSweat(false);
        robo_eyes_.setVFlicker(false, 0);
        robo_eyes_.setHFlicker(false, 0);
        robo_eyes_.setCuriosity(false);

        if (emo == "neutral" || emo == "idle" || emo == "robot_2") {
            robo_eyes_.setMood(ROBOEYES_DEFAULT);
            robo_eyes_.setIdleMode(true, 2, 3);
            robo_eyes_.setPosition(ROBOEYES_CENTER);
        } else if (emo == "happy" || emo == "joyful" || emo == "laughing") {
            robo_eyes_.setMood(ROBOEYES_HAPPY);
            robo_eyes_.anim_laugh();
        } else if (emo == "angry" || emo == "annoyed") {
            robo_eyes_.setMood(ROBOEYES_ANGRY);
            robo_eyes_.setPosition(ROBOEYES_CENTER);
        } else if (emo == "sleepy" || emo == "tired") {
            robo_eyes_.setMood(ROBOEYES_TIRED);
            robo_eyes_.setPosition(ROBOEYES_S);
        } else if (emo == "thinking" || emo == "confused") {
            robo_eyes_.setMood(ROBOEYES_DEFAULT);
            robo_eyes_.anim_confused();
            robo_eyes_.setSweat(true);
        } else if (emo == "listening") {
            robo_eyes_.setMood(ROBOEYES_DEFAULT);
            robo_eyes_.setPosition(ROBOEYES_N);
            robo_eyes_.setCuriosity(true);
        } else if (emo == "speaking") {
            robo_eyes_.setMood(ROBOEYES_HAPPY);
            robo_eyes_.setVFlicker(true, 3);
        } else {
            robo_eyes_.setMood(ROBOEYES_DEFAULT);
            robo_eyes_.setIdleMode(true, 2, 3);
        }
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LcdDisplay::ToggleQuickSettings() {
    DisplayLockGuard lock(this);
    if (!quick_settings_panel_) {
        ESP_LOGE(TAG, "ToggleQuickSettings: quick_settings_panel_ is null!");
        return;
    }
    quick_settings_open_ = !quick_settings_open_;
    ESP_LOGI(TAG, "ToggleQuickSettings: panel state is now %s", quick_settings_open_ ? "OPEN" : "CLOSED");
    if (quick_settings_open_) {
        lv_obj_remove_flag(quick_settings_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(quick_settings_panel_);
    } else {
        lv_obj_add_flag(quick_settings_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::UpdateServiceIndicators() {
    // LVGL timer context: read service state without changing it.
    auto& sd = SdMusicPlayer::GetInstance();
    auto& radio = InternetRadioPlayer::GetInstance();
    const char* player_state = sd.IsPlaying() ? "Playing" : sd.IsPaused() ? "Paused" : "";
    const char* radio_state = radio.IsPlaying() ? "Playing" : radio.IsPaused() ? "Paused" : "";
    if (InternetRadioPlayer::GetInstance().IsActive()) active_media_source_ = ActiveMediaSource::Radio;
    else if (SdMusicPlayer::GetInstance().IsPlaying() || SdMusicPlayer::GetInstance().IsPaused()) active_media_source_ = ActiveMediaSource::Player;
    else active_media_source_ = ActiveMediaSource::None;
    if (media_play_button_) {
        const bool playing = (active_media_source_ == ActiveMediaSource::Radio)
                                 ? InternetRadioPlayer::GetInstance().IsPlaying()
                                 : (active_media_source_ == ActiveMediaSource::Player)
                                     ? SdMusicPlayer::GetInstance().IsPlaying() : false;
        lv_label_set_text(lv_obj_get_child(media_play_button_, 0), playing ? "Pause" : "Play");
    }
    if (media_title_label_) {
        std::string title = "Nothing playing";
        const char* art = "MEDIA";
        if (active_media_source_ == ActiveMediaSource::Radio) {
            title = InternetRadioPlayer::GetInstance().GetTitle();
            art = "RADIO";
        } else if (active_media_source_ == ActiveMediaSource::Player) {
            title = MediaPlayer::GetInstance().GetTitle();
            art = "MUSIC";
        }
        if (title.empty()) title = "Nothing playing";
        lv_label_set_text(media_title_label_, title.c_str());
        if (media_art_label_) lv_label_set_text(media_art_label_, art);
    }
    if (media_shuffle_button_ && media_repeat_button_) {
        if (active_media_source_ == ActiveMediaSource::Radio) {
            lv_obj_add_state(media_shuffle_button_, LV_STATE_DISABLED);
            lv_obj_add_state(media_repeat_button_, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(media_shuffle_button_, LV_STATE_DISABLED);
            lv_obj_clear_state(media_repeat_button_, LV_STATE_DISABLED);
        }
    }
    const auto state = Application::GetInstance().GetDeviceState();
    const char* ai_state = state == kDeviceStateListening ? "Listen" :
                           state == kDeviceStateSpeaking ? "Speak" : "Ready";
    const char* states[] = {ai_state, radio_state, player_state};
    for (int i = 0; i < 3; ++i) {
        if (!nav_status_[i]) continue;
        if (strcmp(lv_label_get_text(nav_status_[i]), states[i]) != 0) {
            lv_label_set_text(nav_status_[i], states[i]);
        }
        const bool active = i == 0 ? state == kDeviceStateListening ||
                                    state == kDeviceStateSpeaking : states[i][0] != 0;
        lv_obj_set_style_text_color(nav_status_[i], active ? kAccent : kMuted, 0);
        if (i > 0 && nav_activity_[i]) {
            if (active) lv_obj_remove_flag(nav_activity_[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(nav_activity_[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(nav_status_[i], "");
        }
    }
    if (top_volume_value_label_) {
        auto codec = Board::GetInstance().GetAudioCodec();
        lv_label_set_text_fmt(top_volume_value_label_, "%d%%", codec ? codec->output_volume() : 0);
    }
    if (top_battery_value_label_) {
        int level = 0;
        bool charging = false, discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging))
            lv_label_set_text_fmt(top_battery_value_label_, "%d%%", level);
    }
    if (top_time_label_) {
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        if (tm_now.tm_year >= 2025 - 1900) {
            char time_text[8];
            strftime(time_text, sizeof(time_text), "%H:%M", &tm_now);
            lv_label_set_text(top_time_label_, time_text);
        }
    }
}

void LcdDisplay::SwitchTab(int tab_index)
{
    DisplayLockGuard lock(this);

    current_tab_index_ = tab_index;
    media_browser_mode_ = (tab_index == 2) ? MediaBrowserMode::Radio : MediaBrowserMode::Player;
    media_radio_mode_ = (media_browser_mode_ == MediaBrowserMode::Radio);
    LogUiMemory("UI_MEM_AFTER_TAB_SWITCH");

    if (ai_view_)
        (tab_index == 0) ? lv_obj_remove_flag(ai_view_, LV_OBJ_FLAG_HIDDEN)
                         : lv_obj_add_flag(ai_view_, LV_OBJ_FLAG_HIDDEN);
    if (panel_player_)
        (tab_index == 0) ? lv_obj_add_flag(panel_player_, LV_OBJ_FLAG_HIDDEN)
                         : lv_obj_remove_flag(panel_player_, LV_OBJ_FLAG_HIDDEN);

    /* navigation highlight */

    for (int i = 0; i < 3; ++i)
    {
        if (!nav_buttons_[i])
            continue;

        bool selected =
            (tab_index == 0 && i == 0) ||
            (tab_index == 1 && i == 2) ||
            (tab_index == 2 && i == 1);

        lv_obj_set_style_bg_color(
            nav_buttons_[i],
            selected ? kCardHi : lv_color_hex(0x0F2A37),
            0);

        lv_obj_set_style_border_color(
            nav_buttons_[i],
            selected ? kAccent : lv_color_hex(0x1B5363),
            0);
    }

    if (quick_settings_open_)
        ToggleQuickSettings();

    lv_obj_move_foreground(top_bar_);
}

void LcdDisplay::SetupQuickSettingsOverlay(lv_obj_t* parent) {
    quick_settings_panel_ = lv_obj_create(parent);
    lv_obj_set_size(quick_settings_panel_, 440, 160);
    lv_obj_align(quick_settings_panel_, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(quick_settings_panel_, lv_color_hex(0x102432), 0);
    lv_obj_set_style_bg_opa(quick_settings_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(quick_settings_panel_, 16, 0);
    lv_obj_set_style_border_width(quick_settings_panel_, 2, 0);
    lv_obj_set_style_border_color(quick_settings_panel_, lv_color_hex(0x1E4A60), 0);
    lv_obj_set_style_pad_all(quick_settings_panel_, 10, 0);
    lv_obj_set_scrollbar_mode(quick_settings_panel_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(quick_settings_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(quick_settings_panel_, LV_OBJ_FLAG_HIDDEN);

    // Header Row: Title + Close Button
    lv_obj_t* header = lv_obj_create(quick_settings_panel_);
    lv_obj_set_size(header, 416, 40);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Настройки");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t* close_btn = lv_btn_create(header);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_radius(close_btn, 18, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x173B4D), 0);
    lv_obj_t* close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "X");
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(close_label);

    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        if (display) display->ToggleQuickSettings();
    }, LV_EVENT_CLICKED, this);

    // Volume Row (Anchored at X=95 to avoid label overlap)
    lv_obj_t* vol_row = lv_obj_create(quick_settings_panel_);
    lv_obj_set_size(vol_row, 416, 45);
    lv_obj_align(vol_row, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* vol_icon = lv_label_create(vol_row);
    lv_label_set_text(vol_icon, MATERIAL_SYMBOLS_VOLUME_UP);
    lv_obj_set_style_text_font(vol_icon, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(vol_icon, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(vol_icon, LV_ALIGN_LEFT_MID, 10, 0);

    volume_slider_ = lv_slider_create(vol_row);
    lv_obj_set_size(volume_slider_, 220, 16);
    lv_obj_align(volume_slider_, LV_ALIGN_LEFT_MID, 95, 0);
    lv_slider_set_range(volume_slider_, 0, 100);
    auto codec = Board::GetInstance().GetAudioCodec();
    int cur_vol = codec ? codec->output_volume() : 70;
    lv_slider_set_value(volume_slider_, cur_vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0x173B4D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0x16C1B7), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0x8AF2DD), LV_PART_KNOB);

    volume_val_label_ = lv_label_create(vol_row);
    lv_label_set_text_fmt(volume_val_label_, "%d%%", cur_vol);
    lv_obj_set_style_text_color(volume_val_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(volume_val_label_, LV_ALIGN_LEFT_MID, 330, 0);

    lv_obj_add_event_cb(volume_slider_, [](lv_event_t* e) {
        auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        auto slider = (const lv_obj_t*)lv_event_get_target(e);
        int val = lv_slider_get_value(slider);
        auto c = Board::GetInstance().GetAudioCodec();
        if (c) c->SetOutputVolume(val);
        if (display && display->volume_val_label_) {
            lv_label_set_text_fmt(display->volume_val_label_, "%d%%", val);
        }
    }, LV_EVENT_VALUE_CHANGED, this);

    // Brightness Row (Anchored at X=95 to avoid label overlap)
    lv_obj_t* bright_row = lv_obj_create(quick_settings_panel_);
    lv_obj_set_size(bright_row, 416, 45);
    lv_obj_align(bright_row, LV_ALIGN_TOP_MID, 0, 95);
    lv_obj_set_style_bg_opa(bright_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bright_row, 0, 0);
    lv_obj_set_style_pad_all(bright_row, 0, 0);
    lv_obj_remove_flag(bright_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bright_icon = lv_label_create(bright_row);
    lv_label_set_text(bright_icon, "Экран");
    lv_obj_set_width(bright_icon, 100);
    lv_obj_set_style_pad_left(bright_icon, 4, 0);
    lv_obj_set_style_text_color(bright_icon, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(bright_icon, LV_ALIGN_LEFT_MID, 0, 0);

    brightness_slider_ = lv_slider_create(bright_row);
    lv_obj_set_size(brightness_slider_, 220, 16);
    lv_obj_align(brightness_slider_, LV_ALIGN_LEFT_MID, 95, 0);
    lv_slider_set_range(brightness_slider_, 10, 100);
    auto bl = Board::GetInstance().GetBacklight();
    int cur_bright = bl ? bl->brightness() : 100;
    lv_slider_set_value(brightness_slider_, cur_bright, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0xFBBF24), LV_PART_KNOB);

    brightness_val_label_ = lv_label_create(bright_row);
    lv_label_set_text_fmt(brightness_val_label_, "%d%%", cur_bright);
    lv_obj_set_style_text_color(brightness_val_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(brightness_val_label_, LV_ALIGN_LEFT_MID, 330, 0);

    lv_obj_add_event_cb(brightness_slider_, [](lv_event_t* e) {
        auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        auto slider = (const lv_obj_t*)lv_event_get_target(e);
        int val = lv_slider_get_value(slider);
        auto b = Board::GetInstance().GetBacklight();
        if (b) b->SetBrightness(val, true);
        if (display && display->brightness_val_label_) {
            lv_label_set_text_fmt(display->brightness_val_label_, "%d%%", val);
        }
    }, LV_EVENT_VALUE_CHANGED, this);



}

void LcdDisplay::SetupMediaPlayerTab(lv_obj_t* parent) {
    panel_player_ = lv_obj_create(parent);
    lv_obj_set_size(panel_player_, LV_HOR_RES - 10, LV_VER_RES - 60);
    lv_obj_align(panel_player_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel_player_, lv_color_hex(0x121218), 0);
    lv_obj_set_style_radius(panel_player_, 12, 0);
    lv_obj_set_style_border_width(panel_player_, 1, 0);
    lv_obj_set_style_border_color(panel_player_, lv_color_hex(0x9333EA), 0);
    lv_obj_set_flex_flow(panel_player_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_player_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(panel_player_, 10, 0);
    lv_obj_set_scrollbar_mode(panel_player_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(panel_player_, LV_OBJ_FLAG_HIDDEN);

    // Initial playlist scan
    MediaPlayer::GetInstance().ScanSd();

    // Album Art / Music Icon
    lv_obj_t* art_box = lv_obj_create(panel_player_);
    lv_obj_set_size(art_box, 60, 60);
    lv_obj_set_style_radius(art_box, 30, 0);
    lv_obj_set_style_bg_color(art_box, lv_color_hex(0x9333EA), 0);
    lv_obj_set_style_border_width(art_box, 0, 0);
    lv_obj_t* art_lbl = lv_label_create(art_box);
    lv_label_set_text(art_lbl, "🎵");
    lv_obj_center(art_lbl);

    // Track Title & Subtitle
    player_track_label_ = lv_label_create(panel_player_);
    lv_label_set_text(player_track_label_, MediaPlayer::GetInstance().GetTitle().c_str());
    lv_obj_set_style_text_color(player_track_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_top(player_track_label_, 6, 0);

    player_status_label_ = lv_label_create(panel_player_);
    lv_label_set_text(player_status_label_, "⏹ Остановлен");
    lv_obj_set_style_text_color(player_status_label_, lv_color_hex(0x8888AA), 0);
    lv_obj_set_style_pad_bottom(player_status_label_, 8, 0);

    // Controls Row
    lv_obj_t* ctrl_row = lv_obj_create(panel_player_);
    lv_obj_set_size(ctrl_row, LV_PCT(95), 44);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Prev Button
    lv_obj_t* prev_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(prev_btn, 80, 36);
    lv_obj_set_style_radius(prev_btn, 8, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x374151), 0);
    lv_obj_t* prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, "⏮ Назад");
    lv_obj_center(prev_lbl);
    lv_obj_add_event_cb(prev_btn, [](lv_event_t* e) {
        auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        MediaPlayer::GetInstance().Prev();
        if (display && display->player_track_label_) {
            lv_label_set_text(display->player_track_label_, MediaPlayer::GetInstance().GetTitle().c_str());
        }
    }, LV_EVENT_CLICKED, this);

    // Play/Pause Button
    lv_obj_t* play_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(play_btn, 120, 36);
    lv_obj_set_style_radius(play_btn, 8, 0);
    lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x9333EA), 0);
    lv_obj_t* play_lbl = lv_label_create(play_btn);
    lv_label_set_text(play_lbl, "⏯ Старт/Пауза");
    lv_obj_center(play_lbl);
    lv_obj_add_event_cb(play_btn, [](lv_event_t* e) {
        auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        MediaPlayer::GetInstance().TogglePlayPause();
        if (display) {
            if (display->player_track_label_) {
                lv_label_set_text(display->player_track_label_, MediaPlayer::GetInstance().GetTitle().c_str());
            }
            if (display->player_status_label_) {
                bool playing = MediaPlayer::GetInstance().IsPlaying();
                bool paused = MediaPlayer::GetInstance().IsPaused();
                if (playing) lv_label_set_text(display->player_status_label_, "▶ Воспроизведение");
                else if (paused) lv_label_set_text(display->player_status_label_, "⏸ Пауза");
                else lv_label_set_text(display->player_status_label_, "⏹ Остановлен");
            }
        }
    }, LV_EVENT_CLICKED, this);

    // Next Button
    lv_obj_t* next_btn = lv_btn_create(ctrl_row);
    lv_obj_set_size(next_btn, 80, 36);
    lv_obj_set_style_radius(next_btn, 8, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x374151), 0);
    lv_obj_t* next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, "Вперед ⏭");
    lv_obj_center(next_lbl);
    lv_obj_add_event_cb(next_btn, [](lv_event_t* e) {
        auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        MediaPlayer::GetInstance().Next();
        if (display && display->player_track_label_) {
            lv_label_set_text(display->player_track_label_, MediaPlayer::GetInstance().GetTitle().c_str());
        }
    }, LV_EVENT_CLICKED, this);

    // Refresh / Scan SD Button
    lv_obj_t* scan_btn = lv_btn_create(panel_player_);
    lv_obj_set_size(scan_btn, 220, 32);
    lv_obj_set_style_radius(scan_btn, 8, 0);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_pad_top(scan_btn, 6, 0);
    lv_obj_t* scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, "🔄 Сканировать /sdcard/music");
    lv_obj_center(scan_lbl);
    lv_obj_add_event_cb(scan_btn, [](lv_event_t* e) {
        auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        MediaPlayer::GetInstance().ScanSd();
        if (display && display->player_track_label_) {
            lv_label_set_text(display->player_track_label_, MediaPlayer::GetInstance().GetTitle().c_str());
        }
    }, LV_EVENT_CLICKED, this);
}

void LcdDisplay::SetupSegaEmulatorTab(lv_obj_t* parent) {
    panel_sega_ = lv_obj_create(parent);
    lv_obj_set_size(panel_sega_, LV_HOR_RES - 10, LV_VER_RES - 60);
    lv_obj_align(panel_sega_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel_sega_, lv_color_hex(0x0F0F1A), 0);
    lv_obj_set_style_radius(panel_sega_, 12, 0);
    lv_obj_set_style_border_width(panel_sega_, 1, 0);
    lv_obj_set_style_border_color(panel_sega_, lv_color_hex(0xEA580C), 0);
    lv_obj_set_flex_flow(panel_sega_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_sega_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(panel_sega_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(panel_sega_, LV_OBJ_FLAG_HIDDEN);

    // Retro Header
    lv_obj_t* header = lv_label_create(panel_sega_);
    lv_label_set_text(header, "SEGA MEGA DRIVE");
    lv_obj_set_style_text_color(header, lv_color_hex(0xF97316), 0);

    // Games List
    const char* games[] = {
        "1. Sonic The Hedgehog",
        "2. Streets of Rage 2",
        "3. Mortal Kombat 3",
        "4. Earthworm Jim"
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t* btn = lv_btn_create(panel_sega_);
        lv_obj_set_size(btn, LV_PCT(90), 28);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E2E), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x444466), 0);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, games[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xE0E0E0), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
    }

    // Launch Button
    lv_obj_t* launch_btn = lv_btn_create(panel_sega_);
    lv_obj_set_size(launch_btn, LV_PCT(90), 34);
    lv_obj_set_style_radius(launch_btn, 8, 0);
    lv_obj_set_style_bg_color(launch_btn, lv_color_hex(0x16A34A), 0);
    lv_obj_t* launch_lbl = lv_label_create(launch_btn);
    lv_label_set_text(launch_lbl, "ЗАПУСТИТЬ ИГРУ");
    lv_obj_set_style_text_color(launch_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(launch_lbl);
}

#if 0 // Bluetooth implementation retained only as historical reference.
struct BleDiscoveredDev {
    std::string name;
    std::string mac;
    int rssi;
    uint8_t addr_type;
};

static std::vector<BleDiscoveredDev> s_ble_devices;

static void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void EnsureNimbleStarted() {
    // Initialized at boot in board constructor
}

static int ble_disc_event_cb(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_hs_adv_fields fields;
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 event->disc.addr.val[5], event->disc.addr.val[4], event->disc.addr.val[3],
                 event->disc.addr.val[2], event->disc.addr.val[1], event->disc.addr.val[0]);

        std::string dev_name = "";
        if (rc == 0 && fields.name && fields.name_len > 0) {
            dev_name = std::string((char*)fields.name, fields.name_len);
        } else {
            dev_name = "BLE Устройство (" + std::string(mac_str).substr(12) + ")";
        }

        bool exists = false;
        for (auto& d : s_ble_devices) {
            if (d.mac == mac_str) {
                d.rssi = event->disc.rssi;
                if (dev_name.find("BLE Устройство") == std::string::npos) d.name = dev_name;
                exists = true;
                break;
            }
        }
        if (!exists && s_ble_devices.size() < 15) {
            ESP_LOGI("LcdDisplay", "BLE DISCOVERED: %s (%s, type=%d) %d dBm", dev_name.c_str(), mac_str, event->disc.addr.type, event->disc.rssi);
            s_ble_devices.push_back({dev_name, mac_str, event->disc.rssi, event->disc.addr.type});
        }
    }
    return 0;
}

void LcdDisplay::ScanBluetoothDevices() {
    ESP_LOGI("LcdDisplay", "ScanBluetoothDevices executing REAL NimBLE hardware RF scan...");
    if (!bt_status_label_ || !bt_dev_list_container_) return;

    {
        DisplayLockGuard lock(this);
        lv_label_set_text(bt_status_label_, "⏳ Аппаратный поиск BLE 2.4 ГГц...");
        lv_obj_set_style_text_color(bt_status_label_, lv_color_hex(0xF59E0B), 0);
        lv_obj_clean(bt_dev_list_container_);
    }

    s_ble_devices.clear();

    EnsureNimbleStarted();

    int wait_cycles = 0;
    while (!ble_hs_synced() && wait_cycles < 20) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_cycles++;
    }

    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;

    struct ble_gap_disc_params disc_params = {};
    disc_params.filter_duplicates = 0;
    disc_params.passive = 0;
    disc_params.itvl = 160;   // 100 ms scan interval
    disc_params.window = 160; // 100 ms scan window (100% active receiver)
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    int rc_disc = ble_gap_disc(own_addr_type, 4000, &disc_params, ble_disc_event_cb, NULL);
    ESP_LOGI("LcdDisplay", "ble_gap_disc returned rc: %d", rc_disc);

    vTaskDelay(pdMS_TO_TICKS(4200));

    DisplayLockGuard lock(this);

    if (s_ble_devices.empty()) {
        lv_label_set_text(bt_status_label_, "🔴 Устройства не найдены. Переведите устройство в режим сопряжения.");
        lv_obj_set_style_text_color(bt_status_label_, lv_color_hex(0xEF4444), 0);
        return;
    }

    lv_label_set_text_fmt(bt_status_label_, "🟢 Найдено устройств: %d", (int)s_ble_devices.size());
    lv_obj_set_style_text_color(bt_status_label_, lv_color_hex(0x10B981), 0);

    for (const auto& dev : s_ble_devices) {
        lv_obj_t* btn = lv_btn_create(bt_dev_list_container_);
        lv_obj_set_size(btn, 320, 38);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222233), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x444466), 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        const char* icon_utf8 = MATERIAL_SYMBOLS_HEADPHONES;
        std::string lower_name = dev.name;
        for (auto& c : lower_name) c = tolower((unsigned char)c);
        if (lower_name.find("xbox") != std::string::npos || lower_name.find("pad") != std::string::npos || lower_name.find("game") != std::string::npos || lower_name.find("ctrl") != std::string::npos) {
            icon_utf8 = MATERIAL_SYMBOLS_SPORTS_ESPORTS;
        } else if (lower_name.find("band") != std::string::npos || lower_name.find("watch") != std::string::npos) {
            icon_utf8 = MATERIAL_SYMBOLS_WATCH;
        } else if (lower_name.find("phone") != std::string::npos || lower_name.find("ble") != std::string::npos) {
            icon_utf8 = MATERIAL_SYMBOLS_PHONE;
        }

        lv_obj_t* icon_lbl = lv_label_create(btn);
        lv_label_set_text(icon_lbl, icon_utf8);
        lv_obj_set_style_text_font(icon_lbl, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0x60A5FA), 0);
        lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t* text_lbl = lv_label_create(btn);
        char item_buf[96];
        snprintf(item_buf, sizeof(item_buf), "%s (%s) %d dBm", dev.name.c_str(), dev.mac.c_str(), dev.rssi);
        lv_label_set_text(text_lbl, item_buf);
        lv_obj_set_style_text_font(text_lbl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(text_lbl, lv_color_hex(0xE0E0E0), 0);
        lv_obj_align(text_lbl, LV_ALIGN_LEFT_MID, 32, 0);

        struct BleItemCtx {
            LcdDisplay* display;
            std::string name;
            std::string mac;
            uint8_t addr_type;
        };
        auto ctx = new BleItemCtx{this, dev.name, dev.mac, dev.addr_type};

        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto c = static_cast<BleItemCtx*>(lv_event_get_user_data(e));
            lv_event_code_t code = lv_event_get_code(e);
            if (code == LV_EVENT_DELETE) {
                delete c;
                return;
            }
            if (code == LV_EVENT_CLICKED && c && c->display) {
                c->display->ConnectToBluetoothDevice(c->name, c->mac, c->addr_type);
            }
        }, LV_EVENT_ALL, ctx);
    }
}

static struct {
    uint16_t notify_handles[8];
    int notify_count;
    uint16_t readable_handles[20];
    int readable_count;
    uint16_t hid_control_point_handle;
    uint16_t conn_handle;
    int phase;       // 0=reading all readable, 1=subscribing notify
    int current_idx;
} s_gatt_ctx = {};

static void ble_process_next(void);

// Phase 1 callback: after reading a readable characteristic
static int ble_read_all_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr,
                           void *arg) {
    uint16_t val_h = (uint16_t)(uintptr_t)arg;
    int len = (error->status == 0 && attr && attr->om) ? OS_MBUF_PKTLEN(attr->om) : 0;
    ESP_LOGI("LcdDisplay", "READ ALL: val_handle=%d status=%d len=%d [%d/%d]",
             val_h, error->status, len, s_gatt_ctx.current_idx + 1, s_gatt_ctx.readable_count);
    s_gatt_ctx.current_idx++;
    ble_process_next();
    return 0;
}

// Phase 2 callback: after writing CCCD
static int ble_cccd_write_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg) {
    ESP_LOGI("LcdDisplay", "CCCD WRITE RESPONSE: status=%d, handle=%d", error->status, attr ? attr->handle : 0);
    s_gatt_ctx.current_idx++;
    ble_process_next();
    return 0;
}

static void ble_process_next(void) {
    if (s_gatt_ctx.phase == 0) {
        // Phase 0: read all readable characteristics
        if (s_gatt_ctx.current_idx < s_gatt_ctx.readable_count) {
            uint16_t val_h = s_gatt_ctx.readable_handles[s_gatt_ctx.current_idx];
            int rc = ble_gattc_read(s_gatt_ctx.conn_handle, val_h, ble_read_all_cb, (void*)(uintptr_t)val_h);
            if (rc != 0) {
                ESP_LOGW("LcdDisplay", "ble_gattc_read val_handle=%d failed rc=%d, skipping", val_h, rc);
                s_gatt_ctx.current_idx++;
                ble_process_next();
            }
        } else {
            ESP_LOGI("LcdDisplay", "ALL %d readable characteristics read. Now subscribing %d NOTIFY chars...",
                     s_gatt_ctx.readable_count, s_gatt_ctx.notify_count);
            s_gatt_ctx.phase = 1;
            s_gatt_ctx.current_idx = 0;
            ble_process_next();
        }
    } else {
        // Phase 1: subscribe to NOTIFY characteristics via CCCD
        if (s_gatt_ctx.current_idx < s_gatt_ctx.notify_count) {
            uint16_t val_h = s_gatt_ctx.notify_handles[s_gatt_ctx.current_idx];
            uint16_t cccd_h = val_h + 1;
            uint8_t cccd_val[2] = {0x01, 0x00};
            int rc = ble_gattc_write_flat(s_gatt_ctx.conn_handle, cccd_h, cccd_val, sizeof(cccd_val), ble_cccd_write_cb, NULL);
            ESP_LOGI("LcdDisplay", "SUBSCRIBING CCCD for val_handle %d (desc %d) rc=%d [%d/%d]",
                     val_h, cccd_h, rc, s_gatt_ctx.current_idx + 1, s_gatt_ctx.notify_count);
        } else {
            ESP_LOGI("LcdDisplay", "ALL %d CCCD SUBSCRIPTIONS COMPLETE!", s_gatt_ctx.notify_count);
            if (s_gatt_ctx.hid_control_point_handle > 0) {
                uint8_t exit_suspend = 0x00;
                int rc = ble_gattc_write_no_rsp_flat(s_gatt_ctx.conn_handle, s_gatt_ctx.hid_control_point_handle, &exit_suspend, 1);
                ESP_LOGI("LcdDisplay", "HID Control Point EXIT SUSPEND (handle=%d) rc=%d", s_gatt_ctx.hid_control_point_handle, rc);
            }
        }
    }
}

static int ble_gattc_disc_chr_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 const struct ble_gatt_chr *chr,
                                 void *arg) {
    if (error->status == 0 && chr) {
        uint16_t uuid16 = ble_uuid_u16(&chr->uuid.u);
        ESP_LOGI("LcdDisplay", "GATT CHR DISCOVERED: val_handle=%d, uuid16=0x%04X, props=0x%02X",
                 chr->val_handle, uuid16, chr->properties);

        // Collect ALL readable characteristics
        if ((chr->properties & BLE_GATT_CHR_PROP_READ) && s_gatt_ctx.readable_count < 20) {
            s_gatt_ctx.readable_handles[s_gatt_ctx.readable_count++] = chr->val_handle;
        }
        // Collect NOTIFY characteristics
        if ((chr->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) &&
            s_gatt_ctx.notify_count < 8) {
            s_gatt_ctx.notify_handles[s_gatt_ctx.notify_count++] = chr->val_handle;
        }
        if (uuid16 == 0x2A4C) {
            s_gatt_ctx.hid_control_point_handle = chr->val_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI("LcdDisplay", "GATT DISCOVERY COMPLETE conn=%d. %d readable, %d notify. Phase 0: reading all...",
                 conn_handle, s_gatt_ctx.readable_count, s_gatt_ctx.notify_count);
        s_gatt_ctx.conn_handle = conn_handle;
        s_gatt_ctx.phase = 0;
        s_gatt_ctx.current_idx = 0;
        ble_process_next();
    }
    return 0;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI("LcdDisplay", "BLE GAP CONNECTED! conn_handle=%d. Initiating SMP Security Pairing...", event->connect.conn_handle);
                int sec_rc = ble_gap_security_initiate(event->connect.conn_handle);
                ESP_LOGI("LcdDisplay", "ble_gap_security_initiate returned rc=%d", sec_rc);
            } else {
                ESP_LOGE("LcdDisplay", "BLE CONNECT FAILED status=%d", event->connect.status);
            }
            break;

        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            ESP_LOGI("LcdDisplay", "BLE PASSKEY ACTION action=%d", event->passkey.params.action);
            struct ble_sm_io pkey = {};
            pkey.action = event->passkey.params.action;
            if (pkey.action == BLE_SM_IOACT_NUMCMP) {
                pkey.numcmp_accept = 1;
            } else if (pkey.action == BLE_SM_IOACT_INPUT) {
                pkey.passkey = 0;
            }
            int pk_rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI("LcdDisplay", "ble_sm_inject_io returned rc=%d", pk_rc);
            break;
        }

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW("LcdDisplay", "BLE DISCONNECTED! Reason=%d (0x%02X)", event->disconnect.reason, event->disconnect.reason);
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI("LcdDisplay", "BLE PAIRING/ENCRYPTION COMPLETE! status=%d. Starting GATT discovery for HID...", event->enc_change.status);
            if (event->enc_change.status == 0) {
                memset(&s_gatt_ctx, 0, sizeof(s_gatt_ctx));
                ble_gattc_disc_all_chrs(event->enc_change.conn_handle, 1, 0xffff, ble_gattc_disc_chr_cb, NULL);
            }
            break;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t data[64] = {0};
            os_mbuf_copydata(event->notify_rx.om, 0, len < 64 ? len : 64, data);

            char hex_buf[200] = {0};
            int pos = 0;
            for (int i = 0; i < len && i < 32; i++) {
                pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", data[i]);
            }
            ESP_LOGI("LcdDisplay", "🎮 GAMEPAD INPUT NOTIFY (handle=%d, len=%d): %s",
                     event->notify_rx.attr_handle, len, hex_buf);
            break;
        }

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            ESP_LOGI("LcdDisplay", "BLE REPEAT PAIRING REQUESTED");
            struct ble_gap_conn_desc desc;
            ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        default:
            break;
    }
    return 0;
}
#endif // CONFIG_BT_ENABLED

#if CONFIG_BT_ENABLED
void LcdDisplay::ConnectToBluetoothDevice(const std::string& name, const std::string& mac, uint8_t addr_type) {
    DisplayLockGuard lock(this);
    bt_connected_device_ = name;
    if (bt_status_label_) {
        lv_label_set_text_fmt(bt_status_label_, "⏳ Сопряжение с %s...", name.c_str());
        lv_obj_set_style_text_color(bt_status_label_, lv_color_hex(0xF59E0B), 0);
    }

    ble_addr_t addr = {};
    addr.type = addr_type;
    unsigned int m[6];
    if (sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        for (int i = 0; i < 6; i++) addr.val[5 - i] = (uint8_t)m[i];
    }

    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;

    int rc = ble_gap_connect(own_addr_type, &addr, 10000, NULL, ble_gap_event_cb, NULL);
    ESP_LOGI("LcdDisplay", "ble_gap_connect (type=%d) to %s returned rc: %d", addr_type, mac.c_str(), rc);

    if (rc != 0 && rc != BLE_HS_EALREADY) {
        addr.type = (addr_type == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
        rc = ble_gap_connect(own_addr_type, &addr, 10000, NULL, ble_gap_event_cb, NULL);
        ESP_LOGI("LcdDisplay", "ble_gap_connect fallback (type=%d) to %s returned rc: %d", addr.type, mac.c_str(), rc);
    }

    if (bt_status_label_) {
        lv_label_set_text_fmt(bt_status_label_, "🟢 Подключено: %s", name.c_str());
        lv_obj_set_style_text_color(bt_status_label_, lv_color_hex(0x10B981), 0);
    }
}
#endif