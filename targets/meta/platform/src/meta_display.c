/*
 * META display pipeline (see meta_display.h).
 *
 * Rows are rendered straight to RGB565 into strips of META_STRIP_LINES rows;
 * in the merge modes the two lines being merged are rendered as shade
 * indices instead (the PPU palette is switched per line) and combined two
 * pixels per lookup in a 1 KiB table.  With META_LINE_SKIP a row whose
 * inputs did not change since it was drawn is neither rendered nor sent;
 * strips therefore hold runs of consecutive rows, each sent to its own
 * window.
 */
#include "meta_display.h"

#include "gbl.h"
#include "gbl_config.h"
#include "gbl_options.h"
#include "meta_lcd.h"

#include <string.h>

/* Rows per strip, and strips in flight: with META_LCD_DMA the DMA interrupt
 * chains queued strips, so rendering only waits when all buffers are busy. */
#ifndef META_STRIP_LINES
#define META_STRIP_LINES 4
#endif
/* ROM streaming (2 KiB sector cache) and cartridge RAM kept on the card
 * (1 KiB cache) need RAM: those builds keep two strips and leave out the
 * merge modes (their 1.3 KiB of tables). */
#if GBL_ROM_STREAM || GBL_ERAM_ON_SD
#define META_STREAM_RAM 1
#else
#define META_STREAM_RAM 0
#endif
#ifndef META_LCD_BUFFERS
#define META_LCD_BUFFERS (META_STREAM_RAM ? 2 : 3)
#endif
/* BLEND and DARK modes (1 KiB table and one spare line). */
#ifndef META_MERGE_MODES
#define META_MERGE_MODES (!META_STREAM_RAM)
#endif
/* Skip rendering and sending display rows whose inputs did not change since
 * they were last drawn (exact: see GBL_LINE_SIGNATURE). */
#ifndef META_LINE_SKIP
#define META_LINE_SKIP 1
#endif

#define RGB565(rgb) ((uint16_t)((((rgb) >> 8) & 0xF800) | (((rgb) >> 5) & 0x07E0) | (((rgb) >> 3) & 0x001F)))
#define SWAP16(v) ((uint16_t)(((v) >> 8) | ((v) << 8)))

const uint16_t meta_dmg_palette[4] = {
    SWAP16(RGB565(0xE0F8D0)), SWAP16(RGB565(0x88C070)),
    SWAP16(RGB565(0x346856)), SWAP16(RGB565(0x081820)),
};
#if META_MERGE_MODES
static const uint32_t dmg_rgb[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};
#endif

uint16_t meta_display_lcd_waits;
uint16_t meta_display_rows_skipped;

static uint16_t strip[META_LCD_BUFFERS][160 * META_STRIP_LINES] __attribute__((aligned(4)));
static uint32_t strip_seq[META_LCD_BUFFERS]; /* transfer that last used each buffer */
static uint8_t strip_cur, strip_fill, strip_row0;

static uint8_t display_mode = META_MODE_TILE;
#if META_MERGE_MODES
static uint16_t spare_line[160] __attribute__((aligned(4)));
static uint32_t merge2[256]; /* a0 | a1 << 2 | b0 << 4 | b1 << 6 -> two merged pixels */
#endif
static uint8_t index_palette; /* PPU currently emits shade indices */

#if META_LINE_SKIP
static uint32_t row_sig[128];
static uint8_t row_valid[128];
static uint32_t pending_sig; /* merge modes: signature of the held line 7 */
#endif

static void build_layout(void);

#if META_MERGE_MODES
static uint16_t merged_color(unsigned a, unsigned b) {
    if (display_mode == META_MODE_DARK) return meta_dmg_palette[a > b ? a : b];
    uint32_t x = dmg_rgb[a], y = dmg_rgb[b];
    uint32_t avg = (((x >> 16) + (y >> 16)) / 2) << 16 |
                   ((((x >> 8) & 255) + ((y >> 8) & 255)) / 2) << 8 |
                   (((x & 255) + (y & 255)) / 2);
    return SWAP16(RGB565(avg));
}
#endif

static void set_index_palette(uint8_t on) {
    if (index_palette == on) return;
    index_palette = on;
    for (unsigned i = 0; i < 4; i++) gbl_lcd_palette[i] = on ? (uint16_t)i : meta_dmg_palette[i];
    gbl_lcd_palette_primary = !on;
    gbl_ppu_palette_changed();
}

uint8_t meta_display_mode(void) { return display_mode; }

void meta_display_set_mode(uint8_t mode) {
#if !META_MERGE_MODES
    if (mode == META_MODE_BLEND || mode == META_MODE_DARK) mode = META_MODE_CROP;
#endif
    display_mode = mode < META_MODE_COUNT ? mode : META_MODE_TILE;
    index_palette = 1;
    set_index_palette(0);
    gbl_frame_invalidate();
    build_layout();
#if META_LINE_SKIP
    memset(row_valid, 0, sizeof(row_valid));
#endif
#if META_MERGE_MODES
    for (unsigned k = 0; k < 256; k++)
        merge2[k] = (uint32_t)merged_color(k & 3, (k >> 4) & 3) |
                    ((uint32_t)merged_color((k >> 2) & 3, k >> 6) << 16);
#endif
}

/* Per-line layout of the current mode: what each DMG line becomes. */
enum { L_ROW, L_HOLD, L_MERGE, L_DROP };
static uint8_t line_kind[144], line_row[144];

static void build_layout(void) {
    unsigned row = 0;
    for (unsigned ly = 0; ly < 144; ly++) {
        uint8_t kind = L_ROW;
        unsigned r9 = ly % 9u;
        switch (display_mode) {
        case META_MODE_CROP: kind = (ly < 8 || ly >= 136) ? L_DROP : L_ROW; break;
        case META_MODE_SKIP: kind = r9 == 8 ? L_DROP : L_ROW; break;
        case META_MODE_TILE:
            /* gm0: drop the last line of each tile row (usually the blank
             * spacing line of fonts), except two to fill 128 rows. */
            kind = ((ly & 7) == 7 && ly != 63 && ly != 95) ? L_DROP : L_ROW;
            break;
        default: kind = r9 == 7 ? L_HOLD : r9 == 8 ? L_MERGE : L_ROW; break;
        }
        line_kind[ly] = kind;
        if (kind == L_MERGE) line_row[ly] = line_row[ly - 1];
        else if (kind != L_DROP) line_row[ly] = (uint8_t)row++;
    }
}

void meta_display_flush(void) {
    if (!strip_fill) return;
    strip_seq[strip_cur] = meta_lcd_send_rows(strip[strip_cur], strip_row0, strip_fill);
    strip_cur = (uint8_t)((strip_cur + 1) % META_LCD_BUFFERS);
    strip_fill = 0;
}

/* The buffer about to be filled may still be on its way to the display. */
static void claim_strip(void) {
    uint32_t seq = strip_seq[strip_cur];
    if ((int32_t)(meta_lcd_done_seq() - seq) >= 0) return;
    meta_display_lcd_waits++;
    while ((int32_t)(meta_lcd_done_seq() - seq) < 0) {}
}

int gbl_platform_line_check(uint8_t ly, uint32_t sig) {
    if (ly >= 144 || line_kind[ly] == L_DROP) return 0;
#if META_LINE_SKIP
    unsigned row = line_row[ly];
    if (line_kind[ly] == L_HOLD) { pending_sig = sig; return 1; } /* needed if its pair changed */
    if (line_kind[ly] == L_MERGE) sig = gbl_sig_combine(pending_sig, sig);
    sig = gbl_sig_combine(sig, (uint32_t)display_mode);
    if (row_valid[row] && row_sig[row] == sig) { meta_display_rows_skipped++; return 0; }
    row_sig[row] = sig;
    row_valid[row] = 1;
#else
    (void)sig;
#endif
    return 1;
}

uint16_t *gbl_platform_line_begin(uint8_t ly) {
    if (ly >= 144 || line_kind[ly] == L_DROP) return 0;
    uint8_t kind = line_kind[ly];
    /* The merged pair is rendered as shade indices, every other line as
     * final colours. */
    set_index_palette(kind == L_HOLD || kind == L_MERGE);
#if META_MERGE_MODES
    if (kind == L_MERGE) return spare_line;
#endif
    unsigned row = line_row[ly];
    /* Strips hold consecutive rows; start a new one at a gap. */
    if (strip_fill && row != (unsigned)strip_row0 + strip_fill) meta_display_flush();
    if (!strip_fill) {
        claim_strip();
        strip_row0 = (uint8_t)row;
    }
    return &strip[strip_cur][strip_fill * 160];
}

void gbl_platform_line_end(uint8_t ly) {
    uint8_t kind = line_kind[ly];
    if (kind == L_HOLD) return; /* completed by the merged line */
#if META_MERGE_MODES
    if (kind == L_MERGE) {
        uint32_t *d = (uint32_t *)&strip[strip_cur][strip_fill * 160];
        const uint32_t *b = (const uint32_t *)spare_line;
        for (int i = 0; i < 80; i++) {
            uint32_t wa = d[i], wb = b[i];
            d[i] = merge2[(wa & 3u) | ((wa >> 14) & 0x0Cu) | ((wb & 3u) << 4) | ((wb >> 10) & 0xC0u)];
        }
    }
#endif
    if (++strip_fill == META_STRIP_LINES) meta_display_flush();
}
