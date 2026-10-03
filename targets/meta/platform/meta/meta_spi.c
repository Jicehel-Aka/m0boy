/*
 * SPI bus of the Gamebuino META: SERCOM4 as the Arduino core's SPI library
 * configured it (master, mode 0, MSB first, DOPO 1 = MOSI on PAD2 and SCK
 * on PAD3, MISO on PAD0), without the library.
 *
 * The SERCOM's clock is generator 6 (48 MHz from the DFLL48M, or 72 or
 * 96 MHz from the PLL for the faster display rates, meta_clock.cpp); the
 * divider gives the fastest rate at or below the one asked: SPI clock /
 * (2 (BAUD + 1)).  The Arduino core capped every rate at 12 MHz: firmwares
 * built with it ran the display, the card and the buttons at 12 MHz (18 at
 * 72 MHz) whatever they asked for.
 */
#include "meta_hw.h"
#include "meta_target.h"

#define SPI SERCOM4->SPI

static uint8_t baud = 0xFF; /* not configured */
static uint32_t sercom_hz = 48000000u;

static void pin_function_d(unsigned group, unsigned pin) {
    PortGroup *g = &PORT->Group[group];
    if (pin & 1u) g->PMUX[pin >> 1].bit.PMUXO = PORT_PMUX_PMUXO_D_Val;
    else g->PMUX[pin >> 1].bit.PMUXE = PORT_PMUX_PMUXE_D_Val;
    g->PINCFG[pin].reg |= PORT_PINCFG_PMUXEN;
}

void meta_spi_init(void) {
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM4;
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(6) | GCLK_GENDIV_DIV(1);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(6) | GCLK_GENCTRL_SRC_DFLL48M | GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_SERCOM4_CORE_Val) |
                                   GCLK_CLKCTRL_GEN_GCLK6 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    sercom_hz = 48000000u;
    pin_function_d(0, 12); /* MISO, PAD0 */
    pin_function_d(1, 10); /* MOSI, PAD2 */
    pin_function_d(1, 11); /* SCK, PAD3 */
    baud = 0xFF;
    meta_spi_rate(4000000); /* the library's default until a device asks */
}

void meta_spi_clock(uint32_t hz) {
    sercom_hz = hz;
    baud = 0xFF; /* the next meta_spi_rate recomputes */
}

void meta_spi_rate(uint32_t hz) {
    uint32_t n = (sercom_hz + 2u * hz - 1u) / (2u * hz); /* BAUD + 1, rounded up */
    uint8_t b = (uint8_t)(n > 256u ? 255u : n - 1u);
    if (b == baud) return;
    baud = b;
    SPI.CTRLA.bit.ENABLE = 0;
    while (SPI.SYNCBUSY.bit.ENABLE) {}
    SPI.CTRLA.bit.SWRST = 1;
    while (SPI.CTRLA.bit.SWRST || SPI.SYNCBUSY.bit.SWRST) {}
    SPI.CTRLA.reg = SERCOM_SPI_CTRLA_MODE_SPI_MASTER | SERCOM_SPI_CTRLA_DOPO(1) | SERCOM_SPI_CTRLA_DIPO(0);
    SPI.CTRLB.reg = SERCOM_SPI_CTRLB_CHSIZE(0) | SERCOM_SPI_CTRLB_RXEN;
    SPI.BAUD.reg = b;
    SPI.CTRLA.bit.ENABLE = 1;
    while (SPI.SYNCBUSY.bit.ENABLE) {}
}

uint8_t meta_spi_transfer(uint8_t out) {
    SPI.DATA.reg = out;
    while (!(SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC)) {}
    return (uint8_t)SPI.DATA.reg;
}
