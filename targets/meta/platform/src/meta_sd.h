/* SD card access (META: meta_sd.cpp, shared SPI bus; Pokitto: pokitto_sd.c, SSP0). */
#ifndef META_SD_H
#define META_SD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void meta_sd_deselect(void); /* chip select high (META: PA27, Pokitto: P0_7) */
int meta_sd_init(void);      /* card-level SD.begin; returns 1 when ready */
/* Read card sector lba (512 bytes) into dst; returns 1 on success. */
int meta_sd_read(uint32_t lba, uint8_t *dst);
/* Write card sector lba (512 bytes) from src; returns 1 once programmed. */
int meta_sd_write(uint32_t lba, const uint8_t *src);

#ifdef __cplusplus
}
#endif

#endif
