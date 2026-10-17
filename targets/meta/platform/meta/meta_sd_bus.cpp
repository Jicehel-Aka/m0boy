/*
 * SD card bus of the Gamebuino META (meta_sd.h): SERCOM4 (meta_spi.c),
 * shared with the display and the buttons; chip select PA27.  The card protocol is meta_sd.c.
 *
 * A card exchange first drains the display's DMA queue and releases the
 * bus (the next strip reopens it), and drops what display writes left in
 * the receive FIFO.  Identification runs at 250 kHz, transfers at
 * META_SD_SPI_HZ.
 */
#include "meta_sd.h"
#include "meta_clock.h"
#include "meta_target.h"

#include "meta_hw.h"

#include "meta_lcd.h"

#define SD_CS_MASK (1u << 27) /* PA27, Arduino pin 26 */
/* Sector reads (ROM streaming) at META_SD_SPI_HZ (gbl_board.h), halved
 * down to 12 MHz after each failed read (bad CRC, no data token): a card
 * or a console that does not keep up falls back by itself.  Global, so
 * that meta-usb.py can read the errors and set a rate while a game runs. */
#define SD_SAFE_HZ 12000000u
extern "C" {
uint32_t meta_sd_spi_hz = META_SD_SPI_HZ;
uint32_t meta_sd_read_errors;
}
static uint16_t recv_crc;

/* CRC16-CCITT (polynomial 0x1021, initial 0), the SD data block's. */
static constexpr unsigned crc_shift(unsigned c, int k) { /* C++11: one return */
    return k ? crc_shift(c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1, k - 1) : c & 0xFFFFu;
}
#define CRC_BYTE(i) (uint16_t)crc_shift((unsigned)(i) << 8, 8)
#define CRC_4(i) CRC_BYTE(i), CRC_BYTE((i) + 1), CRC_BYTE((i) + 2), CRC_BYTE((i) + 3)
#define CRC_16(i) CRC_4(i), CRC_4((i) + 4), CRC_4((i) + 8), CRC_4((i) + 12)
#define CRC_64(i) CRC_16(i), CRC_16((i) + 16), CRC_16((i) + 32), CRC_16((i) + 48)
static const uint16_t crc16_table[256] = {CRC_64(0), CRC_64(64), CRC_64(128), CRC_64(192)};

void meta_sd_deselect(void) {
    PORT->Group[0].DIRSET.reg = SD_CS_MASK;
    PORT->Group[0].OUTSET.reg = SD_CS_MASK;
}

void meta_sd_cs(int selected) {
    if (selected) PORT->Group[0].OUTCLR.reg = SD_CS_MASK;
    else PORT->Group[0].OUTSET.reg = SD_CS_MASK;
}

void meta_sd_bus_begin(int init) {
    meta_watchdog_feed(); /* long sector runs (state saves, the FAT walk) */
    if (init) {
        meta_spi_rate(250000);
        return;
    }
    meta_lcd_end_frame();
    meta_spi_rate(meta_sd_spi_hz);
    /* Drop bytes left in the receive FIFO by display writes. */
    for (int i = 0; i < 4 && (SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); i++)
        (void)SERCOM4->SPI.DATA.reg;
    SERCOM4->SPI.STATUS.reg = SERCOM_SPI_STATUS_BUFOVF;
}

void meta_sd_bus_end(int init, int ok) {
    (void)init;
    (void)ok;
}

int meta_sd_block_ok(uint16_t crc) { return crc == recv_crc; }

int meta_sd_slower(void) {
    meta_sd_read_errors++;
    if (meta_sd_spi_hz <= SD_SAFE_HZ) return 0;
    meta_sd_spi_hz = meta_sd_spi_hz / 2u < SD_SAFE_HZ ? SD_SAFE_HZ : meta_sd_spi_hz / 2u;
    return 1;
}

uint8_t meta_sd_xfer(uint8_t out) { return meta_spi_transfer(out); }

/* Pipelined: the next 0xFF goes into DATA as soon as DRE says the shifter
 * took the previous one, so the bus never idles between bytes (one byte in
 * the shifter, one waiting; the two-level receive buffer cannot overflow).
 * The byte-by-byte loop left a gap of several cycles per byte.  The CRC of
 * the bytes received is computed while the next one shifts in. */
void meta_sd_recv(uint8_t *dst, unsigned n) {
    unsigned crc = 0;
    if (!n) return;
    SercomSpi *spi = &SERCOM4->SPI;
    spi->DATA.reg = 0xFF;
    while (--n) {
        while (!(spi->INTFLAG.reg & SERCOM_SPI_INTFLAG_DRE)) {}
        spi->DATA.reg = 0xFF;
        while (!(spi->INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC)) {}
        uint8_t b = (uint8_t)spi->DATA.reg;
        *dst++ = b;
        crc = ((crc << 8) ^ crc16_table[((crc >> 8) ^ b) & 255u]) & 0xFFFFu;
    }
    while (!(spi->INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC)) {}
    uint8_t b = (uint8_t)spi->DATA.reg;
    *dst = b;
    recv_crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ b) & 255u]);
}

uint32_t meta_sd_ms(void) { return millis(); }
