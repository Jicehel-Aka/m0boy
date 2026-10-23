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
#include "m0_target.h"

#include "m0_clock.h" /* m0_micros, META_BUTTONS_PL_US */
#include "m0_lcd.h"   /* m0_lcd_end_frame */

#define SPI SERCOM4->SPI

/* Pad chip select, PB03 (meta_hw.h). */
#define PAD_CS_MASK (1u << 3)

static uint8_t baud = 0xFF; /* not configured */

/* meta_usb.c, when the firmware has it: buttons the host presses (tests). */
extern volatile uint8_t meta_usb_buttons __attribute__((weak));
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
    /* Do not inherit the loader's PORT state. PA12 is the shared input from
     * both the SD card and the buttons; muxing alone does not explicitly
     * enable its digital input buffer. Never drive MISO or pull it down. */
    PORT->Group[0].DIRCLR.reg = 1u << 12;
    PORT->Group[0].PINCFG[12].reg = PORT_PINCFG_PMUXEN | PORT_PINCFG_INEN;
    /* All other bus devices must be deselected before the first clock.
     * In particular plain/packed Sega games do not open the SD driver. */
    PORT->Group[0].PINCFG[27].reg = 0; /* SD CS, GPIO */
    PORT->Group[0].OUTSET.reg = 1u << 27;
    PORT->Group[0].DIRSET.reg = 1u << 27;
    PORT->Group[1].PINCFG[22].reg = 0; /* LCD CS, GPIO */
    PORT->Group[1].OUTSET.reg = 1u << 22;
    PORT->Group[1].DIRSET.reg = 1u << 22;
    /* The pad's chip select is an output, inactive, from the start: its shift
     * register must not drive MISO while the display or the card is using the
     * bus (the Gamebuino library sets it the same way at boot). */
    PORT->Group[1].PINCFG[3].reg = 0; /* buttons CS, GPIO */
    PORT->Group[1].OUTSET.reg = PAD_CS_MASK;
    PORT->Group[1].DIRSET.reg = PAD_CS_MASK;
    baud = 0xFF;
    meta_spi_rate(4000000); /* the library's default until a device asks */
}

/* The pad's 74HC165 shift register: one byte per frame, active low, in the
 * Gamebuino library's bit order (0 down, 1 left, 2 right, 3 up, 4 A, 5 B,
 * 6 MENU, 7 HOME), read the library's way: CS low, about a microsecond for
 * the parallel load, one byte at META_BUTTONS_SPI_HZ (gbl_board.h).  Measured
 * on device (2026-10-16, RAM log read back through sam-ba): this read gives
 * all eight buttons in that order; restarting the receiver and pulsing CS
 * before it (0.10.12) shifted the byte by one bit and lost DOWN.  The frame
 * loops call it once the frame's strips are out (close_frame, meta_main.cpp);
 * from anywhere else it first waits for them. */
uint8_t meta_pad_read(void) {
    /* The display's strips (and its CS, low between strips of a frame) must
     * be done: the byte clocked out here would otherwise land in the panel.
     * Free when the frame loop already closed the frame. */
    m0_lcd_end_frame();
    meta_spi_rate(META_BUTTONS_SPI_HZ);
    /* The display's DMA completes on its write to DATA, before the wire is
     * idle: wait until the last byte has left the shifter, then drop what
     * display writes left in the receive buffer. */
#if M0_DEBUG_BUS
    m0_lcd_bus_check();
#endif
    for (int t = 0; t < 1000 && !(SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_TXC); t++) {}
    for (int i = 0; i < 4 && (SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); i++)
        (void)SPI.DATA.reg;
    SPI.STATUS.reg = SERCOM_SPI_STATUS_BUFOVF;
    PORT->Group[1].OUTCLR.reg = PAD_CS_MASK;
#if META_BUTTONS_PL_US > 0
    for (uint32_t t = m0_micros(); m0_micros() - t < META_BUTTONS_PL_US;) {}
#endif
    uint8_t raw = meta_spi_transfer(1);
    PORT->Group[1].OUTSET.reg = PAD_CS_MASK;
    uint8_t pressed = (uint8_t)~raw; /* active low */
    if (&meta_usb_buttons) pressed |= meta_usb_buttons;
    return pressed;
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
    /* Let the last byte leave the shifter: disabling the SERCOM cuts it (a
     * display strip ends when its DMA writes DATA, not when the byte is
     * out).  TXC only rises after a transmission: bounded wait. */
    if (SPI.CTRLA.bit.ENABLE)
        for (int t = 0; t < 4000 && !(SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_TXC); t++) {}
    /* Disable, reconfigure, enable: the Arduino core's SPI.beginTransaction
     * sequence.  A full SERCOM reset (SWRST) here froze the pad byte: the
     * released pads clock the 74HC165 while its parallel load is asserted
     * (the library and meta-lapinou never reset the SERCOM either).  Stale
     * receive bytes across the switch are fine: the pad read and the SD bus
     * both drain the FIFO after a rate change. */
    SPI.CTRLA.bit.ENABLE = 0;
    while (SPI.SYNCBUSY.bit.ENABLE) {}
    SPI.CTRLA.reg = SERCOM_SPI_CTRLA_MODE_SPI_MASTER | SERCOM_SPI_CTRLA_DOPO(1) | SERCOM_SPI_CTRLA_DIPO(0);
    SPI.CTRLB.reg = SERCOM_SPI_CTRLB_CHSIZE(0) | SERCOM_SPI_CTRLB_RXEN;
    while (SPI.SYNCBUSY.bit.CTRLB) {}
    SPI.BAUD.reg = b;
    SPI.CTRLA.bit.ENABLE = 1;
    while (SPI.SYNCBUSY.bit.ENABLE) {}
}

uint8_t meta_spi_transfer(uint8_t out) {
    SPI.DATA.reg = out;
    while (!(SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC)) {}
    return (uint8_t)SPI.DATA.reg;
}
