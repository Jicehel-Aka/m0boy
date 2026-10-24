/* PC storage: the SD card is the file sd.img in the working directory, a raw
 * 512-byte-sector image (dd + mkfs.fat).  m0_fat.c, m0_card.h, m0_state.cpp
 * and the save code read and write it through these sector calls, exactly as
 * they do a real card — which is why m0_sd.c (the SPI protocol) stays out of
 * the PC build. */
#include <stdio.h>
#include <stdint.h>

#include "m0_sd.h"

static FILE *image;
static uint32_t sectors;

int m0_sd_init(void) {
    image = fopen("sd.img", "r+b");
    if (!image) return 0;
    fseek(image, 0, SEEK_END);
    const long bytes = ftell(image);
    sectors = bytes > 0 ? (uint32_t)(bytes / 512) : 0;
    if (!sectors) {
        fclose(image);
        image = NULL;
    }
    return image != NULL;
}

uint32_t m0_sd_sectors(void) { return sectors; }

int m0_sd_read(uint32_t lba, uint8_t *dst) {
    if (!image || lba >= sectors) return 0;
    fseek(image, (long)lba * 512, SEEK_SET);
    return fread(dst, 1, 512, image) == 512;
}

int m0_sd_write(uint32_t lba, const uint8_t *src) {
    if (!image || lba >= sectors) return 0;
    fseek(image, (long)lba * 512, SEEK_SET);
    if (fwrite(src, 1, 512, image) != 512) return 0;
    fflush(image);
    return 1;
}

void m0_sd_deselect(void) {}
