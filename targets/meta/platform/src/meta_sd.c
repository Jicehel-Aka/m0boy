/*
 * SD card protocol in SPI mode, shared by the consoles (the bus is theirs:
 * meta_sd.h).
 *
 * The games never read the card, but the META loader does: it expects the
 * card to have been put in SPI mode by the running program (the Gamebuino
 * library calls SD.begin at boot), and a card left uninitialised or with a
 * floating chip select during display traffic makes the loader crash.  This
 * performs the card-level part of SD.begin (no FAT layer, so no flash cost):
 * 74+ clocks with CS high, CMD0, CMD8, ACMD41 until ready, CMD58; then
 * single-sector reads (CMD17, ROM streaming, saves, states) and writes
 * (CMD24).
 */
#include "meta_sd.h"

static uint8_t block_addressing; /* SDHC/SDXC: sector numbers, else bytes */

static uint8_t command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    meta_sd_xfer(0xFF);
    meta_sd_xfer((uint8_t)(0x40 | cmd));
    meta_sd_xfer((uint8_t)(arg >> 24));
    meta_sd_xfer((uint8_t)(arg >> 16));
    meta_sd_xfer((uint8_t)(arg >> 8));
    meta_sd_xfer((uint8_t)arg);
    meta_sd_xfer(crc);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && (r & 0x80); i++) r = meta_sd_xfer(0xFF);
    return r;
}

int meta_sd_init(void) {
    meta_sd_deselect();
    meta_sd_bus_begin(1);
    for (int i = 0; i < 10; i++) meta_sd_xfer(0xFF); /* >= 74 clocks, CS high */

    int ok = 0;
    uint32_t start = meta_sd_ms();
    meta_sd_cs(1);
    while (command(0, 0, 0x95) != 0x01) { /* GO_IDLE_STATE */
        if (meta_sd_ms() - start > 300) goto done;
    }
    {
        uint32_t hcs = 0;
        if (command(8, 0x1AA, 0x87) == 0x01) { /* SEND_IF_COND: SD v2 */
            uint8_t r[4];
            for (int i = 0; i < 4; i++) r[i] = meta_sd_xfer(0xFF);
            if (r[3] != 0xAA) goto done;
            hcs = 1ul << 30;
        }
        start = meta_sd_ms();
        for (;;) { /* APP_CMD + SD_SEND_OP_COND until the card leaves idle */
            command(55, 0, 0x65);
            if (command(41, hcs, 0x77) == 0x00) break;
            if (meta_sd_ms() - start > 1000) goto done;
        }
        block_addressing = 0;
        if (hcs && command(58, 0, 0xFD) == 0x00) { /* READ_OCR */
            uint8_t ocr0 = meta_sd_xfer(0xFF);
            for (int i = 0; i < 3; i++) meta_sd_xfer(0xFF);
            block_addressing = (ocr0 & 0x40) != 0; /* CCS */
        }
        ok = 1;
    }
done:
    meta_sd_cs(0);
    meta_sd_xfer(0xFF); /* release the card's data output */
    meta_sd_bus_end(1, ok);
    return ok;
}

/* One 512-byte sector (CMD17). */
int meta_sd_read(uint32_t lba, uint8_t *dst) {
    meta_sd_bus_begin(0);
    meta_sd_cs(1);
    int ok = 0;
    if (command(17, block_addressing ? lba : lba << 9, 0x01) == 0x00) {
        uint32_t start = meta_sd_ms();
        uint8_t token;
        while ((token = meta_sd_xfer(0xFF)) == 0xFF && meta_sd_ms() - start < 100) {}
        if (token == 0xFE) {
            meta_sd_recv(dst, 512);
            meta_sd_xfer(0xFF); /* CRC */
            meta_sd_xfer(0xFF);
            ok = 1;
        }
    }
    meta_sd_cs(0);
    meta_sd_xfer(0xFF);
    meta_sd_bus_end(0, ok);
    return ok;
}

/* Write one 512-byte sector (CMD24), waiting until the card has programmed
 * it; returns 1 on success. */
int meta_sd_write(uint32_t lba, const uint8_t *src) {
    meta_sd_bus_begin(0);
    meta_sd_cs(1);
    int ok = 0;
    if (command(24, block_addressing ? lba : lba << 9, 0x01) == 0x00) {
        meta_sd_xfer(0xFF);
        meta_sd_xfer(0xFE); /* start block token */
        for (int i = 0; i < 512; i++) meta_sd_xfer(src[i]);
        meta_sd_xfer(0xFF); /* CRC (ignored in SPI mode) */
        meta_sd_xfer(0xFF);
        /* The data response token follows the CRC, possibly a byte or
         * more later (gamebuino-emulator answers one byte late). */
        uint8_t resp = 0xFF;
        for (int i = 0; i < 8 && resp == 0xFF; i++) resp = meta_sd_xfer(0xFF);
        if ((resp & 0x1F) == 0x05) { /* data accepted: wait while busy */
            uint32_t start = meta_sd_ms();
            while (meta_sd_xfer(0xFF) != 0xFF && meta_sd_ms() - start < 500) {}
            ok = meta_sd_xfer(0xFF) == 0xFF;
        }
    }
    meta_sd_cs(0);
    meta_sd_xfer(0xFF);
    meta_sd_bus_end(0, ok);
    return ok;
}
