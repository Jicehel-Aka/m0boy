/* ODROID-GO facts for the shared ESP32 drivers (targets/common/consoles/
 * esp32/esp32_config.h's model): ILI9341 320x240 and the microSD share the
 * VSPI bus, the speaker is the ESP32's internal DAC on GPIO25. */
#ifndef M0_ESP32_CONFIG_H
#define M0_ESP32_CONFIG_H

#define ESP32_PANEL_COLS 320
#define ESP32_PANEL_ROWS 240

#define ESP32_AUDIO_DAC 1

#define ESP32_LCD_SCK 18
#define ESP32_LCD_MOSI 23
#define ESP32_LCD_MISO 19

#define ESP32_SD_CS 22
#define ESP32_SD_MOSI 23
#define ESP32_SD_MISO 19
#define ESP32_SD_SCK 18
#define ESP32_SD_HZ_INIT 400000
#define ESP32_SD_HZ 20000000

#endif
