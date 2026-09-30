/*
 * SD card bring-up for the Gamebuino META.
 *
 * The games never read the card, but the META loader does: it expects the
 * card to have been put in SPI mode by the running program (the Gamebuino
 * library calls SD.begin at boot), and a card left uninitialised or with a
 * floating chip select during display traffic makes the loader crash.  This
 * performs the card-level part of SD.begin (no FAT layer, so no flash cost):
 * 74+ clocks with CS high, CMD0, CMD8, ACMD41 until ready, CMD58.
 */
#include "meta_sd.h"

#include <Arduino.h>
#include <SPI.h>

#include "meta_lcd.h"

#define SD_CS_MASK (1u << 27) /* PA27, Arduino pin 26 */
/* Sector reads (ROM streaming).  SD cards accept 25 MHz in SPI mode; the
 * SERCOM tops out at 24 MHz. */
#ifndef META_SD_SPI_HZ
#define META_SD_SPI_HZ 24000000
#endif

static uint8_t block_addressing; /* SDHC/SDXC: sector numbers, else bytes */

static inline void cs_high(void) { PORT->Group[0].OUTSET.reg = SD_CS_MASK; }
static inline void cs_low(void) { PORT->Group[0].OUTCLR.reg = SD_CS_MASK; }

void meta_sd_deselect(void) {
    PORT->Group[0].DIRSET.reg = SD_CS_MASK;
    cs_high();
}

static uint8_t command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    SPI.transfer(0xFF);
    SPI.transfer((uint8_t)(0x40 | cmd));
    SPI.transfer((uint8_t)(arg >> 24));
    SPI.transfer((uint8_t)(arg >> 16));
    SPI.transfer((uint8_t)(arg >> 8));
    SPI.transfer((uint8_t)arg);
    SPI.transfer(crc);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && (r & 0x80); i++) r = SPI.transfer(0xFF);
    return r;
}

int meta_sd_init(void) {
    meta_sd_deselect();
    SPI.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; i++) SPI.transfer(0xFF); /* >= 74 clocks, CS high */

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
            for (int i = 0; i < 4; i++) r[i] = SPI.transfer(0xFF);
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
            uint8_t ocr0 = SPI.transfer(0xFF);
            for (int i = 0; i < 3; i++) SPI.transfer(0xFF);
            block_addressing = (ocr0 & 0x40) != 0; /* CCS */
        }
        ok = 1;
    }
done:
    cs_high();
    SPI.transfer(0xFF); /* release the card's data output */
    SPI.endTransaction();
    return ok;
}

/* One 512-byte sector (CMD17).  The display shares the bus: its DMA queue is
 * drained and the bus released first; the next strip reopens it. */
int meta_sd_read(uint32_t lba, uint8_t *dst) {
    meta_lcd_end_frame();
    SPI.beginTransaction(SPISettings(META_SD_SPI_HZ, MSBFIRST, SPI_MODE0));
    /* Drop bytes left in the receive FIFO by display writes. */
    for (int i = 0; i < 2 && (SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); i++)
        (void)SERCOM4->SPI.DATA.reg;
    SERCOM4->SPI.STATUS.reg = SERCOM_SPI_STATUS_BUFOVF;
    cs_low();
    int ok = 0;
    if (command(17, block_addressing ? lba : lba << 9, 0x01) == 0x00) {
        uint32_t start = millis();
        uint8_t token;
        while ((token = SPI.transfer(0xFF)) == 0xFF && millis() - start < 100) {}
        if (token == 0xFE) {
            for (int i = 0; i < 512; i++) {
                SERCOM4->SPI.DATA.reg = 0xFF;
                while (!(SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC)) {}
                dst[i] = (uint8_t)SERCOM4->SPI.DATA.reg;
            }
            SPI.transfer(0xFF); /* CRC */
            SPI.transfer(0xFF);
            ok = 1;
        }
    }
    cs_high();
    SPI.transfer(0xFF);
    SPI.endTransaction();
    return ok;
}

/* Write one 512-byte sector (CMD24), waiting until the card has programmed
 * it; returns 1 on success.  Same bus sharing as meta_sd_read. */
int meta_sd_write(uint32_t lba, const uint8_t *src) {
    meta_lcd_end_frame();
    SPI.beginTransaction(SPISettings(META_SD_SPI_HZ, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 2 && (SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); i++)
        (void)SERCOM4->SPI.DATA.reg;
    SERCOM4->SPI.STATUS.reg = SERCOM_SPI_STATUS_BUFOVF;
    cs_low();
    int ok = 0;
    if (command(24, block_addressing ? lba : lba << 9, 0x01) == 0x00) {
        SPI.transfer(0xFF);
        SPI.transfer(0xFE); /* start block token */
        for (int i = 0; i < 512; i++) SPI.transfer(src[i]);
        SPI.transfer(0xFF); /* CRC (ignored in SPI mode) */
        SPI.transfer(0xFF);
        uint8_t resp = SPI.transfer(0xFF);
        if ((resp & 0x1F) == 0x05) { /* data accepted: wait while busy */
            uint32_t start = millis();
            while (SPI.transfer(0xFF) != 0xFF && millis() - start < 500) {}
            ok = SPI.transfer(0xFF) == 0xFF;
        }
    }
    cs_high();
    SPI.transfer(0xFF);
    SPI.endTransaction();
    return ok;
}
