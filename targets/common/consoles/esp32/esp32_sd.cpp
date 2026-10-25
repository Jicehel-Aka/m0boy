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

#include "m0_target.h"
#if M0_TARGET_AKA
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
static sdmmc_card_t card;
static bool ready;
extern "C" int m0_sd_init(void) {
    if (ready) return 1;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    host.max_freq_khz = 20000;
    slot.width = 4;
    slot.clk = (gpio_num_t)ESP32_SD_SCK;
    slot.cmd = (gpio_num_t)ESP32_SD_CMD;
    slot.d0 = (gpio_num_t)ESP32_SD_D0;
    slot.d1 = (gpio_num_t)ESP32_SD_D1;
    slot.d2 = (gpio_num_t)ESP32_SD_D2;
    slot.d3 = (gpio_num_t)ESP32_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    if (sdmmc_host_init() != ESP_OK) return 0;
    if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK ||
        sdmmc_card_init(&host, &card) != ESP_OK) {
        sdmmc_host_deinit();
        return 0;
    }
    ready = true;
    return 1;
}
extern "C" uint32_t meta_sd_sectors(void) { return ready ? card.csd.capacity : 0; }
extern "C" int m0_sd_read(uint32_t lba, uint8_t *dst) {
    return ready && lba < card.csd.capacity && sdmmc_read_sectors(&card, dst, lba, 1) == ESP_OK;
}
extern "C" int m0_sd_write(uint32_t lba, const uint8_t *src) {
    return ready && lba < card.csd.capacity && sdmmc_write_sectors(&card, src, lba, 1) == ESP_OK;
}
/* Callers release the old SPI bus before leaving the application. */
extern "C" void m0_sd_deselect(void) {}
#else
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

#endif /* M0_TARGET_AKA */
