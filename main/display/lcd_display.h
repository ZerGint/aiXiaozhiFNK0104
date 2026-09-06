#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"
#include "robo_eyes.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000

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
    lv_obj_t* media_title_label_ = nullptr;
    lv_obj_t* media_status_label_ = nullptr;
    lv_obj_t* media_play_label_ = nullptr;
    lv_timer_t* media_update_timer_ = nullptr;
    bool quick_settings_open_ = false;

    // Tabs (AI RoboEyes, Player, Sega Emulator)
    lv_obj_t* panel_roboeyes_ = nullptr;
    lv_obj_t* panel_player_ = nullptr;
    lv_obj_t* panel_sega_ = nullptr;
    lv_obj_t* player_track_label_ = nullptr;
    lv_obj_t* player_status_label_ = nullptr;
    int current_tab_index_ = 0; // 0 = AI RoboEyes, 1 = Player, 2 = Sega

    // Full-Screen Settings Modal (Left Sidebar & Content Panels)
    lv_obj_t* settings_modal_ = nullptr;
    lv_obj_t* settings_sidebar_ = nullptr;
    lv_obj_t* settings_content_area_ = nullptr;
    lv_obj_t* panel_settings_wifi_ = nullptr;
    lv_obj_t* panel_settings_ha_ = nullptr;
    lv_obj_t* panel_settings_info_ = nullptr;
    lv_obj_t* settings_kb_ = nullptr;
    lv_obj_t* ha_url_ta_ = nullptr;
    lv_obj_t* ha_token_ta_ = nullptr;
    lv_obj_t* ha_status_label_ = nullptr;

    // Wi-Fi Interactive Manager
    lv_obj_t* wifi_status_label_ = nullptr;
    lv_obj_t* wifi_ap_list_container_ = nullptr;
    lv_obj_t* wifi_connect_popup_ = nullptr;
    lv_obj_t* wifi_pw_ta_ = nullptr;
    std::string wifi_selected_ssid_;
    bool wifi_selected_open_ = false;

    bool settings_modal_open_ = false;
    int current_settings_cat_ = 0; // 0 = Wi-Fi, 1 = Home Assistant, 2 = Info

    void SetupQuickSettingsOverlay(lv_obj_t* parent);
    void SetupTabPanels(lv_obj_t* parent);
    void SetupMediaPlayerTab(lv_obj_t* parent);
    void SetupSegaEmulatorTab(lv_obj_t* parent);
    void SetupFullSettingsModal(lv_obj_t* parent);
    void SwitchSettingsCategory(int cat_index);
    void UpdateWifiStatusLabel();
    void ScanWifiNetworks();
    void ConnectToWifi(const std::string& ssid, const std::string& password);
    void UpdateMediaControls();

public:
    void ToggleQuickSettings();
    void SwitchTab(int tab_index);
    void OpenSettingsModal();
    void CloseSettingsModal();
    bool IsSettingsModalOpen() const { return settings_modal_open_; }

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
