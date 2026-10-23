/* m0_cache.inc with a card that refuses writes, and a menu that borrows
 * every block: a refused write-back must not lose cartridge RAM, and the
 * lend of the whole pool must stay inside the arrays (run under ASan). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define M0C_BLOCK 64
#define M0C_SHIFT 6
#define M0C_WAYS 6
#define M0C_BUF t_buf
#define M0C_TAG t_tag
#define M0C_ROM_LAST t_rom_last
#define M0C_MISSES t_misses
#define M0C_SAVE 1
#define M0C_LEND 1
#define M0C_LEND_KEEP 0
#define NSEC 8

static uint8_t disk[NSEC][512];
static int refuse;
static uint8_t pending;
static int t_save_read(uint32_t s, uint8_t *dst) { assert(s < NSEC); memcpy(dst, disk[s], 512); return 1; }
static int t_save_write(uint32_t s, const uint8_t *src) {
    assert(s < NSEC);
    if (refuse) return 0;
    memcpy(disk[s], src, 512);
    return 1;
}
static int t_rom_sector(uint32_t s, uint8_t *dst) { memset(dst, (int)s, 512); return 1; }
#define M0C_ROM_SECTOR(sec, dst) t_rom_sector((sec), (dst))
#define M0C_SAVE_READ(sec, dst) t_save_read((sec), (dst))
#define M0C_SAVE_WRITE(sec, src) t_save_write((sec), (src))
#define M0C_PENDING pending
#define M0C_NOTE_CHANGE() ((void)0)

#include "m0_cache.h"
#include "m0_cache.inc"

/* A card that refuses: three blocks changed, many ROM blocks read meanwhile. */
static void refused_write_back(void) {
    memset(disk, 0, sizeof disk);
    cache_reset();
    refuse = 1;
    cache_ram_store(0, 0x42);
    cache_ram_store(64, 0x43);
    for (uint32_t b = 0; b < 40; b++) assert(cache_rom_byte(1024 + b * 64) == (uint8_t)((1024 + b * 64) >> 9));
    refuse = 0;
    assert(cache_ram_load(0) == 0x42 && cache_ram_load(64) == 0x43);
    cache_flush();
    assert(disk[0][0] == 0x42 && disk[0][64] == 0x43);
}

/* The whole pool lent while a write-back is refused: the changes stay. */
static void lend_all(int fail) {
    memset(disk, 0, sizeof disk);
    cache_reset();
    for (uint32_t b = 0; b < 12; b++) (void)cache_rom_byte(2048 + b * 64);
    refuse = fail;
    cache_ram_store(128, 0x77);
    uintptr_t top = cache_lend();
    assert(top >= (uintptr_t)t_buf[0] && top <= (uintptr_t)t_buf[0] + sizeof t_buf);
    memset(t_buf, 0x5A, top - (uintptr_t)t_buf); /* the menu's stack */
    cache_unlend();
    refuse = 0;
    for (uint32_t b = 0; b < 12; b++) assert(cache_rom_byte(2048 + b * 64) == (uint8_t)((2048 + b * 64) >> 9));
    assert(cache_ram_load(128) == 0x77);
    cache_flush();
    assert(disk[0][128] == 0x77);
}

int main(void) {
    refused_write_back();
    lend_all(0);
    lend_all(1);
    puts("cache: refused write-back and whole-pool lend ok");
    return 0;
}
