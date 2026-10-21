/* GBL_CACHE_UNIFIED: streamed ROM and cartridge RAM in one pool of blocks
 * (GBL_CACHE_BLOCK), through the public bus: every ROM byte read matches
 * the ROM, every cartridge RAM byte the last one written, the save file
 * holds them after a flush, and blocks lent to the option menu (scribbled
 * over meanwhile) never serve a byte.  With GBL_CACHE_DYN (all blocks in
 * the array, GBL_CACHE_DYN_INLINE), the blocks after the fixed part come
 * and go meanwhile, scribbled over while out of the cache (the C stack's
 * use of them); without GBL_CACHE_UNIFIED, the same for the ROM sectors. */
#include "gbl.h"
#include "gbl_options.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const uint8_t gbl_rom[0x4000] = {[0] = 0xA5};
const uint32_t gbl_rom_size = 0x8000;
const uint8_t gbl_rom_resident[1][4096] = {{0}};
const uint8_t gbl_rom_chunk_map[8] = {0};
const gbl_bank_table gbl_tables[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
const uint16_t gbl_table_count = 2;

#define NSEC (GBL_ERAM_SIZE / 512)
static uint8_t disk[NSEC][512];
static uint8_t mirror[GBL_ERAM_SIZE];
static unsigned reads, writes, rom_reads;

static uint8_t rom_byte(uint32_t x) { return (uint8_t)(x * 7u + (x >> 8) * 13u + 1u); }

int gbl_platform_save_read(uint32_t s, uint8_t *dst) {
    assert(s < NSEC);
    ++reads;
    memcpy(dst, disk[s], 512);
    return 1;
}
int gbl_platform_save_write(uint32_t s, const uint8_t *src) {
    assert(s < NSEC);
    ++writes;
    memcpy(disk[s], src, 512);
    return 1;
}
int gbl_platform_rom_read(uint32_t s, uint8_t *dst) {
    assert(s >= 32 && s < 64);
    ++rom_reads;
    for (unsigned i = 0; i < 512; i++) dst[i] = rom_byte(s * 512u + i);
    return 1;
}
uint16_t *gbl_platform_line_begin(uint8_t ly) { (void)ly; return 0; }
void gbl_platform_line_end(uint8_t ly) { (void)ly; }
void gbl_platform_vblank(void) {}
int gbl_platform_line_check(uint8_t ly, uint32_t sig) { (void)ly; (void)sig; return 0; }

static uint32_t rng = 7;
static uint32_t next(void) { return rng = rng * 1664525u + 1013904223u; }

static void step(unsigned i) {
    uint32_t r = next() >> 8;
    if (r % 3 == 0) { /* ROM bank 1, clustered like code and tables */
        uint16_t a = (uint16_t)(0x4000 + ((r >> 4) % 0x4000));
        assert(gbl_rd(a) == rom_byte(0x4000u + (a - 0x4000u)));
        return;
    }
    uint16_t off = (uint16_t)((r >> 4) % (i & 1 ? 0x2000 : 0x600)); /* a hot area, as a stack */
    uint16_t a = (uint16_t)(0xA000 + off);
    if (r & 8) {
        mirror[off] = (uint8_t)(r >> 16);
        gbl_wr(a, mirror[off]);
    } else {
        assert(gbl_rd(a) == mirror[off]);
    }
}

#if GBL_CACHE_DYN
void gbl_platform_pool(void) {}

/* The blocks after the fixed part follow a saw: shrink to it, grow to the
 * most, and the RAM out of the cache scribbled over every step. */
static void dyn_step(unsigned i) {
    static int down;
    if (i % 997 == 0) {
        if (down ? !gbl_cache_shrink() : !gbl_cache_grow()) down = !down;
    }
    uintptr_t top = gbl_cache_top(), end = (uintptr_t)gbl_stream_scratch() + GBL_CACHE_DYN_BLOCK *
#if GBL_CACHE_UNIFIED
        (GBL_CACHE_MAX_BYTES / GBL_CACHE_BLOCK);
#else
        GBL_STREAM_MAX;
#endif
    memset((void *)top, (int)(i * 5u + 3u), end - top);
}
#define DYN_STEP(i) dyn_step(i)
#else
#define DYN_STEP(i) ((void)0)
#endif

int main(void) {
    for (unsigned s = 0; s < NSEC; s++)
        for (unsigned k = 0; k < 512; k++) disk[s][k] = mirror[s * 512 + k] = (uint8_t)(s * 31 + k);
    gbl_reset();
    gbl_wr(0x2000, 1); /* ROM bank 1 */
    gbl_wr(0x4000, 0); /* RAM bank 0 */
    gbl_wr(0, 0x0A);   /* RAM on */
    for (unsigned i = 0; i < 200000; i++) {
        step(i);
        DYN_STEP(i);
    }
#if GBL_CACHE_UNIFIED
    /* The option menu borrows the pool's start: scribble over it. */
    uintptr_t top = gbl_cache_lend();
    memset((void *)gbl_stream_scratch(), 0x5A, top - (uintptr_t)gbl_stream_scratch());
    for (unsigned i = 0; i < 50000; i++) {
        memset((void *)gbl_stream_scratch(), (int)i, top - (uintptr_t)gbl_stream_scratch());
        step(i);
    }
    gbl_stream_invalidate();
#endif
    for (unsigned i = 0; i < 50000; i++) {
        step(i);
        DYN_STEP(i);
    }
    gbl_save_flush();
    assert(!gbl_save_pending());
    for (unsigned s = 0; s < NSEC; s++) assert(!memcmp(disk[s], mirror + s * 512, 512));
    printf("block %d: %u ROM sectors, %u RAM sectors read, %u written\n", GBL_CACHE_BLOCK, rom_reads, reads,
           writes);
    return 0;
}
