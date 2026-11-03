#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// Buttons
#define BOOT_BUTTON_GPIO        GPIO_NUM_9
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_7   // right-hand button (+)
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_8   // left-hand button (-)

// VB6824 audio chip (UART). The chip runs the hardware wake word ("你好小智")
// and voice commands locally; the mic link delivers OPUS packets (decoded to
// 16 kHz PCM on the ESP32), playback takes plain 16 kHz PCM
// (CONFIG_VB6824_TYPE_OPUS_16K_20MS_PCM_16K=y).
#define CODEC_TX_GPIO           GPIO_NUM_20
#define CODEC_RX_GPIO           GPIO_NUM_10

// Display: 1.54" ST7789 240x240 (FPC-HJC-JCD-154)
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_5
#define DISPLAY_MOSI_PIN      GPIO_NUM_1
#define DISPLAY_CLK_PIN       GPIO_NUM_3
#define DISPLAY_DC_PIN        GPIO_NUM_0
#define DISPLAY_RST_PIN       GPIO_NUM_2
#define DISPLAY_CS_PIN        GPIO_NUM_12

#define DISPLAY_WIDTH         240
#define DISPLAY_HEIGHT        240
#define DISPLAY_MIRROR_X      false
#define DISPLAY_MIRROR_Y      false
#define DISPLAY_SWAP_XY       false
#define DISPLAY_INVERT_COLOR  true
#define DISPLAY_RGB_ORDER     LCD_RGB_ELEMENT_ORDER_RGB

#define DISPLAY_OFFSET_X      0
#define DISPLAY_OFFSET_Y      0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#endif // _BOARD_CONFIG_H_
