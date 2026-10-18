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
#include "meta_card.h"
#include "meta_target.h"
#include <string.h>

#define MAX_EXTENTS 16

#if GG_CART_ON_CARD
static meta_extent save_extents[4];
static int n_save_extents, have_save;

int gg_platform_save_read(uint32_t sector, uint8_t *dst) {
    return have_save && meta_card_read(save_extents, n_save_extents, sector, dst);
}
int gg_platform_save_write(uint32_t sector, const uint8_t *src) {
    return have_save && meta_card_write(save_extents, n_save_extents, sector, src);
}
#endif

#if GG_STREAM && !defined(GG_ROM_PACKED)
static meta_extent extents[MAX_EXTENTS];
static int n_extents;

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

/* A META_CARD_ code (meta_card.h): the ROM file when it is on the card,
 * then the save file, looked up in the folder of the game's .BIN. */
int gg_stream_open(const uint8_t *rom, uint32_t rom_size) {
#if defined(GG_ROM_PACKED) && !GG_CART_ON_CARD
    (void)rom; (void)rom_size;
    return META_CARD_OK;
#else
    if (!meta_sd_init()) return META_CARD_NO_CARD;
    uint8_t *buf = gg_stream_scratch();
    uint32_t dir = META_FAT_ANY_DIR;
#if !META_TARGET_POKITTO
    /* The game's files are the ones in the folder of its own .BIN: other
     * games' folders may hold files of the same name. */
    dir = meta_fat_game_dir(meta_card_sector, buf, GG_BIN_NAME83, META_APP_START);
#endif
#if GG_STREAM && !defined(GG_ROM_PACKED)
    {
        rom_match m = {rom, rom_size, 0};
        const int r = meta_card_find_rom(buf, GG_STREAM_NAME83, rom_matches, &m, &m.seen, &dir, extents,
                                         MAX_EXTENTS, &n_extents);
        if (r) return r;
    }
#else
    (void)rom; (void)rom_size;
#endif
#if GG_CART_ON_CARD
    /* The cartridge RAM lives there: without it the game would lose its
     * own variables, as on the Game Boy. */
    uint32_t save_size = 0;
    have_save = meta_fat_find_in(meta_card_sector, buf, GG_SAVE_NAME83, 0, 0, &dir, save_extents, 4,
                                 &n_save_extents, &save_size) == META_FAT_OK &&
                save_size >= GG_CART_RAM_SIZE;
    if (!have_save) return META_CARD_NO_SAVE;
#endif
    return META_CARD_OK;
#endif
}

#if GG_STREAM
int gg_platform_rom_read(uint32_t sector, uint8_t *dst) {
#ifdef GG_ROM_PACKED
    (void)sector; (void)dst;
    return 0;
#else
    return meta_card_read(extents, n_extents, sector, dst);
#endif
}
#endif
#endif
