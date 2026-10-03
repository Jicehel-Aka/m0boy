/*
 * SD card bus of the Pokitto (meta_sd.h): SSP0 with PokittoLib's wiring
 * (MOSI P0_9, MISO P0_8, SCK P0_6, chip select P0_7 as GPIO), SPI mode 0,
 * 8-bit frames.  The card protocol is meta_sd.c.  Identification runs at
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

static uint8_t fast;             /* initialised: transfer rate */


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

uint8_t meta_sd_xfer(uint8_t b) {
    while (!(SSP0_SR & SSP_SR_TNF)) {}
    SSP0_DR = b;
    while (!(SSP0_SR & SSP_SR_RNE)) {}
    return (uint8_t)SSP0_DR;
}

void meta_sd_deselect(void) {
    IOCON_SET(IOCON_PIO0(7), IOCON_FUNC(0), IOCON_MODE_PULLUP);
    GPIO_DIR(0) |= CS_BIT;
    GPIO_SET(0) = CS_BIT;
}

void meta_sd_cs(int selected) {
    if (selected) GPIO_CLR(0) = CS_BIT;
    else GPIO_SET(0) = CS_BIT;
}

void meta_sd_bus_begin(int init) {
    if (!init) return; /* the bus is the card's alone */
    SYSCON_SYSAHBCLKCTRL |= AHB_SSP0;
    SYSCON_PRESETCTRL |= PRESET_SSP0;
    SYSCON_SSP0CLKDIV = 1;
    IOCON_SET(IOCON_PIO0(6), IOCON_FUNC(2), IOCON_MODE_NONE); /* SCK0 */
    IOCON_SET(IOCON_PIO0(8), IOCON_FUNC(1), IOCON_MODE_PULLUP); /* MISO0 */
    IOCON_SET(IOCON_PIO0(9), IOCON_FUNC(1), IOCON_MODE_NONE); /* MOSI0 */
    fast = 0;
    set_rate(meta_clock_mhz() * 1000000u, SD_SLOW_HZ);
}

void meta_sd_bus_end(int init, int ok) {
    if (init && ok) {
        fast = 1;
        set_rate(meta_clock_mhz() * 1000000u, SD_FAST_HZ);
    }
}

void meta_sd_recv(uint8_t *dst, unsigned n) {
    for (unsigned i = 0; i < n; i++) dst[i] = meta_sd_xfer(0xFF);
}

uint32_t meta_sd_ms(void) { return millis(); }

