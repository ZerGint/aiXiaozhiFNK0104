#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"
#include "robo_eyes.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>

#define PREVIEW_IMAGE_DURATION_MS 5000

enum class MediaBrowserMode { Player, Radio };
enum class ActiveMediaSource { None, Player, Radio };

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    lv_obj_t* robo_eyes_canvas_ = nullptr;
    void* robo_eyes_buf_ = nullptr;
    RoboEyes robo_eyes_;
    lv_timer_t* robo_eyes_timer_ = nullptr;
    int robo_eyes_runtime_state_ = -1;
    bool robo_eyes_vad_speaking_ = false;
    void UpdateRoboEyesRuntimeState();
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    // Quick Settings Drop-down Overlay & Tab Navigation
    lv_obj_t* quick_settings_panel_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* volume_val_label_ = nullptr;
    lv_obj_t* brightness_val_label_ = nullptr;
    lv_obj_t* auto_brightness_button_ = nullptr;
    lv_obj_t* auto_brightness_zero_button_ = nullptr;
    lv_obj_t* auto_brightness_timeout_label_ = nullptr;
    lv_obj_t* media_status_label_ = nullptr;
    lv_obj_t* media_title_label_ = nullptr;
    lv_obj_t* media_header_label_ = nullptr;
    bool quick_settings_open_ = false;
    bool auto_brightness_enabled_ = false;
    bool auto_brightness_zero_enabled_ = false;
    bool auto_brightness_dimmed_ = false;
    bool ai_brightness_active_ = false;
    uint8_t auto_brightness_timeout_index_ = 2;
    uint32_t last_display_activity_ms_ = 0;

    lv_obj_t* nav_buttons_[3] = {};
    lv_obj_t* nav_status_[3] = {};
    lv_obj_t* nav_activity_[3] = {};
    lv_obj_t* top_time_label_ = nullptr;
#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S
    lv_obj_t* network_ip_label_ = nullptr;
    lv_obj_t* ha_settings_button_ = nullptr;
#endif
    lv_obj_t* top_volume_value_label_ = nullptr;
    lv_obj_t* top_battery_value_label_ = nullptr;
    lv_obj_t* ai_view_ = nullptr;
    lv_obj_t* radio_view_ = nullptr;
    lv_obj_t* media_shuffle_button_ = nullptr;
    lv_obj_t* media_repeat_button_ = nullptr;
    lv_obj_t* media_stop_button_ = nullptr;
    lv_obj_t* media_play_button_ = nullptr;
    lv_obj_t* media_volume_slider_ = nullptr;
    lv_obj_t* media_favorite_button_ = nullptr;
    lv_obj_t* radio_list_panel_ = nullptr;
    lv_obj_t* media_list_title_label_ = nullptr;
    lv_obj_t* radio_page_label_ = nullptr;
    int radio_page_ = 0;
    int player_page_ = 0;
    bool media_radio_mode_ = false;
    MediaBrowserMode media_browser_mode_ = MediaBrowserMode::Player;
    ActiveMediaSource active_media_source_ = ActiveMediaSource::None;
    char favorite_station_uuid_[65] = {};
    bool favorite_station_cached_ = false;
    bool favorite_station_is_favorite_ = false;
    std::function<void(int)> create_page_;
    lv_timer_t* service_timer_ = nullptr;
#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S
    lv_obj_t* weather_city_label_ = nullptr;
    lv_obj_t* weather_date_label_ = nullptr;
    lv_obj_t* weather_temperature_label_ = nullptr;
    lv_obj_t* weather_description_label_ = nullptr;
    lv_obj_t* weather_wind_label_ = nullptr;
    lv_obj_t* weather_icon_ = nullptr;
    lv_obj_t* weather_forecast_icons_[2] = {};
    lv_obj_t* weather_forecast_temp_labels_[2] = {};
    lv_obj_t* weather_forecast_rain_labels_[2] = {};
    lv_obj_t* weather_updated_label_ = nullptr;
    lv_obj_t* weather_refresh_button_ = nullptr;
    uint32_t weather_generation_ = UINT32_MAX;
    bool weather_time_valid_ = false;
    void UpdateWeatherUI();
    lv_obj_t* boot_overlay_ = nullptr;
    lv_obj_t* boot_ring_ = nullptr;
    lv_obj_t* boot_dot_ = nullptr;
    lv_obj_t* boot_bars_[5] = {};
    lv_timer_t* boot_animation_timer_ = nullptr;
    std::atomic<bool> boot_server_connected_{false};
    uint32_t boot_animation_started_ms_ = 0;
    uint32_t boot_exit_started_ms_ = 0;
    int16_t boot_exit_bar_heights_[5] = {};
    bool boot_min_wait_logged_ = false;
    bool boot_intro_logged_ = false;
    bool boot_exiting_ = false;
    void StartBootAnimation();
    void UpdateBootAnimation();
    void DestroyBootAnimation();
#endif
    void UpdateServiceIndicators();
    bool IsStationFavorite(const std::string& uuid);
    void SetStationFavoriteCache(const std::string& uuid, bool is_favorite);
    void UpdateAutoBrightness();
    void UpdateAutoBrightnessControls();
    void SetAutoBrightnessEnabled(bool enabled);
    void SetAutoBrightnessZeroEnabled(bool enabled);
    void AdjustAutoBrightnessTimeout(int delta);
    void RestoreSystemBrightness();

    // Tabs (AI RoboEyes, Player, Sega Emulator)
    lv_obj_t* panel_roboeyes_ = nullptr;
    lv_obj_t* panel_player_ = nullptr;
    lv_obj_t* panel_sega_ = nullptr;
    lv_obj_t* player_track_label_ = nullptr;
    lv_obj_t* player_status_label_ = nullptr;
    int current_tab_index_ = 0; // 0 = AI RoboEyes, 1 = Player, 2 = Sega

    void SetupQuickSettingsOverlay(lv_obj_t* parent);
    void SetupTabPanels(lv_obj_t* parent);
    void SetupMediaPlayerTab(lv_obj_t* parent);
    void SetupSegaEmulatorTab(lv_obj_t* parent);
    void SwitchSettingsCategory(int cat_index);
    void UpdateWifiStatusLabel();
    void RefreshRadioCatalogPage();

public:
    void ToggleQuickSettings();
    void SwitchTab(int tab_index);
    void RegisterDisplayActivity(const char* source = "OTHER_UI");
    bool WakeDisplayFromTouch();

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S
    virtual void UpdateStatusBar(bool update_all = false) override;
#endif
    virtual void OnServerConnected() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);

    bool IsQuickSettingsOpen() const { return quick_settings_open_; }
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
