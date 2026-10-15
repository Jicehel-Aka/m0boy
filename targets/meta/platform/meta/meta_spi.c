/*
 * SPI bus of the Gamebuino META: SERCOM4 as the Arduino core's SPI library
 * configured it (master, mode 0, MSB first, DOPO 1 = MOSI on PAD2 and SCK
 * on PAD3, MISO on PAD0), without the library.  The display, the SD card and
 * the pad share it, each with its own chip select; meta_pad_read() is the
 * pad's 74HC165, the other two are in meta_lcd.cpp and meta_sd_bus.cpp.
 *
 * The SERCOM's clock is generator 6 (48 MHz from the DFLL48M, or 72 or
 * 96 MHz from the PLL for the faster display rates, meta_clock.cpp); the
 * divider gives the fastest rate at or below the one asked: SPI clock /
 * (2 (BAUD + 1)).  The rates each device asks for are in gbl_board.h.  The
 * Arduino core capped every rate at 12 MHz: firmwares built with it ran the
 * display, the card and the buttons at 12 MHz (18 at 72 MHz) whatever they
 * asked for.
 */
#include "meta_hw.h"
#include "meta_target.h"

#include "meta_clock.h" /* meta_micros, META_BUTTONS_PL_US */

#define SPI SERCOM4->SPI

/* Pad chip select, PB03 (meta_hw.h). */
#define PAD_CS_MASK (1u << 3)

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
    /* The pad's chip select is an output, inactive, from the start: its shift
     * register must not drive MISO while the display or the card is using the
     * bus (the Gamebuino library sets it the same way at boot). */
    PORT->Group[1].DIRSET.reg = PAD_CS_MASK;
    PORT->Group[1].OUTSET.reg = PAD_CS_MASK;
    baud = 0xFF;
    meta_spi_rate(4000000); /* the library's default until a device asks */
}

/* The pad's 74HC165 shift register: one byte per frame, active low, in the
 * Gamebuino library's bit order at META_BUTTONS_SPI_HZ (gbl_board.h; at 24
 * MHz the bits come in another order and UP is lost).  Call it only when the
 * bus is free: the frame loops postpone it by one frame while a strip is in
 * flight (close_frame, meta_main.cpp). */
uint8_t meta_pad_read(void) {
    meta_spi_rate(META_BUTTONS_SPI_HZ);
    /* Drop bytes left by display writes (the receive FIFO holds two). */
    for (int i = 0; i < 2 && (SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); i++)
        (void)SPI.DATA.reg;
    SPI.STATUS.reg = SERCOM_SPI_STATUS_BUFOVF;
    PORT->Group[1].OUTCLR.reg = PAD_CS_MASK;
#if META_BUTTONS_PL_US > 0
    /* The library waits about a microsecond for the parallel load. */
    for (uint32_t t = meta_micros(); meta_micros() - t < META_BUTTONS_PL_US;) {}
#endif
    uint8_t raw = meta_spi_transfer(1);
    PORT->Group[1].OUTSET.reg = PAD_CS_MASK;
    return (uint8_t)~raw; /* active low */
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
