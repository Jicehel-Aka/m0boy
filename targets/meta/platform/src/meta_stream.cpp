/*
 * ROM streaming for the Gamebuino META (projects generated with
 * GBL_ROM_STREAM, i.e. ROMs too large for the flash).
 *
 * At boot the ROM file (GBL_STREAM_NAME, in the card's root directory or one
 * directory below) is located once through the FAT and checked against the
 * header of bank 0 in flash; from then on the runtime's sector cache calls
 * gbl_platform_rom_read, which is a direct card read.
 */
#include "meta_stream.h"

extern "C" {
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_options.h"
}

uint32_t meta_stream_errors; /* sectors that could not be read */

#if GBL_ROM_SD

#include <string.h>

#include "meta_fat.h"
#include "meta_sd.h"

#define MAX_EXTENTS 16

static meta_extent extents[MAX_EXTENTS];
static int n_extents;

static int read_sector(uint32_t lba, uint8_t *buf) { return meta_sd_read(lba, buf); }

int meta_stream_init(int card_ok) {
    if (!card_ok) return META_STREAM_NO_CARD;
    uint8_t *buf = gbl_stream_scratch();
    uint32_t size = 0;
    int r = meta_fat_find(read_sector, buf, GBL_STREAM_NAME83, extents, MAX_EXTENTS, &n_extents, &size);
    if (r == META_FAT_NOT_FOUND) return META_STREAM_NOT_FOUND;
    if (r == META_FAT_FRAGMENTED) return META_STREAM_FRAGMENTED;
    if (r) return META_STREAM_NO_CARD;
    if (size < gbl_rom_size) return META_STREAM_WRONG_ROM;
    /* Same game: the cartridge header (title .. checksums) must match. */
    if (!meta_sd_read(meta_fat_lba(extents, n_extents, 0), buf)) return META_STREAM_NO_CARD;
    if (memcmp(buf + 0x134, gbl_rom + 0x134, 0x150 - 0x134)) return META_STREAM_WRONG_ROM;
    return META_STREAM_OK;
}

extern "C" int gbl_platform_rom_read(uint32_t sector, uint8_t *dst) {
    uint32_t lba = meta_fat_lba(extents, n_extents, sector);
    if (lba)
        for (int attempt = 0; attempt < 3; attempt++)
            if (meta_sd_read(lba, dst)) return 1;
    meta_stream_errors++;
    return 0;
}

#else

int meta_stream_init(int card_ok) {
    (void)card_ok;
    return META_STREAM_OK;
}

#endif
