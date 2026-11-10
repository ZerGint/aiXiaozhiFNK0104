#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// FNK0104B uses the same ES8311 audio wiring as the existing FNK0104S
// integration.  The B variant deliberately has no external-I2S amplifier
// option; its output remains on the onboard codec/amplifier path.
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK      GPIO_NUM_4
#define AUDIO_I2S_GPIO_BCLK      GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN       GPIO_NUM_6
#define AUDIO_I2S_GPIO_WS        GPIO_NUM_7
#define AUDIO_I2S_GPIO_DOUT      GPIO_NUM_8
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_1

#define AUDIO_CODEC_I2C_NUM      I2C_NUM_0
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_15
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_16
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

// Microphone: ES8311 digital ADC gain (6 dB steps) plus REG17 fine boost.
#define FNK_MIC_INPUT_GAIN_DB     36
#define FNK_MIC_DIGITAL_BOOST_DB  3.0f

#define BOOT_BUTTON_GPIO         GPIO_NUM_0
#define BUILTIN_LED_GPIO         GPIO_NUM_42

// Display: 2.8-inch ILI9341, landscape 320x240.
#define DISPLAY_BACKLIGHT_PIN    GPIO_NUM_45
#define DISPLAY_RST_PIN          GPIO_NUM_NC
#define DISPLAY_SCK_PIN          GPIO_NUM_12
#define DISPLAY_DC_PIN           GPIO_NUM_46
#define DISPLAY_CS_PIN           GPIO_NUM_10
#define DISPLAY_MOSI_PIN         GPIO_NUM_11
#define DISPLAY_MIS0_PIN         GPIO_NUM_13
#define DISPLAY_SPI_SCLK_HZ      (20 * 1000 * 1000)

#define LCD_SPI_HOST             SPI3_HOST
#define LCD_TYPE_ILI9341_SERIAL
#define DISPLAY_WIDTH            320
#define DISPLAY_HEIGHT           240
#define DISPLAY_MIRROR_X         false
#define DISPLAY_MIRROR_Y         false
#define DISPLAY_SWAP_XY          true
#define DISPLAY_INVERT_COLOR     true
#define DISPLAY_RGB_ORDER        LCD_RGB_ELEMENT_ORDER_BGR
#define DISPLAY_OFFSET_X         0
#define DISPLAY_OFFSET_Y         0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE         0

// FT6336 raw coordinates before the landscape transform.
#define TOUCH_I2C_ADDRESS        0x38
#define TOUCH_RESET_PIN          GPIO_NUM_18
#define TOUCH_RAW_X_MAX          240
#define TOUCH_RAW_Y_MAX          320

// The current storage manager uses 1-bit SDMMC on this board family.
#define SDMMC_CLK_PIN            GPIO_NUM_38
#define SDMMC_CMD_PIN            GPIO_NUM_40
#define SDMMC_D0_PIN             GPIO_NUM_39

#endif  // _BOARD_CONFIG_H_
