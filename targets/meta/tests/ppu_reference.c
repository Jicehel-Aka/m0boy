/*
 * Compare the optimized gbl scanline renderer with a direct per-pixel DMG
 * reference on random VRAM/OAM/register states: BG scrolling, 8000/8800 tile
 * addressing, both tile maps, the window (including WX < 7 and line
 * counting), 8x8 and 8x16 sprites with flips, palettes, the ten-sprite limit,
 * X/OAM-index priority, and BG-over-OBJ priority.
 */
#include "gbl.h"
#include "gbl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t gbl_rom[0x8000];
const uint32_t gbl_rom_size = sizeof(gbl_rom);
const gbl_bank_table gbl_tables[2] = {{0, 0, 0}, {0, 0, 0}};
const uint16_t gbl_table_count = 2;

static uint16_t line[160] __attribute__((aligned(4)));
static uint8_t got[144][160];
static int lines_seen;

uint16_t *gbl_platform_line_begin(uint8_t ly) { (void)ly; return line; }
void gbl_platform_line_end(uint8_t ly) {
    if (ly < 144) for (int x = 0; x < 160; x++) got[ly][x] = (uint8_t)line[x];
    lines_seen++;
}
void gbl_platform_vblank(void) {}
static uint32_t sigs[144];
int gbl_platform_line_check(uint8_t ly, uint32_t sig) {
    if (ly < 144) sigs[ly] = sig;
    return 1;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

#include "dmg_reference.h"

int main(void) {
    static uint8_t ref[144][160];
    int failures = 0;
    for (int iter = 0; iter < 300; iter++) {
        gbl_reset();
        for (int i = 0; i < 0x2000; i++) gbl_vram[i] = (uint8_t)rnd();
        for (int i = 0; i < 40; i++) {
            gbl_oam[i * 4] = (uint8_t)(rnd() % 176);
            gbl_oam[i * 4 + 1] = (uint8_t)(rnd() % 176);
            gbl_oam[i * 4 + 2] = (uint8_t)rnd();
            gbl_oam[i * 4 + 3] = (uint8_t)rnd();
            if (iter % 3 == 0) gbl_oam[i * 4 + 1] = (uint8_t)(40 + rnd() % 8); /* force X ties */
        }
        gbl_oam_dirty = 1;
        gbl_io_wr(0x40, (uint8_t)(0x80 | (rnd() & 0x7F)));
        gbl_io_wr(0x42, (uint8_t)rnd());
        gbl_io_wr(0x43, (uint8_t)rnd());
        gbl_io_wr(0x47, (uint8_t)rnd());
        gbl_io_wr(0x48, (uint8_t)rnd());
        gbl_io_wr(0x49, (uint8_t)rnd());
        gbl_io_wr(0x4A, (uint8_t)(rnd() % 150));
        gbl_io_wr(0x4B, (uint8_t)(rnd() % 172));
        lines_seen = 0;
        gbr.cyc += 70224u - 1u;
        gbl_sync();
        reference(ref);
        if (lines_seen != 144 || memcmp(ref, got, sizeof(ref)) != 0) {
            int bad = 0, fy = -1, fx = -1;
            for (int y = 0; y < 144; y++)
                for (int x = 0; x < 160; x++)
                    if (ref[y][x] != got[y][x]) { if (!bad) { fy = y; fx = x; } bad++; }
            if (failures++ < 5)
                printf("iter %d: %d pixels differ (first y=%d x=%d ref %u got %u) lcdc=%02x lines=%d\n",
                       iter, bad, fy, fx, fy >= 0 ? ref[fy][fx] : 0, fy >= 0 ? got[fy][fx] : 0,
                       gbl_io[0x40], lines_seen);
        }
    }
    /* Line signatures: after a random VRAM/OAM/register change, a line with
     * an unchanged signature must have unchanged pixels, and changed pixels
     * must come with a changed signature. */
    static uint8_t before[144][160];
    static uint32_t sig_before[144];
    int sig_wrong = 0, sig_same = 0, sig_lines = 0;
    for (int iter = 0; iter < 2000; iter++) {
        if (iter % 50 == 0) {
            gbl_reset();
            for (int i = 0; i < 0x2000; i++) gbl_vram[i] = (uint8_t)rnd();
            for (int i = 0; i < 0xA0; i++) gbl_oam[i] = (uint8_t)rnd();
            gbl_oam_dirty = 1;
            gbl_io_wr(0x40, (uint8_t)(0x80 | (rnd() & 0x7F)));
            gbl_io_wr(0x4A, (uint8_t)(rnd() % 150));
            gbl_io_wr(0x4B, (uint8_t)(rnd() % 172));
        }
        gbr.cyc += 70224u;
        gbl_sync();
        memcpy(before, got, sizeof(before));
        memcpy(sig_before, sigs, sizeof(sigs));
        switch (rnd() % 4) {
        case 0: gbl_wr((uint16_t)(0x8000 + rnd() % 0x2000), (uint8_t)rnd()); break;
        case 1: gbl_wr((uint16_t)(0xFE00 + rnd() % 0xA0), (uint8_t)rnd()); break;
        case 2: gbl_io_wr((uint8_t)(0x42 + rnd() % 2), (uint8_t)rnd()); break;
        default: gbl_io_wr((uint8_t)(0x47 + rnd() % 3), (uint8_t)rnd()); break;
        }
        gbr.cyc += 70224u;
        gbl_sync();
        for (int y = 0; y < 144; y++) {
            sig_lines++;
            int same_px = !memcmp(before[y], got[y], 160);
            if (sigs[y] == sig_before[y]) {
                sig_same++;
                if (!same_px) sig_wrong++;
            }
        }
    }
    if (sig_wrong) {
        printf("ppu_reference: %d lines kept their signature but changed pixels\n", sig_wrong);
        return 1;
    }
    printf("ppu_reference: line signatures exact (%d of %d lines unchanged)\n", sig_same, sig_lines);

    if (failures) {
        printf("ppu_reference: %d of 300 frames differ\n", failures);
        return 1;
    }
    printf("ppu_reference: 300 random frames match the per-pixel reference\n");
    return 0;
}
