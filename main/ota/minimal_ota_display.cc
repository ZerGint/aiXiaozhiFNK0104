#include "minimal_ota_display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S

#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_st7796.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include "boards/freenove-fnk0104s/config.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

#endif

namespace {

constexpr char kTag[] = "MinimalOtaDisplay";

struct MemorySnapshot {
    size_t internal_free;
    size_t internal_largest;
    size_t psram_free;
};

MemorySnapshot SnapshotMemory() {
    return {
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
    };
}

void LogMemory(const char* phase, const MemorySnapshot& memory) {
    ESP_LOGI(kTag, "OTA_UI_MEM_%s internal_free=%u internal_largest=%u psram_free=%u", phase,
             static_cast<unsigned>(memory.internal_free),
             static_cast<unsigned>(memory.internal_largest),
             static_cast<unsigned>(memory.psram_free));
}

}  // namespace

#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S

namespace {

esp_lcd_panel_io_handle_t s_panel_io = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;
lv_display_t* s_display = nullptr;
lv_obj_t* s_screen = nullptr;
lv_obj_t* s_label = nullptr;

bool InitializePanel() {
    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = DISPLAY_MOSI_PIN;
    bus_config.miso_io_num = DISPLAY_MIS0_PIN;
    bus_config.sclk_io_num = DISPLAY_SCK_PIN;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    if (spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(kTag, "SPI initialization failed");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_CS_PIN;
    io_config.dc_gpio_num = DISPLAY_DC_PIN;
    io_config.spi_mode = DISPLAY_SPI_MODE;
    io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
    io_config.trans_queue_depth = 4;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &s_panel_io) != ESP_OK) {
        ESP_LOGE(kTag, "LCD panel IO initialization failed");
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = DISPLAY_RST_PIN;
    panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7796(s_panel_io, &panel_config, &s_panel) != ESP_OK) {
        ESP_LOGE(kTag, "ST7796 panel initialization failed");
        return false;
    }

    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_invert_color(s_panel, DISPLAY_INVERT_COLOR) != ESP_OK ||
        esp_lcd_panel_swap_xy(s_panel, DISPLAY_SWAP_XY) != ESP_OK ||
        esp_lcd_panel_mirror(s_panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y) != ESP_OK) {
        ESP_LOGE(kTag, "ST7796 panel setup failed");
        return false;
    }

    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << DISPLAY_BACKLIGHT_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&backlight_config) != ESP_OK ||
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT ? 0 : 1) != ESP_OK) {
        ESP_LOGE(kTag, "Backlight initialization failed");
        return false;
    }

    const esp_err_t display_power = esp_lcd_panel_disp_on_off(s_panel, true);
    if (display_power != ESP_OK && display_power != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(kTag, "Display power-on failed: %s", esp_err_to_name(display_power));
        return false;
    }
    return true;
}

}  // namespace

#endif

namespace {
bool s_initialized = false;
}

bool MinimalOtaDisplay::Init() {
    if (s_initialized) return true;

    const MemorySnapshot before = SnapshotMemory();
    LogMemory("BEFORE", before);

#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S
    if (!InitializePanel()) return false;

    lv_init();
    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    port_config.task_priority = 1;
    port_config.task_stack = 4096;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_config.task_affinity = 1;
#endif
    lvgl_port_init(&port_config);

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(DISPLAY_WIDTH * 10),
        .double_buffer = false,
        .trans_size = 0,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation =
            {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
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
    s_display = lvgl_port_add_disp(&display_config);
    if (s_display == nullptr) {
        ESP_LOGE(kTag, "Minimal LVGL display registration failed");
        return false;
    }
#else
    ESP_LOGW(kTag, "Minimal updater display is unsupported on this board");
#endif

    const MemorySnapshot after = SnapshotMemory();
    LogMemory("AFTER", after);
    ESP_LOGI(kTag, "OTA_UI_MEM_DELTA internal=%ld largest=%ld psram=%ld",
             static_cast<long>(after.internal_free) - static_cast<long>(before.internal_free),
             static_cast<long>(after.internal_largest) - static_cast<long>(before.internal_largest),
             static_cast<long>(after.psram_free) - static_cast<long>(before.psram_free));
    s_initialized = true;
    ShowUpdating();
    return true;
}

void MinimalOtaDisplay::ShowUpdating() {
#if CONFIG_BOARD_TYPE_FREENOVE_FNK0104S
    if (!s_initialized || s_display == nullptr) return;
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(kTag, "Unable to lock minimal LVGL display");
        return;
    }
    if (s_screen == nullptr) {
        s_screen = lv_obj_create(nullptr);
        lv_obj_remove_style_all(s_screen);
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

        s_label = lv_label_create(s_screen);
        lv_label_set_text(s_label, "Обновление");
        lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_label, &BUILTIN_TEXT_FONT, 0);
        lv_obj_align(s_label, LV_ALIGN_CENTER, 0, 0);
    }
    lv_disp_load_scr(s_screen);
    lv_refr_now(s_display);
    lvgl_port_unlock();
    ESP_LOGI(kTag, "OTA_UI_READY text=Обновление background=black position=center");
#endif
}
