#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>

#include <wifi_station.h>
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "fnk0104b_lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "adc_battery_monitor.h"
#include "home_assistant.h"
#include "managers/storage_manager.h"
#include "media/radio_browser.h"
#include "weather/weather_service.h"
#include "music/music_service.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_rom_sys.h>
#include <lvgl.h>
#include <algorithm>
#include <atomic>

#include "led/single_led.h"

#define TAG "FreenoveFnk0104bBoard"

const char* GetCompiledAudioOutputVariant() {
    return "internal_amp";
}

class TouchDriver {
public:
    TouchDriver() : dev_(nullptr) {}

    bool Init(i2c_master_bus_handle_t bus, uint8_t addr) {
        i2c_device_config_t cfg = {
            .device_address = addr,
            .scl_speed_hz = 400000,
            .scl_wait_us = 0,
        };
        return i2c_master_bus_add_device(bus, &cfg, &dev_) == ESP_OK;
    }

    bool StartPolling() {
        return xTaskCreate(
                   [](void* arg) {
                       auto* self = static_cast<TouchDriver*>(arg);
                       while (true) {
                           bool touched = false;
                           uint16_t x = 0;
                           uint16_t y = 0;
                           if (!self->ReadHardware(touched, x, y)) {
                               touched = false;
                           }
                           const uint32_t packed = (touched ? 1U : 0U) |
                                                   (static_cast<uint32_t>(x) << 1) |
                                                   (static_cast<uint32_t>(y) << 10);
                           self->latest_sample_.store(packed, std::memory_order_release);
                           vTaskDelay(pdMS_TO_TICKS(20));
                       }
                   },
                   "fnk0104b_touch", 3072, this, 5, nullptr) == pdPASS;
    }

    void ReadSample(bool& touched, uint16_t& x, uint16_t& y) const {
        const uint32_t packed = latest_sample_.load(std::memory_order_acquire);
        touched = (packed & 1U) != 0;
        x = static_cast<uint16_t>((packed >> 1) & 0x1FFU);
        y = static_cast<uint16_t>((packed >> 10) & 0x1FFU);
    }

private:
    bool ReadHardware(bool& touched, uint16_t& x, uint16_t& y) {
        touched = false;
        x = y = 0;
        if (!dev_)
            return false;

        uint8_t reg = 0x02;
        uint8_t buf[5] = {};
        if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 5, 50) != ESP_OK)
            return false;

        const uint8_t points = buf[0] & 0x0F;
        if (points == 0)
            return true;

        x = static_cast<uint16_t>(((buf[1] & 0x0F) << 8) | buf[2]);
        y = static_cast<uint16_t>(((buf[3] & 0x0F) << 8) | buf[4]);
        if (points > 2 || x > TOUCH_RAW_X_MAX || y > TOUCH_RAW_Y_MAX)
            return true;

        touched = true;
        return true;
    }

    i2c_master_dev_handle_t dev_;
    std::atomic<uint32_t> latest_sample_{0};
};

class FreenoveFnk0104bBoard : public WifiBoard {
private:
    Button boot_button_;
    Display* display_ = nullptr;
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    TouchDriver touch_;
    AdcBatteryMonitor* adc_battery_monitor_ = nullptr;

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_8, 200000, 200000, GPIO_NUM_NC);
    }

    void InitializeTouch() {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << TOUCH_RESET_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&rst_cfg));
        gpio_set_level(TOUCH_RESET_PIN, 0);
        esp_rom_delay_us(10000);
        gpio_set_level(TOUCH_RESET_PIN, 1);
        esp_rom_delay_us(50000);

        if (!touch_.Init(codec_i2c_bus_, TOUCH_I2C_ADDRESS)) {
            ESP_LOGE(TAG, "Failed to initialize FT6336 touch driver at 0x%02x", TOUCH_I2C_ADDRESS);
            return;
        }

        auto* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, [](lv_indev_t* input, lv_indev_data_t* data) {
            auto* self = static_cast<FreenoveFnk0104bBoard*>(lv_indev_get_user_data(input));
            bool touched = false;
            uint16_t raw_x = 0;
            uint16_t raw_y = 0;
            self->touch_.ReadSample(touched, raw_x, raw_y);

            static int16_t last_x = 0;
            static int16_t last_y = 0;
            static bool was_pressed = false;
            static bool wake_only_gesture = false;
            static bool wake_long_press_handled = false;
            static TickType_t press_started_ticks = 0;
            static uint8_t confirm_count = 0;
            static uint16_t candidate_x = 0;
            static uint16_t candidate_y = 0;

            if (touched) {
                if (!was_pressed) {
                    const uint16_t dx =
                        raw_x > candidate_x ? raw_x - candidate_x : candidate_x - raw_x;
                    const uint16_t dy =
                        raw_y > candidate_y ? raw_y - candidate_y : candidate_y - raw_y;
                    if (confirm_count == 0 || dx > 12 || dy > 12) {
                        candidate_x = raw_x;
                        candidate_y = raw_y;
                        confirm_count = 1;
                        data->state = LV_INDEV_STATE_RELEASED;
                        data->point.x = last_x;
                        data->point.y = last_y;
                        return;
                    }
                    if (++confirm_count < 2) {
                        data->state = LV_INDEV_STATE_RELEASED;
                        return;
                    }
                }

                last_x = std::min<uint16_t>(raw_y, DISPLAY_WIDTH - 1);
                last_y = DISPLAY_HEIGHT - 1 - std::min<uint16_t>(raw_x, DISPLAY_HEIGHT - 1);
                data->point.x = last_x;
                data->point.y = last_y;
                if (!was_pressed) {
                    was_pressed = true;
                    press_started_ticks = xTaskGetTickCount();
                    wake_long_press_handled = false;
                    auto* lcd = static_cast<LcdDisplay*>(self->GetDisplay());
                    wake_only_gesture = lcd && lcd->WakeDisplayFromTouch();
                }
                data->state = wake_only_gesture ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
                if (wake_only_gesture && !wake_long_press_handled &&
                    xTaskGetTickCount() - press_started_ticks >= pdMS_TO_TICKS(3000)) {
                    wake_long_press_handled = true;
                    Application::GetInstance().Schedule(
                        [self]() { self->EnterWifiConfigMode(); });
                }
            } else {
                data->state = LV_INDEV_STATE_RELEASED;
                data->point.x = last_x;
                data->point.y = last_y;
                if (was_pressed) {
                    was_pressed = false;
                    wake_only_gesture = false;
                    wake_long_press_handled = false;
                }
                confirm_count = 0;
            }
        });
        lv_indev_set_user_data(indev, this);
        lv_indev_set_long_press_time(indev, 3000);
        lv_indev_add_event_cb(
            indev,
            [](lv_event_t* event) {
                auto* self = static_cast<FreenoveFnk0104bBoard*>(lv_event_get_user_data(event));
                Application::GetInstance().Schedule([self]() { self->EnterWifiConfigMode(); });
            },
            LV_EVENT_LONG_PRESSED, this);
        if (!touch_.StartPolling()) {
            ESP_LOGE(TAG, "Failed to start FT6336 polling task");
        }
    }

    void InitializeI2c() {
        i2c_master_bus_config_t cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t cfg = {};
        cfg.mosi_io_num = DISPLAY_MOSI_PIN;
        cfg.miso_io_num = DISPLAY_MIS0_PIN;
        cfg.sclk_io_num = DISPLAY_SCK_PIN;
        cfg.quadwp_io_num = GPIO_NUM_NC;
        cfg.quadhd_io_num = GPIO_NUM_NC;
        cfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &cfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = DISPLAY_CS_PIN;
        io_cfg.dc_gpio_num = DISPLAY_DC_PIN;
        io_cfg.spi_mode = DISPLAY_SPI_MODE;
        io_cfg.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_cfg.trans_queue_depth = 10;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &panel_io));

        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = DISPLAY_RST_PIN;
        panel_cfg.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_cfg.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_cfg, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        display_ = new Fnk0104bLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                          DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                          DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        StorageManager::GetInstance().InitializeSdCard();
        HomeAssistant::GetInstance().Initialize();
        RadioBrowser::GetInstance().RegisterMcpTools();
        WeatherService::GetInstance().RegisterMcpTools();
        MusicService::GetInstance().Initialize();
    }

public:
    FreenoveFnk0104bBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeBatteryMonitor();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, AUDIO_CODEC_I2C_NUM, AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!adc_battery_monitor_) return false;
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(FreenoveFnk0104bBoard);
