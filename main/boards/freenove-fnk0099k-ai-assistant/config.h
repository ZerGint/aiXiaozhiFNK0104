#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// SIMPLEX I2S (отдельные пины для микрофона и динамика)
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

// INMP441 Microphone (I2S Input)
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

// MAX98357A Speaker Amplifier (I2S Output)
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_18
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_21
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_14

#endif

// RGB LED (встроенный на FNK0099K)
#define BUILTIN_LED_GPIO        GPIO_NUM_48

// Пользовательская кнопка
#define BOOT_BUTTON_GPIO        GPIO_NUM_15

// ILI9341 2.8" TFT SPI Display (Landscape 320x240)
#define DISPLAY_CLK_PIN         GPIO_NUM_12
#define DISPLAY_MOSI_PIN        GPIO_NUM_11
#define DISPLAY_MISO_PIN        GPIO_NUM_13
#define DISPLAY_CS_PIN          GPIO_NUM_10
#define DISPLAY_DC_PIN          GPIO_NUM_9
#define DISPLAY_RST_PIN         GPIO_NUM_8
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_7

#define LCD_TYPE_ILI9341_SERIAL
#define DISPLAY_WIDTH           320
#define DISPLAY_HEIGHT          240
#define DISPLAY_MIRROR_X        true
#define DISPLAY_MIRROR_Y        false
#define DISPLAY_SWAP_XY         true
#define DISPLAY_INVERT_COLOR    false
#define DISPLAY_RGB_ORDER       LCD_RGB_ELEMENT_ORDER_BGR
#define DISPLAY_OFFSET_X        0
#define DISPLAY_OFFSET_Y        0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE        0

// XPT2046 Touchscreen (Shared SPI Bus)
#define TOUCH_CS_PIN            GPIO_NUM_16
#define TOUCH_IRQ_PIN           GPIO_NUM_17

#endif // _BOARD_CONFIG_H_
