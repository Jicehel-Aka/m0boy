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
#include "gbl_options.h"
#include "meta_display.h"
#include "meta_lcd.h"
#include "dmg_reference.h"
#include "meta_lcd_pack.h"

#include <stdint.h>

#include <stdio.h>
#include <string.h>

const uint8_t gbl_rom[0x8000];
const uint32_t gbl_rom_size = sizeof(gbl_rom);
const gbl_bank_table gbl_tables[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
const uint16_t gbl_table_count = 2;
void gbl_platform_vblank(void) {}

/* ---- simulated panel ------------------------------------------------------ */

static uint16_t panel[128][160];
static uint32_t sent, rows_sent;

uint32_t meta_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *p = (const uint16_t *)buf;
    for (unsigned r = 0; r < rows; r++) {
        if (row0 + r >= 128) { printf("window past the panel: row %u\n", row0 + r); continue; }
#if META_LCD_12BPP
        /* as the driver does: packed in place, then read back as the panel would */
        uint8_t *b = (uint8_t *)(uintptr_t)(p + r * 160);
        meta_lcd_pack444(b, 160);
        for (unsigned x = 0; x < 160; x += 2) {
            const uint8_t *q = b + x / 2 * 3;
            panel[row0 + r][x] = (uint16_t)(q[0] << 4 | q[1] >> 4);
            panel[row0 + r][x + 1] = (uint16_t)((q[1] & 15) << 8 | q[2]);
        }
#else
        memcpy(panel[row0 + r], p + r * 160, 320);
#endif
    }
    rows_sent += rows;
    return ++sent;
}
uint32_t meta_lcd_done_seq(void) { return sent; }

/* ---- expected picture ------------------------------------------------------ */

static const uint32_t rgb[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};

/* The panel's pixel format: byte-swapped RGB565, or 0x0RGB with
 * META_LCD_12BPP (the MADCTL BGR bit of the init corrects the glass). */
static uint16_t be565(uint32_t c) {
#if META_LCD_12BPP
    return (uint16_t)(((c >> 12) & 0xF00) | ((c >> 8) & 0xF0) | ((c >> 4) & 0xF));
#else
    uint16_t v = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F));
    return (uint16_t)((v >> 8) | (v << 8));
#endif
}

/* SMOOTH: row k of each group of 9 lines is (8 - k)/9 of line k and
 * (k + 1)/9 of line k + 1, rounded per channel. */
static uint32_t smooth_rgb(unsigned k, unsigned a, unsigned b) {
    uint32_t c = 0;
    for (int sh = 0; sh <= 16; sh += 8)
        c |= (((8 - k) * ((rgb[a] >> sh) & 255) + (k + 1) * ((rgb[b] >> sh) & 255) + 4) / 9) << sh;
    return c;
}

static uint16_t expected_pixel(uint8_t mode, uint8_t ref[144][160], unsigned row, unsigned x) {
    if (mode == META_MODE_SMOOTH) {
        unsigned group = row / 8, k = row % 8, line = group * 9 + k;
        return be565(smooth_rgb(k, ref[line][x], ref[line + 1][x]));
    }
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
    if (mode == META_MODE_TBLEND) {
        /* TILE's rows; line 8k+7 (not 63, 95, 143) is averaged into the
         * row of line 8k+8. */
        unsigned r = 0;
        for (unsigned ly = 0; ly < 143; ly++, r++) {
            int held = (ly % 8) == 7 && ly != 63 && ly != 95;
            if (r != row) { ly += held; continue; }
            if (!held) return be565(rgb[ref[ly][x]]);
            unsigned a = ref[ly][x], b = ref[ly + 1][x];
            uint32_t c = 0;
            for (int sh = 0; sh <= 16; sh += 8)
                c |= ((((rgb[a] >> sh) & 255) + ((rgb[b] >> sh) & 255)) / 2) << sh;
            return be565(c);
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
    static const char *names[] = {"smooth", "tile", "blend", "dark", "crop", "skip", "tblend"};
    int failures = 0;
    /* The firmware's SMOOTH table against the formula. */
    extern const uint32_t meta_smooth_pairs[4][256];
    for (unsigned k = 0; k < 4; k++)
        for (unsigned i = 0; i < 256; i++) {
            /* upper group | lower group << 4, left pixel in bits 3:2 */
            uint32_t want = be565(smooth_rgb(k, (i >> 2) & 3, (i >> 6) & 3)) |
                            (uint32_t)be565(smooth_rgb(k, i & 3, (i >> 4) & 3)) << 16;
            if (meta_smooth_pairs[k][i] != want) {
                printf("smooth table entry %u/%u wrong\n", k, i);
                failures++;
            }
        }
    /* Every mode, then BLEND again: it shares its buffers with TBLEND. */
    for (unsigned step = 0; step <= META_MODE_COUNT; step++) {
        uint8_t mode = step < META_MODE_COUNT ? (uint8_t)step : (uint8_t)META_MODE_BLEND;
#if !META_MERGE_MODES
        if (mode == META_MODE_BLEND || mode == META_MODE_DARK) continue; /* left out of this profile */
#endif
        gbl_reset();
        for (int i = 0; i < 0x2000; i++) gbl_vram[i] = (uint8_t)rnd();
        for (int i = 0; i < 0xA0; i++) gbl_oam[i] = (uint8_t)rnd();
        gbl_oam_dirty = 1;
        gbl_io_wr(0x40, 0xE3); /* LCD, BG, sprites, window on */
        gbl_io_wr(0x4A, 90);
        gbl_io_wr(0x4B, 60);
        meta_display_set_mode(mode);
        memset(panel, 0, sizeof(panel));
        uint32_t frames_bad = 0, frames_late = 0, rows_before = rows_sent;
        for (int frame = 0; frame < 400; frame++) {
            /* SMOOTH may show a row one frame late (drawn from its lower line
             * alone when only that line changed): its state changes every
             * other frame, and only the frames after an unchanged one must be
             * exact (the late ones are counted). */
            int settled = mode != META_MODE_SMOOTH || (frame & 1);
            if (frame && (mode != META_MODE_SMOOTH || !(frame & 1))) {
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
            if (bad_rows && !settled) {
                frames_late++;
            } else if (bad_rows) {
                if (frames_bad++ < 3)
                    printf("%s frame %d: %d rows wrong (first row %d)\n", names[mode], frame, bad_rows, first_bad);
            }
        }
        printf("display_sim %-6s: %s, %u rows sent for 400 frames (%.0f%% of a full redraw)", names[mode],
               frames_bad ? "MISMATCH" : "panel exact", rows_sent - rows_before,
               100.0 * (rows_sent - rows_before) / (400.0 * 128));
        if (mode == META_MODE_SMOOTH) printf(", %u of 200 changed frames with a late row", frames_late);
        printf("\n");
        if (mode == META_MODE_SMOOTH) {
            /* Things that keep moving: sprites going down one line per frame
             * must not leave late rows after the first frame. */
            unsigned moving_late = 0;
            gbl_io_wr(0x47, 0xE4); /* the identity palette has its own path */
            for (int frame = 0; frame < 120; frame++) {
                for (int i = 0; i < 40; i += 4) gbl_wr((uint16_t)(0xFE00 + i * 4), (uint8_t)(16 + (frame + i) % 150));
                gbl_io_wr(0x43, (uint8_t)(frame * 3)); /* fine scroll too */
                gbr.cyc += 70224u;
                gbl_sync();
                meta_display_flush();
                reference(ref);
                int bad = 0;
                for (unsigned row = 0; row < 128 && !bad; row++)
                    for (unsigned x = 0; x < 160; x++)
                        if (panel[row][x] != expected_pixel(mode, ref, row, x)) { bad = 1; break; }
                if (bad && frame > 0) moving_late++;
            }
            printf("display_sim smooth : sprites moving down and scrolling (BGP E4), %u of 119 frames with a late row\n", moving_late);
            if (moving_late) failures++;
        }
        if (frames_bad) failures++;
    }
    return failures ? 1 : 0;
}
