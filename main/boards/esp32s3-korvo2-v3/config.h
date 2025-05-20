
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_48
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_17
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_18
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_NC//GPIO_NUM_5
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

#ifdef CONFIG_LCD_ST7789
#define DISPLAY_SDA_PIN GPIO_NUM_NC
#define DISPLAY_SCL_PIN GPIO_NUM_NC
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_SWAP_XY false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define BACKLIGHT_INVERT false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#endif

#ifdef CONFIG_LCD_ILI9341
#define LCD_TYPE_ILI9341_SERIAL
#define DISPLAY_SDA_PIN GPIO_NUM_NC
#define DISPLAY_SCL_PIN GPIO_NUM_NC
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

#define DISPLAY_SWAP_XY false
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#define BACKLIGHT_INVERT false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#endif

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

/***********************************************************/
// #define PWDN_GPIO_NUM -1
// #define RESET_GPIO_NUM -1
// #define XCLK_GPIO_NUM 40
// #define SIOD_GPIO_NUM 17
// #define SIOC_GPIO_NUM 18

// #define Y9_GPIO_NUM 39
// #define Y8_GPIO_NUM 41
// #define Y7_GPIO_NUM 42
// #define Y6_GPIO_NUM 12
// #define Y5_GPIO_NUM 3
// #define Y4_GPIO_NUM 14
// #define Y3_GPIO_NUM 47
// #define Y2_GPIO_NUM 13
// #define VSYNC_GPIO_NUM 21
// #define HREF_GPIO_NUM 38
// #define PCLK_GPIO_NUM 11
/****************    摄像头 ↓   ****************************/
#define CAMERA_PIN_PWDN -1
#define CAMERA_PIN_RESET -1
#define CAMERA_PIN_XCLK 40
#define CAMERA_PIN_SIOD 17
#define CAMERA_PIN_SIOC 18

#define CAMERA_PIN_D7 39
#define CAMERA_PIN_D6 41
#define CAMERA_PIN_D5 42
#define CAMERA_PIN_D4 12
#define CAMERA_PIN_D3 3
#define CAMERA_PIN_D2 14
#define CAMERA_PIN_D1 47
#define CAMERA_PIN_D0 13
#define CAMERA_PIN_VSYNC 21
#define CAMERA_PIN_HREF 38
#define CAMERA_PIN_PCLK 11

#define XCLK_FREQ_HZ 24000000

/********************    摄像头 ↑   *************************/
/***********************************************************/
#endif // _BOARD_CONFIG_H_
