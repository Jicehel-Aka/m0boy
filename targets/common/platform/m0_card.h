/*
 * The game's files on the SD card, shared by the Game Boy frontend
 * (meta_stream.cpp, meta_save.cpp) and the Game Gear / Master System one
 * (gg_stream.c): each is located once through the FAT, in the folder of
 * the game's own .BIN, then its sectors are read and rewritten in place.
 * The same boot errors, and colours, on every console.
 */
#ifndef M0_CARD_H
#define M0_CARD_H

#include <stdint.h>

#include "m0_fat.h"
#include "m0_sd.h"

enum {
    M0_CARD_OK = 0,
    M0_CARD_NO_CARD,    /* card or FAT volume unreadable */
    M0_CARD_NOT_FOUND,  /* the ROM file is not in the root or a first-level folder */
    M0_CARD_FRAGMENTED, /* file in too many pieces: copy it again */
    M0_CARD_WRONG_ROM,  /* only another game's file of that name */
    M0_CARD_NO_SAVE,    /* the save file is missing (cartridge RAM on the card) */
};

/* Screen colour of each error (0xRRGGBB, for M0_COLOR): the ROM file's
 * as a table of colour(rgb), blue, red, orange, magenta; the save's
 * yellow. */
#define M0_CARD_COLOURS(colour) {0, colour(0x0000FF), colour(0xFF0000), colour(0xFFA000), colour(0xFF00FF)}
#define M0_CARD_RGB_NO_SAVE 0xFFFF00

/* Sector of a located file into dst / from src: three attempts, 0 when
 * the card refused all of them or the file is shorter. */
static inline int m0_card_read(const meta_extent *ext, int n_ext, uint32_t sector, uint8_t *dst) {
    uint32_t lba = m0_fat_lba(ext, n_ext, sector);
    if (lba)
        for (int attempt = 0; attempt < 3; attempt++)
            if (m0_sd_read(lba, dst)) return 1;
    return 0;
}

static inline int m0_card_write(const meta_extent *ext, int n_ext, uint32_t sector, const uint8_t *src) {
    uint32_t lba = m0_fat_lba(ext, n_ext, sector);
    if (lba)
        for (int attempt = 0; attempt < 3; attempt++)
            if (m0_sd_write(lba, src)) return 1;
    return 0;
}

static inline int m0_card_sector(uint32_t lba, uint8_t *buf) { return m0_sd_read(lba, buf); }

/* The ROM file name83 in *dir (M0_FAT_ANY_DIR: any), whose first sector
 * accept takes as this game's; *seen is set by accept when a file of that
 * name exists.  buf: 512 bytes of scratch.  A M0_CARD_ code. */
static inline int m0_card_find_rom(uint8_t *buf, const char *name83, m0_fat_accept_fn accept, void *ctx,
                                     const uint8_t *seen, uint32_t *dir, meta_extent *ext, int max_ext,
                                     int *n_ext) {
    uint32_t size = 0;
    int r = m0_fat_find_in(m0_card_sector, buf, name83, accept, ctx, dir, ext, max_ext, n_ext, &size);
    if (r == M0_FAT_NOT_FOUND) return *seen ? M0_CARD_WRONG_ROM : M0_CARD_NOT_FOUND;
    if (r == M0_FAT_FRAGMENTED) return M0_CARD_FRAGMENTED;
    if (r) return M0_CARD_NO_CARD;
    return M0_CARD_OK;
}

#endif
