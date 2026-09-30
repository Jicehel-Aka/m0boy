/*
 * Runs the real META display pipeline (platform/src/meta_display.c) against
 * a simulated ST7735 that honours row windows exactly like the panel, and
 * checks the panel after every frame against an independently computed
 * image: the per-pixel DMG reference, mapped from 144 lines to 128 rows by
 * each display mode.  Random VRAM/OAM/register changes between frames
 * exercise line skipping (unchanged rows are neither rendered nor sent) and
 * the strip/window logic, including the last rows of the screen.
 */
#include "gbl.h"
#include "gbl_internal.h"
#include "meta_display.h"
#include "meta_lcd.h"
#include "dmg_reference.h"

#include <stdio.h>
#include <string.h>

const uint8_t gbl_rom[0x8000];
const uint32_t gbl_rom_size = sizeof(gbl_rom);
const gbl_bank_table gbl_tables[2] = {{0, 0, 0}, {0, 0, 0}};
const uint16_t gbl_table_count = 2;
void gbl_platform_vblank(void) {}

/* ---- simulated panel ------------------------------------------------------ */

static uint16_t panel[128][160];
static uint32_t sent, rows_sent;

uint32_t meta_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *p = (const uint16_t *)buf;
    for (unsigned r = 0; r < rows; r++) {
        if (row0 + r >= 128) { printf("window past the panel: row %u\n", row0 + r); continue; }
        memcpy(panel[row0 + r], p + r * 160, 320);
    }
    rows_sent += rows;
    return ++sent;
}
uint32_t meta_lcd_done_seq(void) { return sent; }

/* ---- expected picture ------------------------------------------------------ */

static const uint32_t rgb[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};

static uint16_t be565(uint32_t c) {
    uint16_t v = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F));
    return (uint16_t)((v >> 8) | (v << 8));
}

static uint16_t expected_pixel(uint8_t mode, uint8_t ref[144][160], unsigned row, unsigned x) {
    if (mode == META_MODE_CROP) return be565(rgb[ref[row + 8][x]]);
    if (mode == META_MODE_TILE) {
        /* Rows are the lines left after removing 8k+7, except 63 and 95. */
        unsigned seen = 0;
        for (unsigned ly = 0; ly < 144; ly++) {
            if ((ly % 8) == 7 && ly != 63 && ly != 95) continue;
            if (seen++ == row) return be565(rgb[ref[ly][x]]);
        }
        return 0;
    }
    unsigned group = row / 8, k = row % 8, line = group * 9 + k;
    if (mode == META_MODE_SKIP || k != 7) return be565(rgb[ref[line][x]]);
    unsigned a = ref[line][x], b = ref[line + 1][x];
    if (mode == META_MODE_DARK) return be565(rgb[a > b ? a : b]);
    uint32_t c = 0;
    for (int sh = 0; sh <= 16; sh += 8)
        c |= ((((rgb[a] >> sh) & 255) + ((rgb[b] >> sh) & 255)) / 2) << sh;
    return be565(c);
}

static uint32_t rng = 99;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

int main(void) {
    static uint8_t ref[144][160];
    static const char *names[] = {"tile", "blend", "dark", "crop", "skip"};
    int failures = 0;
    for (uint8_t mode = 0; mode < META_MODE_COUNT; mode++) {
        gbl_reset();
        for (int i = 0; i < 0x2000; i++) gbl_vram[i] = (uint8_t)rnd();
        for (int i = 0; i < 0xA0; i++) gbl_oam[i] = (uint8_t)rnd();
        gbl_oam_dirty = 1;
        gbl_io_wr(0x40, 0xE3); /* LCD, BG, sprites, window on */
        gbl_io_wr(0x4A, 90);
        gbl_io_wr(0x4B, 60);
        meta_display_set_mode(mode);
        memset(panel, 0, sizeof(panel));
        uint32_t frames_bad = 0, rows_before = rows_sent;
        for (int frame = 0; frame < 400; frame++) {
            if (frame) {
                /* Mostly small changes, sometimes none, sometimes a scroll. */
                switch (rnd() % 6) {
                case 0: break;
                case 1: gbl_wr((uint16_t)(0x8000 + rnd() % 0x1800), (uint8_t)rnd()); break;
                case 2: gbl_wr((uint16_t)(0x9800 + rnd() % 0x800), (uint8_t)rnd()); break;
                case 3: gbl_wr((uint16_t)(0xFE00 + rnd() % 0xA0), (uint8_t)rnd()); break;
                case 4: gbl_io_wr((uint8_t)(0x42 + rnd() % 2), (uint8_t)rnd()); break;
                default: gbl_io_wr((uint8_t)(0x47 + rnd() % 3), (uint8_t)rnd()); break;
                }
            }
            gbr.cyc += 70224u;
            gbl_sync();
            meta_display_flush();
            reference(ref);
            int bad_rows = 0, first_bad = -1;
            for (unsigned row = 0; row < 128; row++)
                for (unsigned x = 0; x < 160; x++)
                    if (panel[row][x] != expected_pixel(mode, ref, row, x)) {
                        if (first_bad < 0) first_bad = (int)row;
                        bad_rows++;
                        break;
                    }
            if (bad_rows) {
                if (frames_bad++ < 3)
                    printf("%s frame %d: %d rows wrong (first row %d)\n", names[mode], frame, bad_rows, first_bad);
            }
        }
        printf("display_sim %-5s: %s, %u rows sent for 400 frames (%.0f%% of a full redraw)\n", names[mode],
               frames_bad ? "MISMATCH" : "panel exact", rows_sent - rows_before,
               100.0 * (rows_sent - rows_before) / (400.0 * 128));
        if (frames_bad) failures++;
    }
    return failures ? 1 : 0;
}
