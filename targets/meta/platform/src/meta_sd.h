/* SD card chip select and SPI-mode initialisation for the META loader. */
#ifndef META_SD_H
#define META_SD_H

#include <stdint.h>

void meta_sd_deselect(void); /* drive SD_CS (PA27) high */
int meta_sd_init(void);      /* card-level SD.begin; returns 1 when ready */
/* Read card sector lba (512 bytes) into dst; returns 1 on success. */
int meta_sd_read(uint32_t lba, uint8_t *dst);
/* Write card sector lba (512 bytes) from src; returns 1 once programmed. */
int meta_sd_write(uint32_t lba, const uint8_t *src);

#endif
