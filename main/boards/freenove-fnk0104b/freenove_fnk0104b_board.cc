#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>

#include <wifi_station.h>
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
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
#include <esp_timer.h>
#include <esp_rom_sys.h>

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

    bool Read(bool& touched, uint16_t& x, uint16_t& y) {
        touched = false;
        x = y = 0;
        if (!dev_) return false;

        uint8_t reg = 0x02;
        uint8_t buf[5] = {};
        if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 5, 50) != ESP_OK) return false;

        const uint8_t points = buf[0] & 0x0F;
        if (points == 0) return true;

        x = static_cast<uint16_t>(((buf[1] & 0x0F) << 8) | buf[2]);
        y = static_cast<uint16_t>(((buf[3] & 0x0F) << 8) | buf[4]);
        if (points > 2 || x > TOUCH_RAW_X_MAX || y > TOUCH_RAW_Y_MAX) return true;

        touched = true;
        return true;
    }

private:
    i2c_master_dev_handle_t dev_;
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

        // This initial board layer keeps the existing simple wake/chat gesture.
        // The next UI step will replace it with LVGL coordinate input for the
        // 320x240 radio and SD-player controls.
        xTaskCreatePinnedToCore([](void* arg) {
            auto* self = static_cast<FreenoveFnk0104bBoard*>(arg);
            auto& app = Application::GetInstance();
            bool down = false;
            uint32_t last_tap = 0;
            uint32_t down_start = 0;
            while (true) {
                bool touched = false;
                uint16_t raw_x = 0;
                uint16_t raw_y = 0;
                self->touch_.Read(touched, raw_x, raw_y);
                const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
                if (touched && !down) {
                    down = true;
                    down_start = now;
                } else if (!touched && down) {
                    down = false;
                    const uint32_t press_ms = now - down_start;
                    if (press_ms > 3000) {
                        self->EnterWifiConfigMode();
                    } else if (now - last_tap < 250) {
                        app.StartListening();
                        last_tap = 0;
                    } else {
                        app.ToggleChatState();
                        last_tap = now;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }, "fnk0104b_touch", 4096, this, 5, nullptr, 0);
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

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
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
