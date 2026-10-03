/*
 * SD card bus of the Gamebuino META (meta_sd.h): SERCOM4 through the
 * Arduino SPI library, shared with the display and the buttons; chip
 * select PA27.  The card protocol is meta_sd.c.
 *
 * A card exchange first drains the display's DMA queue and releases the
 * bus (the next strip reopens it), and drops what display writes left in
 * the receive FIFO.  Identification runs at 250 kHz, transfers at
 * META_SD_SPI_HZ.
 */
#include "meta_sd.h"
#include "meta_clock.h"

#include <Arduino.h>
#include <SPI.h>

#include "meta_lcd.h"

#define SD_CS_MASK (1u << 27) /* PA27, Arduino pin 26 */
/* Sector reads (ROM streaming).  SD cards accept 25 MHz in SPI mode; the
 * SERCOM tops out at 24 MHz. */
#ifndef META_SD_SPI_HZ
#define META_SD_SPI_HZ 24000000
#endif

void meta_sd_deselect(void) {
    PORT->Group[0].DIRSET.reg = SD_CS_MASK;
    PORT->Group[0].OUTSET.reg = SD_CS_MASK;
}

void meta_sd_cs(int selected) {
    if (selected) PORT->Group[0].OUTCLR.reg = SD_CS_MASK;
    else PORT->Group[0].OUTSET.reg = SD_CS_MASK;
}

void meta_sd_bus_begin(int init) {
    if (init) {
        SPI.beginTransaction(SPISettings(meta_spi_hz(250000), MSBFIRST, SPI_MODE0));
        return;
    }
    meta_lcd_end_frame();
    SPI.beginTransaction(SPISettings(meta_spi_hz(META_SD_SPI_HZ), MSBFIRST, SPI_MODE0));
    /* Drop bytes left in the receive FIFO by display writes. */
    for (int i = 0; i < 2 && (SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); i++)
        (void)SERCOM4->SPI.DATA.reg;
    SERCOM4->SPI.STATUS.reg = SERCOM_SPI_STATUS_BUFOVF;
}

void meta_sd_bus_end(int init, int ok) {
    (void)init;
    (void)ok;
    SPI.endTransaction();
}

uint8_t meta_sd_xfer(uint8_t out) { return SPI.transfer(out); }

void meta_sd_recv(uint8_t *dst, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        SERCOM4->SPI.DATA.reg = 0xFF;
        while (!(SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC)) {}
        dst[i] = (uint8_t)SERCOM4->SPI.DATA.reg;
    }
}

uint32_t meta_sd_ms(void) { return millis(); }
