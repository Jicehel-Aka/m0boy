/* AKA CORE v1.4: official jmp42/Gamebuino_AKA_lib, gb_ll_common.h.
 * LCD uses an 8-bit i80 bus; the card has its own 4-bit SDMMC bus. */
#ifndef M0_ESP32_CONFIG_H
#define M0_ESP32_CONFIG_H
#define ESP32_PANEL_COLS 320
#define ESP32_PANEL_ROWS 240
#define ESP32_AUDIO_I2S 1
#define ESP32_SD_CMD 38
#define ESP32_SD_SCK 2
#define ESP32_SD_D0 1
#define ESP32_SD_D1 15
#define ESP32_SD_D2 7
#define ESP32_SD_D3 16
#define AKA_I2C_SDA 0
#define AKA_I2C_SCL 43
#define AKA_LCD_WR 21
#define AKA_LCD_DC 48
#define AKA_LCD_BL 45
#define AKA_LCD_TE 47
#define AKA_LCD_DATA {3, 46, 9, 10, 11, 12, 13, 14}
#define AKA_I2S_BCLK 18
#define AKA_I2S_WS 17
#define AKA_I2S_DOUT 8
#endif
