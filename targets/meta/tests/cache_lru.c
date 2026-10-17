/* Public bus accesses against a separate CLOCK model (every access marks
 * its sector, the hand evicts the first unmarked way), including dirty
 * eviction, cartridge RAM banks (GBL_ERAM_SIZE above 8 KiB) and ROM
 * bank 0. */
#include "gbl.h"
#include "gbl_options.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const uint8_t gbl_rom[0x4000] = {[0] = 0xA5, [0x2345] = 0x5A};
const uint32_t gbl_rom_size = 0x8000;
const uint8_t gbl_rom_resident[1][4096] = {{0}};
const uint8_t gbl_rom_chunk_map[8] = {0};
const gbl_bank_table gbl_tables[2] = {{0,0,0,0}, {0,0,0,0}};
const uint16_t gbl_table_count = 2;
#define NSEC (GBL_ERAM_SIZE / 512)
static uint8_t disk[NSEC][512];
static unsigned reads, writes, rom_reads;
int gbl_platform_save_read(uint32_t s, uint8_t *dst) {
    assert(s < NSEC); ++reads; memcpy(dst, disk[s], 512); return 1;
}
int gbl_platform_save_write(uint32_t s, const uint8_t *src) {
    assert(s < NSEC); ++writes; memcpy(disk[s], src, 512); return 1;
}
int gbl_platform_rom_read(uint32_t s, uint8_t *dst) {
    assert(s >= 32); ++rom_reads; memset(dst, 0x77, 512); return 1;
}
uint16_t *gbl_platform_line_begin(uint8_t ly) { (void)ly; return 0; }
void gbl_platform_line_end(uint8_t ly) { (void)ly; }
void gbl_platform_vblank(void) {}
int gbl_platform_line_check(uint8_t ly, uint32_t sig) { (void)ly; (void)sig; return 0; }

#define W GBL_ERAM_CACHE_SECTORS
static int tag[W];
static uint8_t ref[W];
static unsigned hand, bank = ~0u;
static uint8_t values[NSEC];
static void access_sector(unsigned s, unsigned step) {
    unsigned i = 0;
    while (i < W && tag[i] != (int)s) ++i;
    unsigned before = reads;
    unsigned miss = i == W;
    if (miss) {
        for (;;) {
            i = hand;
            hand = (hand + 1) % W;
            if (!ref[i]) break;
            ref[i] = 0;
        }
        tag[i] = (int)s;
    }
    ref[i] = 1;
    if (s / 16 != bank) {
        bank = s / 16;
        gbl_wr(0x4000, (uint8_t)bank); /* MBC5 RAM bank */
    }
    uint16_t a = (uint16_t)(0xA000 + (s % 16) * 512);
    assert(gbl_rd(a) == values[s]);
    assert(reads == before + miss);
    if ((step & 3u) == 0) {
        values[s] = (uint8_t)(values[s] + 1);
        gbl_wr(a, values[s]);
    }
}
int main(void) {
    for (unsigned i = 0; i < NSEC; ++i) {
        values[i] = (uint8_t)i;
        memset(disk[i], i, 512);
    }
    for (unsigned i = 0; i < W; ++i) tag[i] = -1;
    gbl_reset();
    /* Lower slot: always bank 0. Upper slot: MBC5 explicitly selects 0. */
    assert(gbl_rd(0) == 0xA5 && gbl_rd(0x2345) == 0x5A);
    gbl_wr(0x2000, 0);
    assert(gbl_rd(0x4000) == 0xA5 && gbl_rd(0x6345) == 0x5A);
    assert(rom_reads == 0 && gbl_stream_misses == 0);
    gbl_wr(0x2000, 1);
    assert(gbl_rd(0x4000) == 0x77 && rom_reads == 1);
    /* Switch back to resident bank 0 after filling a ROM cache entry. */
    gbl_wr(0x2000, 0);
    assert(gbl_rd(0x6345) == 0x5A && rom_reads == 1);
    gbl_wr(0x2000, 1);
    assert(gbl_rd(0x4000) == 0x77 && rom_reads == 1);
    gbl_wr(0, 0x0A);
    const unsigned regression[] = {0,3,0,6,2,1,9,4,8,10};
    for (unsigned i = 0; i < sizeof(regression)/sizeof(*regression); ++i)
        access_sector(regression[i], i);
    uint32_t rng = 31;
    for (unsigned i = 0; i < 150000; ++i) {
        rng = rng * 1664525u + 1013904223u;
        /* Mostly a few hot sectors, as games use their work RAM. */
        unsigned s = (rng >> 28) < 12 ? (rng >> 16) % 6 : (rng >> 16) % NSEC;
        access_sector(s, i);
    }
    /* Periodic save (meta_save_tick): one sector per call, then nothing. */
    assert(gbl_save_pending());
    unsigned before = writes, steps = 0;
    for (int i = 0; (i = gbl_save_flush_next(i)) >= 0;) ++steps;
    assert(writes == before + steps && steps <= W && !gbl_save_pending());
    assert(gbl_save_flush_next(0) < 0 && writes == before + steps);
    for (unsigned i = 0; i < NSEC; ++i) assert(disk[i][0] == values[i]);
    /* Sectors written back stay cached and take new writes. */
    for (unsigned i = 0; i < 4000; ++i) {
        rng = rng * 1664525u + 1013904223u;
        access_sector((rng >> 16) % 6, i);
    }
    gbl_save_flush();
    for (unsigned i = 0; i < NSEC; ++i) {
        uint8_t want[512];
        memset(want, i, 512);
        memset(want, values[i], 1); /* only offset 0 is ever written */
        assert(disk[i][0] == values[i] && !memcmp(disk[i] + 1, want + 1, 511));
    }
    assert(writes > 0);
    printf("CLOCK matches model (%u reads, %u writes); dirty data kept; bank 0 bypasses ROM cache\n",
           reads, writes);
    return 0;
}
