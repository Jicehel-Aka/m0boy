/* Files on the SD card: ROM pages left there (SG_ROM_STREAM projects; the
 * ROM file GG_STREAM_NAME, checked against its first sector kept in flash)
 * and the cartridge RAM's save file (GG_CART_SD; GG_SAVE_NAME, prepared by
 * compile.sh).  Each is located once through the FAT, in the card's root
 * directory or one directory below; the runtime's sector cache then reads
 * and rewrites sectors in place. */
#include "gg.h"
#include "sg_config.h"

#if GG_STREAM || GG_CART_ON_CARD
#include "meta_fat.h"
#include "meta_sd.h"
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

#if GG_STREAM
static meta_extent extents[MAX_EXTENTS];
static int n_extents;
#endif

/* 0 ready; 1 no card, 2 file not found, 3 fragmented, 4 another ROM.  A
 * missing save file only loses the saves. */
int gg_stream_open(const uint8_t *rom, uint32_t rom_size) {
    if (!meta_sd_init()) return 1;
    uint8_t *buf = gg_stream_scratch();
#if GG_CART_ON_CARD
    uint32_t save_size = 0;
    have_save = meta_fat_find(read_sector, buf, GG_SAVE_NAME83, save_extents, 4, &n_save_extents, &save_size) == META_FAT_OK &&
                save_size >= GG_CART_RAM_SIZE;
#endif
#if !GG_STREAM
    (void)rom; (void)rom_size;
    return 0;
#else
    uint32_t size = 0;
    const int r = meta_fat_find(read_sector, buf, GG_STREAM_NAME83, extents, MAX_EXTENTS, &n_extents, &size);
    if (r == META_FAT_NOT_FOUND) return 2;
    if (r == META_FAT_FRAGMENTED) return 3;
    if (r) return 1;
    if (size < rom_size) return 4;
    if (!meta_sd_read(meta_fat_lba(extents, n_extents, 0), buf)) return 1;
    return memcmp(buf, rom, 512) ? 4 : 0;
#endif
}

#if GG_STREAM
int gg_platform_rom_read(uint32_t sector, uint8_t *dst) {
    const uint32_t lba = meta_fat_lba(extents, n_extents, sector);
    if (lba)
        for (int attempt = 0; attempt < 3; attempt++)
            if (meta_sd_read(lba, dst)) return 1;
    return 0;
}
#endif
#endif
