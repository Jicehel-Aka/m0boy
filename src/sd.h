#ifndef SD_H
#define SD_H
#include <stdint.h>
int sd_init(void);
/* Pressed buttons, Gamebuino library order (bit 6 MENU, bit 7 HOME); the card stays deselected. */
uint8_t sd_pad(void);
uint32_t sd_sectors(void);
int sd_read(uint32_t lba, uint8_t *dst);
int sd_write(uint32_t lba, const uint8_t *src);
#endif
