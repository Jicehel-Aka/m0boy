/*
 * The game's files on the SD card, shared by the Game Boy frontend
 * (meta_stream.cpp, meta_save.cpp) and the Game Gear / Master System one
 * (gg_stream.c): each is located once through the FAT, in the folder of
 * the game's own .BIN, then its sectors are read and rewritten in place.
 * The same boot errors, and colours, on every console.
 */
#ifndef META_CARD_H
#define META_CARD_H

#include <stdint.h>

#include "meta_fat.h"
#include "meta_sd.h"

enum {
    META_CARD_OK = 0,
    META_CARD_NO_CARD,    /* card or FAT volume unreadable */
    META_CARD_NOT_FOUND,  /* the ROM file is not in the root or a first-level folder */
    META_CARD_FRAGMENTED, /* file in too many pieces: copy it again */
    META_CARD_WRONG_ROM,  /* only another game's file of that name */
    META_CARD_NO_SAVE,    /* the save file is missing (cartridge RAM on the card) */
};

/* Screen colour of each error (0xRRGGBB, for META_COLOR): the ROM file's
 * as a table of colour(rgb), blue, red, orange, magenta; the save's
 * yellow. */
#define META_CARD_COLOURS(colour) {0, colour(0x0000FF), colour(0xFF0000), colour(0xFFA000), colour(0xFF00FF)}
#define META_CARD_RGB_NO_SAVE 0xFFFF00

/* Sector of a located file into dst / from src: three attempts, 0 when
 * the card refused all of them or the file is shorter. */
static inline int meta_card_read(const meta_extent *ext, int n_ext, uint32_t sector, uint8_t *dst) {
    uint32_t lba = meta_fat_lba(ext, n_ext, sector);
    if (lba)
        for (int attempt = 0; attempt < 3; attempt++)
            if (meta_sd_read(lba, dst)) return 1;
    return 0;
}

static inline int meta_card_write(const meta_extent *ext, int n_ext, uint32_t sector, const uint8_t *src) {
    uint32_t lba = meta_fat_lba(ext, n_ext, sector);
    if (lba)
        for (int attempt = 0; attempt < 3; attempt++)
            if (meta_sd_write(lba, src)) return 1;
    return 0;
}

static inline int meta_card_sector(uint32_t lba, uint8_t *buf) { return meta_sd_read(lba, buf); }

/* The ROM file name83 in *dir (META_FAT_ANY_DIR: any), whose first sector
 * accept takes as this game's; *seen is set by accept when a file of that
 * name exists.  buf: 512 bytes of scratch.  A META_CARD_ code. */
static inline int meta_card_find_rom(uint8_t *buf, const char *name83, meta_fat_accept_fn accept, void *ctx,
                                     const uint8_t *seen, uint32_t *dir, meta_extent *ext, int max_ext,
                                     int *n_ext) {
    uint32_t size = 0;
    int r = meta_fat_find_in(meta_card_sector, buf, name83, accept, ctx, dir, ext, max_ext, n_ext, &size);
    if (r == META_FAT_NOT_FOUND) return *seen ? META_CARD_WRONG_ROM : META_CARD_NOT_FOUND;
    if (r == META_FAT_FRAGMENTED) return META_CARD_FRAGMENTED;
    if (r) return META_CARD_NO_CARD;
    return META_CARD_OK;
}

#endif
