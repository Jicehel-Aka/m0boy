/* Gamebuino AKA facts for the shared ESP32 drivers: ESP32-S3 (16 MiB flash,
 * 8 MiB octal PSRAM), 320x240 panel on the SPI, the speaker behind I2S.
 * The console is not documented publicly yet: the pins below are the
 * working assumption, to correct in one place when the real ones are
 * known (aka_hw.cpp holds the rest). */
#ifndef M0_ESP32_CONFIG_H
#define M0_ESP32_CONFIG_H

#define ESP32_PANEL_COLS 320
#define ESP32_PANEL_ROWS 240

#define ESP32_AUDIO_I2S 1

#define ESP32_LCD_SCK 12
#define ESP32_LCD_MOSI 11
#define ESP32_LCD_MISO 13

#define ESP32_SD_CS 21
#define ESP32_SD_MOSI 11
#define ESP32_SD_MISO 13
#define ESP32_SD_SCK 12
#define ESP32_SD_HZ_INIT 400000
#define ESP32_SD_HZ 20000000

#endif
