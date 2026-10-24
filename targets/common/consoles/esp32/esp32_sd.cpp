/* The SD card over the panel's SPI bus (they share it on the ODROID-GO):
 * the byte-level hooks m0_sd.c's protocol engine drives (m0_sd.h's "bus",
 * one per console).  The identification rate and the transfer rate come
 * from esp32_config.h; the bus is taken only while a card exchange runs. */
#include <Arduino.h>
#include <SPI.h>
#include <esp_timer.h>
#include "esp32_hw.h"
extern "C" {
#include "m0_sd.h"
}

#include "esp32_config.h"

static SPIClass *spi;
static uint32_t bus_hz = ESP32_SD_HZ_INIT;
static uint32_t slower_at;

static SPIClass *bus(void) {
    if (!spi) spi = (SPIClass *)esp32_lcd_bus();
    return spi;
}

extern "C" void m0_sd_deselect(void) {
    digitalWrite(ESP32_SD_CS, HIGH);
    if (bus()) bus()->transfer(0xFF);
}
extern "C" void m0_sd_bus_begin(int init) {
    bus_hz = init ? ESP32_SD_HZ_INIT : ESP32_SD_HZ;
    if (bus()) bus()->beginTransaction(SPISettings(bus_hz, MSBFIRST, SPI_MODE0));
    digitalWrite(ESP32_SD_CS, LOW);
}
extern "C" void meta_sd_bus_end(int init, int ok) {
    (void)init;
    digitalWrite(ESP32_SD_CS, HIGH);
    if (bus()) bus()->endTransaction();
    if (ok && bus_hz < ESP32_SD_HZ) return;
    if (!ok) slower_at = (uint32_t)esp_timer_get_time();
}
extern "C" void m0_sd_cs(int selected) { digitalWrite(ESP32_SD_CS, selected ? LOW : HIGH); }
extern "C" uint8_t m0_sd_xfer(uint8_t out) { return bus() ? bus()->transfer(out) : 0xFF; }
extern "C" void meta_sd_recv(uint8_t *dst, unsigned n) {
    if (!bus()) {
        while (n--) *dst++ = 0xFF;
        return;
    }
    while (n--) *dst++ = bus()->transfer(0xFF);
}
extern "C" int meta_sd_block_ok(uint16_t crc) {
    (void)crc;
    uint8_t a = bus() ? bus()->transfer(0xFF) : 0, b = bus() ? bus()->transfer(0xFF) : 0;
    (void)a; (void)b; /* the block's CRC16, read and not checked */
    return 1;
}
extern "C" int meta_sd_slower(void) {
    /* after a failure at the full rate: worth one retry at 10 MHz */
    return bus_hz > 10000000 ? (bus_hz = 10000000, 1) : 0;
}
extern "C" uint32_t meta_sd_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
