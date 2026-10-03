/*
 * Pokitto SD card on SSP0 (PokittoLib's wiring: MOSI P0_9, MISO P0_8,
 * SCK P0_6, chip select P0_7 as GPIO), SPI mode 0, 8-bit frames.
 *
 * Same card protocol as the META's meta_sd.cpp: 74+ clocks with CS high,
 * CMD0, CMD8, ACMD41 until ready, CMD58 for the addressing mode, then
 * single-sector reads (CMD17) and writes (CMD24).  Initialisation runs at
 * about 400 kHz, transfers at up to 24 MHz (the SSP divides the CPU clock
 * by at least two).
 */
#include "meta_sd.h"

#include "lpc11u6x.h"
#include "meta_clock.h"
#include "pokitto_hw.h"

#define CS_BIT (1u << 7)
#define SD_FAST_HZ 24000000u
#define SD_SLOW_HZ 400000u

static uint8_t block_addressing; /* SDHC/SDXC: sector numbers, else bytes */
static uint8_t fast;             /* initialised: transfer rate */

static void cs_high(void) { GPIO_SET(0) = CS_BIT; }
static void cs_low(void) { GPIO_CLR(0) = CS_BIT; }

/* SCK = clock / (CPSR * (SCR + 1)), CPSR even, at most hz. */
static void set_rate(uint32_t clock, uint32_t hz) {
    uint32_t div = (clock + hz - 1u) / hz;
    if (div < 2u) div = 2u;
    uint32_t cpsr = 2u, scr = (div + 1u) / 2u - 1u;
    while (scr > 255u) {
        cpsr += 2u;
        scr = (div + cpsr - 1u) / cpsr - 1u;
    }
    SSP0_CR1 = 0;
    SSP0_CPSR = cpsr;
    SSP0_CR0 = 0x07u | (scr << 8); /* 8 bits, SPI, CPOL 0, CPHA 0 */
    SSP0_CR1 = 2u;                  /* enabled, master */
}

void pokitto_sd_clock_changed(uint32_t hz) {
    if (!(SYSCON_SYSAHBCLKCTRL & AHB_SSP0)) return;
    set_rate(hz, fast ? SD_FAST_HZ : SD_SLOW_HZ);
}

static uint8_t xfer(uint8_t b) {
    while (!(SSP0_SR & SSP_SR_TNF)) {}
    SSP0_DR = b;
    while (!(SSP0_SR & SSP_SR_RNE)) {}
    return (uint8_t)SSP0_DR;
}

void meta_sd_deselect(void) {
    IOCON_SET(IOCON_PIO0(7), IOCON_FUNC(0), IOCON_MODE_PULLUP);
    GPIO_DIR(0) |= CS_BIT;
    cs_high();
}

static uint8_t command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    xfer(0xFF);
    xfer((uint8_t)(0x40 | cmd));
    xfer((uint8_t)(arg >> 24));
    xfer((uint8_t)(arg >> 16));
    xfer((uint8_t)(arg >> 8));
    xfer((uint8_t)arg);
    xfer(crc);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && (r & 0x80); i++) r = xfer(0xFF);
    return r;
}

int meta_sd_init(void) {
    meta_sd_deselect();
    SYSCON_SYSAHBCLKCTRL |= AHB_SSP0;
    SYSCON_PRESETCTRL |= PRESET_SSP0;
    SYSCON_SSP0CLKDIV = 1;
    IOCON_SET(IOCON_PIO0(6), IOCON_FUNC(2), IOCON_MODE_NONE); /* SCK0 */
    IOCON_SET(IOCON_PIO0(8), IOCON_FUNC(1), IOCON_MODE_PULLUP); /* MISO0 */
    IOCON_SET(IOCON_PIO0(9), IOCON_FUNC(1), IOCON_MODE_NONE); /* MOSI0 */
    fast = 0;
    set_rate(meta_clock_mhz() * 1000000u, SD_SLOW_HZ);
    for (int i = 0; i < 10; i++) xfer(0xFF); /* >= 74 clocks, CS high */

    int ok = 0;
    uint32_t start = millis();
    cs_low();
    while (command(0, 0, 0x95) != 0x01) { /* GO_IDLE_STATE */
        if (millis() - start > 300) goto done;
    }
    {
        uint32_t hcs = 0;
        if (command(8, 0x1AA, 0x87) == 0x01) { /* SEND_IF_COND: SD v2 */
            uint8_t r[4];
            for (int i = 0; i < 4; i++) r[i] = xfer(0xFF);
            if (r[3] != 0xAA) goto done;
            hcs = 1ul << 30;
        }
        start = millis();
        for (;;) { /* APP_CMD + SD_SEND_OP_COND until the card leaves idle */
            command(55, 0, 0x65);
            if (command(41, hcs, 0x77) == 0x00) break;
            if (millis() - start > 1000) goto done;
        }
        block_addressing = 0;
        if (hcs && command(58, 0, 0xFD) == 0x00) { /* READ_OCR */
            uint8_t ocr0 = xfer(0xFF);
            for (int i = 0; i < 3; i++) xfer(0xFF);
            block_addressing = (ocr0 & 0x40) != 0; /* CCS */
        }
        ok = 1;
    }
done:
    cs_high();
    xfer(0xFF); /* release the card's data output */
    if (ok) {
        fast = 1;
        set_rate(meta_clock_mhz() * 1000000u, SD_FAST_HZ);
    }
    return ok;
}

int meta_sd_read(uint32_t lba, uint8_t *dst) {
    cs_low();
    int ok = 0;
    if (command(17, block_addressing ? lba : lba << 9, 0x01) == 0x00) {
        uint32_t start = millis();
        uint8_t token;
        while ((token = xfer(0xFF)) == 0xFF && millis() - start < 100) {}
        if (token == 0xFE) {
            for (int i = 0; i < 512; i++) dst[i] = xfer(0xFF);
            xfer(0xFF); /* CRC */
            xfer(0xFF);
            ok = 1;
        }
    }
    cs_high();
    xfer(0xFF);
    return ok;
}

int meta_sd_write(uint32_t lba, const uint8_t *src) {
    cs_low();
    int ok = 0;
    if (command(24, block_addressing ? lba : lba << 9, 0x01) == 0x00) {
        xfer(0xFF);
        xfer(0xFE); /* start block token */
        for (int i = 0; i < 512; i++) xfer(src[i]);
        xfer(0xFF); /* CRC (ignored in SPI mode) */
        xfer(0xFF);
        uint8_t resp = 0xFF;
        for (int i = 0; i < 8 && resp == 0xFF; i++) resp = xfer(0xFF);
        if ((resp & 0x1F) == 0x05) { /* data accepted: wait while busy */
            uint32_t start = millis();
            while (xfer(0xFF) != 0xFF && millis() - start < 500) {}
            ok = xfer(0xFF) == 0xFF;
        }
    }
    cs_high();
    xfer(0xFF);
    return ok;
}
