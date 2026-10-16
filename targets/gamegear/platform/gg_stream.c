/* Files on the SD card: ROM pages left there (SG_ROM_STREAM projects; the
 * ROM file GG_STREAM_NAME, checked against its first sector kept in flash)
 * and the cartridge RAM's save file (GG_CART_SD; GG_SAVE_NAME, prepared by
 * compile.sh).  Each is located once through the FAT, in the card's root
 * directory or one directory below: in the folder of the game's own .BIN
 * (its first sector is the firmware in flash) when there is one, and the
 * ROM must also have this game's first sector; the save file is taken from
 * the ROM's folder.  The runtime's sector cache then reads
 * and rewrites sectors in place. */
#include "gg.h"
#include "sg_config.h"

#if GG_STREAM || GG_CART_ON_CARD
#include "meta_fat.h"
#include "meta_sd.h"
#include "meta_target.h"
#include <string.h>

#define MAX_EXTENTS 16
static int read_sector(uint32_t lba, uint8_t *buf) { return meta_sd_read(lba, buf); }

#if GG_CART_ON_CARD
static meta_extent save_extents[4];
static int n_save_extents, have_save;

int gg_platform_save_read(uint32_t sector, uint8_t *dst) {
    const uint32_t lba = have_save ? meta_fat_lba(save_extents, n_save_extents, sector) : 0;
    for (int attempt = 0; lba && attempt < 3; attempt++)
        if (meta_sd_read(lba, dst)) return 1;
    return 0;
}
int gg_platform_save_write(uint32_t sector, const uint8_t *src) {
    const uint32_t lba = have_save ? meta_fat_lba(save_extents, n_save_extents, sector) : 0;
    for (int attempt = 0; lba && attempt < 3; attempt++)
        if (meta_sd_write(lba, src)) return 1;
    return 0;
}
#endif

#if GG_STREAM && !defined(GG_ROM_PACKED)
static meta_extent extents[MAX_EXTENTS];
static int n_extents;
#endif

#if GG_STREAM && !defined(GG_ROM_PACKED)
/* Every game folder has its own GG_STREAM_NAME: take the one whose first
 * sector is this firmware's (the ROM's first 512 bytes are in flash). */
typedef struct {
    const uint8_t *rom;
    uint32_t rom_size;
    uint8_t seen; /* a file of that name exists, of another game */
} rom_match;

static int rom_matches(uint32_t first_lba, uint32_t size, uint8_t *buf, void *ctx) {
    rom_match *m = (rom_match *)ctx;
    m->seen = 1;
    return size >= m->rom_size && meta_sd_read(first_lba, buf) && !memcmp(buf, m->rom, 512);
}
#endif

/* 0 ready; 1 no card, 2 file not found, 3 fragmented, 4 another ROM.  A
 * missing save file only loses the saves; it is looked up in the ROM's
 * folder when the ROM is on the card. */
int gg_stream_open(const uint8_t *rom, uint32_t rom_size) {
#if defined(GG_ROM_PACKED) && !GG_CART_ON_CARD
    (void)rom; (void)rom_size;
    return 0;
#else
    if (!meta_sd_init()) return 1;
    uint8_t *buf = gg_stream_scratch();
    uint32_t dir = META_FAT_ANY_DIR;
#if !META_TARGET_POKITTO
    /* The game's files are the ones in the folder of its own .BIN: other
     * games' folders may hold files of the same name. */
    dir = meta_fat_game_dir(read_sector, buf, GG_BIN_NAME83, META_APP_START);
#endif
#if GG_STREAM && !defined(GG_ROM_PACKED)
    {
        uint32_t size = 0;
        rom_match m = {rom, rom_size, 0};
        const int r = meta_fat_find_in(read_sector, buf, GG_STREAM_NAME83, rom_matches, &m, &dir, extents,
                                       MAX_EXTENTS, &n_extents, &size);
        if (r == META_FAT_NOT_FOUND) return m.seen ? 4 : 2;
        if (r == META_FAT_FRAGMENTED) return 3;
        if (r) return 1;
    }
#else
    (void)rom; (void)rom_size;
#endif
#if GG_CART_ON_CARD
    uint32_t save_size = 0;
    have_save = meta_fat_find_in(read_sector, buf, GG_SAVE_NAME83, 0, 0, &dir, save_extents, 4, &n_save_extents,
                                 &save_size) == META_FAT_OK &&
                save_size >= GG_CART_RAM_SIZE;
#endif
    return 0;
#endif
}

#if GG_STREAM
int gg_platform_rom_read(uint32_t sector, uint8_t *dst) {
#ifdef GG_ROM_PACKED
    (void)sector; (void)dst;
    return 0;
#else
    const uint32_t lba = meta_fat_lba(extents, n_extents, sector);
    if (lba)
        for (int attempt = 0; attempt < 3; attempt++)
            if (meta_sd_read(lba, dst)) return 1;
    return 0;
#endif
}
#endif
#endif
