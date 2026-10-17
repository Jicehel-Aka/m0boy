/*
 * SD card access in SPI mode.
 *
 * The card protocol (meta_sd.c, shared) sits on a small bus layer each
 * console provides: META meta_sd_bus.cpp (SERCOM4, shared with the display
 * and the buttons), Pokitto pokitto_sd.c (SSP0).
 */
#ifndef META_SD_H
#define META_SD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the card (meta_sd.c) ------------------------------------------------ */
int meta_sd_init(void);      /* card-level SD.begin; returns 1 when ready */
/* Read card sector lba (512 bytes) into dst; returns 1 on success. */
int meta_sd_read(uint32_t lba, uint8_t *dst);
/* Write card sector lba (512 bytes) from src; returns 1 once programmed. */
int meta_sd_write(uint32_t lba, const uint8_t *src);

/* ---- the bus (one per console) ------------------------------------------- */
void meta_sd_deselect(void);       /* chip select high (META: PA27, Pokitto: P0_7) */
/* Take the bus for a card exchange: at the card's identification rate
 * (about 400 kHz) for init, else at the transfer rate. */
void meta_sd_bus_begin(int init);
/* Give the bus back; after init, ok tells whether the card answered. */
void meta_sd_bus_end(int init, int ok);
void meta_sd_cs(int selected);     /* chip select low (1) or high (0) */
uint8_t meta_sd_xfer(uint8_t out); /* one byte each way */
void meta_sd_recv(uint8_t *dst, unsigned n); /* n bytes in, 0xFF out */
/* After a 512-byte data block: 1 if crc (the block's CRC16, as sent) matches
 * what meta_sd_recv received, or if the bus does not check it. */
int meta_sd_block_ok(uint16_t crc);
/* A read failed: 1 if the bus lowered its rate (worth reading again). */
int meta_sd_slower(void);
uint32_t meta_sd_ms(void);         /* milliseconds, for timeouts */

#ifdef __cplusplus
}
#endif

#endif
