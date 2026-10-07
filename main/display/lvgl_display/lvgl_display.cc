#include <esp_err.h>
#include <esp_log.h>
#include <material_symbols.h>
#include <cstdlib>
#include <cstring>
#include <string>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "dynamic_glyph_cache.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "Display"

LvglDisplay::LvglDisplay() {
    dynamic_glyph_cache_ = std::make_unique<DynamicGlyphCache>();
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback =
            [](void* arg) {
                LvglDisplay* display = static_cast<LvglDisplay*>(arg);
                DisplayLockGuard lock(display);
                lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

bool LvglDisplay::SetTextFont(std::shared_ptr<LvglFont> text_font) {
    if (text_font == nullptr || text_font->font() == nullptr) {
        return false;
    }

    DisplayLockGuard lock(this);
    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");
    if (light_theme == nullptr && dark_theme == nullptr) {
        return false;
    }

    // LVGL styles keep raw lv_font_t pointers. Keep the previous font owners alive until the
    // current theme has rebound every style to the new font.
    auto previous_light_font = light_theme != nullptr ? light_theme->text_font() : nullptr;
    auto previous_dark_font = dark_theme != nullptr ? dark_theme->text_font() : nullptr;
    if (light_theme != nullptr) {
        light_theme->set_text_font(text_font);
    }
    if (dark_theme != nullptr) {
        dark_theme->set_text_font(text_font);
    }
    if (current_theme_ != nullptr) {
        SetTheme(current_theme_);
    }
    previous_light_font.reset();
    previous_dark_font.reset();
    return true;
}

bool LvglDisplay::AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) {
    if (dynamic_glyph_cache_ == nullptr) {
        return false;
    }
    if (glyphs.empty()) {
        if (!TextGlyphStorageUsesPsram()) {
            ClearTextGlyphs();
        }
        return false;
    }
    if (bpp != 1 && bpp != 4) {
        return false;
    }

    DisplayLockGuard lock(this);
    auto theme = dynamic_cast<LvglTheme*>(current_theme_);
    if (theme == nullptr || theme->text_font() == nullptr) {
        return false;
    }

    auto fallback = dynamic_glyph_cache_->EnsureFont(theme->text_font()->font(), bpp);
    if (fallback == nullptr) {
        return false;
    }
    theme->text_font()->SetFallback(fallback);
    return dynamic_glyph_cache_->AddGlyphs(glyphs);
}

void LvglDisplay::ClearTextGlyphs() {
    if (dynamic_glyph_cache_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    dynamic_glyph_cache_->Clear();
}

LvglDisplay::~LvglDisplay() {
    generated_ready_callback_ = {};
    if (generated_ready_popup_ != nullptr) {
        lv_obj_del(generated_ready_popup_);
    }
    if (generated_download_overlay_ != nullptr) {
        lv_obj_del(generated_download_overlay_);
    }
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if (low_battery_popup_ != nullptr) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::ShowGeneratedReadyPrompt(const char* title, std::function<void(bool)> callback) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;

    if (generated_ready_popup_ == nullptr) {
        auto screen = lv_screen_active();
        generated_ready_popup_ = lv_obj_create(screen);
        // Absorb touches outside the dialog buttons so controls underneath
        // cannot be activated through the modal window.
        lv_obj_add_flag(generated_ready_popup_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(generated_ready_popup_, LV_HOR_RES * 0.90, 128);
        lv_obj_center(generated_ready_popup_);
        lv_obj_set_style_radius(generated_ready_popup_, 14, 0);
        lv_obj_set_style_bg_color(generated_ready_popup_, lv_color_hex(0x20252B), 0);
        lv_obj_set_style_bg_opa(generated_ready_popup_, LV_OPA_90, 0);
        lv_obj_set_style_border_width(generated_ready_popup_, 1, 0);
        lv_obj_set_style_border_color(generated_ready_popup_, lv_color_hex(0x55C2FF), 0);
        lv_obj_set_style_pad_all(generated_ready_popup_, 10, 0);
        lv_obj_clear_flag(generated_ready_popup_, LV_OBJ_FLAG_SCROLLABLE);

        generated_ready_title_ = lv_label_create(generated_ready_popup_);
        lv_obj_set_width(generated_ready_title_, LV_HOR_RES * 0.78);
        lv_obj_align(generated_ready_title_, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_set_style_text_align(generated_ready_title_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(generated_ready_title_, lv_color_white(), 0);

        auto make_button = [this](const char* text, lv_align_t align, int x) {
            lv_obj_t* button = lv_btn_create(generated_ready_popup_);
            lv_obj_set_size(button, 88, 42);
            // Keep the buttons inside the panel instead of letting their lower
            // edge touch or clip against the dialog frame.
            lv_obj_align(button, align, x, -8);
            lv_obj_t* label = lv_label_create(button);
            lv_label_set_text(label, text);
            lv_obj_center(label);
            lv_obj_add_event_cb(button,
                                [](lv_event_t* event) {
                                    auto* display = static_cast<LvglDisplay*>(lv_event_get_user_data(event));
                                    if (display == nullptr) return;
                                    const bool confirmed = lv_event_get_target(event) == display->generated_ready_yes_button_;
                                    auto callback = std::move(display->generated_ready_callback_);
                                    lv_obj_add_flag(display->generated_ready_popup_, LV_OBJ_FLAG_HIDDEN);
                                    if (callback) callback(confirmed);
                                },
                                LV_EVENT_CLICKED, this);
            return button;
        };
        generated_ready_no_button_ = make_button("Нет", LV_ALIGN_BOTTOM_LEFT, 4);
        generated_ready_yes_button_ = make_button("Скачать", LV_ALIGN_BOTTOM_RIGHT, -4);
    }

    std::string prompt = "Трек готов. Скачать?";
    if (title != nullptr && title[0] != '\0') {
        prompt = std::string(title) + "\nСкачать?";
    }
    lv_label_set_text(generated_ready_title_, prompt.c_str());
    generated_ready_callback_ = std::move(callback);
    lv_obj_clear_flag(generated_ready_popup_, LV_OBJ_FLAG_HIDDEN);
}

void LvglDisplay::ShowGeneratedDownloadProgress(const char* title, int percent) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    if (generated_download_overlay_ == nullptr) {
        auto screen = lv_screen_active();
        generated_download_overlay_ = lv_obj_create(screen);
        // The progress window is modal while a download is active.
        lv_obj_add_flag(generated_download_overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(generated_download_overlay_, LV_HOR_RES * 0.86, 92);
        lv_obj_center(generated_download_overlay_);
        lv_obj_set_style_radius(generated_download_overlay_, 14, 0);
        lv_obj_set_style_bg_color(generated_download_overlay_, lv_color_hex(0x20252B), 0);
        lv_obj_set_style_bg_opa(generated_download_overlay_, LV_OPA_90, 0);
        lv_obj_set_style_border_width(generated_download_overlay_, 1, 0);
        lv_obj_set_style_border_color(generated_download_overlay_, lv_color_hex(0x55C2FF), 0);
        lv_obj_set_style_pad_all(generated_download_overlay_, 10, 0);
        lv_obj_clear_flag(generated_download_overlay_, LV_OBJ_FLAG_SCROLLABLE);

        generated_download_title_ = lv_label_create(generated_download_overlay_);
        lv_obj_align(generated_download_title_, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_width(generated_download_title_, LV_HOR_RES * 0.78);
        lv_obj_set_style_text_align(generated_download_title_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(generated_download_title_, lv_color_white(), 0);

        generated_download_bar_ = lv_bar_create(generated_download_overlay_);
        lv_obj_set_size(generated_download_bar_, LV_HOR_RES * 0.70, 12);
        lv_obj_align(generated_download_bar_, LV_ALIGN_CENTER, 0, 10);
        lv_bar_set_range(generated_download_bar_, 0, 100);

        generated_download_percent_ = lv_label_create(generated_download_overlay_);
        lv_obj_align(generated_download_percent_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_text_color(generated_download_percent_, lv_color_white(), 0);
    }
    lv_label_set_text(generated_download_title_, title != nullptr ? title : "Downloading song");
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    lv_bar_set_value(generated_download_bar_, percent, LV_ANIM_OFF);
    lv_label_set_text_fmt(generated_download_percent_, "%d%%", percent);
    lv_obj_clear_flag(generated_download_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void LvglDisplay::HideGeneratedDownloadProgress() {
    DisplayLockGuard lock(this);
    if (generated_download_overlay_ != nullptr) {
        lv_obj_add_flag(generated_download_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but "
                     "label not created)",
                     status);
        }
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!",
                 notification);
    }
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "ShowNotification('%s') failed: notification_label_ is nullptr (SetupUI() was "
                     "called but label not created)",
                     notification);
        }
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // Update icon if mute state changes
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, MATERIAL_SYMBOLS_VOLUME_OFF);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time — show HH:MM when idle; refresh immediately on state
    // transitions (update_all=true) and on every minute change thereafter.
    if (app.GetDeviceState() == kDeviceStateIdle) {
        time_t now = time(NULL);
        struct tm* tm_now = localtime(&now);
        if (tm_now->tm_year >= 2025 - 1900) {
            int cur_min = tm_now->tm_hour * 60 + tm_now->tm_min;
            if (update_all || cur_min != last_displayed_clock_min_) {
                last_displayed_clock_min_ = cur_min;
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", tm_now);
                SetStatus(time_str);
            }
        } else {
            ESP_LOGW(TAG, "System time not set (tm_year=%d)", tm_now->tm_year);
        }
    } else {
        // Reset so the clock re-appears immediately when idle resumes.
        last_displayed_clock_min_ = -1;
    }

    esp_pm_lock_acquire(pm_lock_);
    // Update battery icon
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
        } else {
            const char* levels[] = {
                MATERIAL_SYMBOLS_BATTERY_ANDROID_0,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
            };
            int level_index = battery_level <= 0
                                  ? 0
                                  : (battery_level >= 100 ? 7 : 1 + ((battery_level - 1) * 6 / 99));
            icon = levels[level_index];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        // Check low battery popup only when clock tick event is triggered
        // Because when initializing, the battery level is not ready yet.
        if (low_battery_popup_ != nullptr && !update_all) {
            if (strcmp(icon, MATERIAL_SYMBOLS_BATTERY_ANDROID_0) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_,
                                    LV_OBJ_FLAG_HIDDEN)) {  // Show if low battery popup is hidden
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.Schedule([&app]() { app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY); });
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_,
                                     LV_OBJ_FLAG_HIDDEN)) {  // Hide if low battery popup is shown
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // Update network icon every 10 seconds
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,      kDeviceStateStarting,   kDeviceStateWifiConfiguring,
            kDeviceStateListening, kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) !=
            allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {}

void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret =
        image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w,
                         draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
                         [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                             std::string* output = static_cast<std::string*>(arg);
                             if (data && len > 0) {
                                 output->append(static_cast<const char*>(data), len);
                             }
                             return len;
                         },
                         &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}
