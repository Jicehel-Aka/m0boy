/*
 * SPI bus of the Gamebuino META: SERCOM4 as the Arduino core's SPI library
 * configured it (master, mode 0, MSB first, DOPO 1 = MOSI on PAD2 and SCK
 * on PAD3, MISO on PAD0), without the library.
 *
 * The divider follows the core's: BAUD = 24 MHz / rate - 1, the rate capped
 * at META_SPI_MAX_HZ (gbl_profile.h, 24 MHz).  The core capped it at
 * 12 MHz: firmwares built with it ran the display, the card and the
 * buttons at 12 MHz (18 at 72 MHz) whatever they asked for.  The SERCOM's
 * clock is generator 0, which meta_clock.cpp moves to the PLL at 72 MHz:
 * meta_spi_hz() rescales the requested rates.
 */
#include "meta_hw.h"
#include "meta_target.h"

#define SPI SERCOM4->SPI

static uint8_t baud = 0xFF; /* not configured */

static void pin_function_d(unsigned group, unsigned pin) {
    PortGroup *g = &PORT->Group[group];
    if (pin & 1u) g->PMUX[pin >> 1].bit.PMUXO = PORT_PMUX_PMUXO_D_Val;
    else g->PMUX[pin >> 1].bit.PMUXE = PORT_PMUX_PMUXE_D_Val;
    g->PINCFG[pin].reg |= PORT_PINCFG_PMUXEN;
}

void meta_spi_init(void) {
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM4;
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_SERCOM4_CORE_Val) |
                                   GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    pin_function_d(0, 12); /* MISO, PAD0 */
    pin_function_d(1, 10); /* MOSI, PAD2 */
    pin_function_d(1, 11); /* SCK, PAD3 */
    baud = 0xFF;
    meta_spi_rate(4000000); /* the library's default until a device asks */
}

void meta_spi_rate(uint32_t hz) {
    if (hz > META_SPI_MAX_HZ) hz = META_SPI_MAX_HZ;
    uint8_t b = (uint8_t)(24000000u / hz - 1u);
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
