/*
 * META display pipeline (see meta_display.h).
 *
 * SMOOTH resamples 9 DMG lines onto 8 rows by area: row k of
 * each group of nine lines is (8 - k)/9 of line k plus (k + 1)/9 of line
 * k + 1, two pixels per lookup in compile-time tables.  Lines are rendered
 * packed (gbl_ppu_packed: 2-bit shades, 40 bytes, no colour conversion in
 * the PPU); the previous one is kept and each row is blended from it and
 * the current one, the only conversion to colours.  With line skipping a row can need an upper line that
 * was not rendered because only the lower one changed: that row is then
 * drawn from the lower line alone for one frame and redrawn blended on the
 * next (keeping all 144 lines would take 5.6 KiB of RAM).  Lines that
 * changed are rendered again on the next frame, so things that keep moving
 * find their upper lines rendered and only the first frame of a change can
 * be late.
 *
 * The other modes render rows straight to RGB565 into strips of META_STRIP_LINES rows;
 * in the merge modes the two lines being merged are rendered as shade
 * indices instead (the PPU palette is switched per line) and combined two
 * pixels per lookup in a 1 KiB table.  TBLEND (the default; gm0's ESPboy
 * shrink; TILE where the merge modes are left out) keeps
 * TILE's rows but merges line 8k+7 into the next line: both are rendered
 * packed, the held one kept across frames (600 bytes, sharing RAM with the
 * BLEND/DARK buffers) so it is rendered only when it changes, and the pair
 * is blended by SMOOTH's loop with a 1/2 table.  With META_LINE_SKIP a row whose
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
/* The SMOOTH row blend in Thumb-1 (meta_display_asm.S) on Cortex-M0+. */
#ifndef META_SMOOTH_ASM
#if defined(__ARM_ARCH_6M__)
#define META_SMOOTH_ASM 1
#else
#define META_SMOOTH_ASM 0
#endif
#endif
/* SMOOTH: render again on the next frame the lines that changed, so that a
 * row whose lower line keeps moving finds its upper line rendered. */
#ifndef META_SMOOTH_SPECULATE
#define META_SMOOTH_SPECULATE 1
#endif
/* BLEND, DARK and TBLEND modes (1 KiB table and one spare line). */
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

static uint8_t display_mode = META_MODE_TILE; /* until meta_display_set_mode */
#if META_MERGE_MODES
/* Buffers of one mode at a time (meta_display_set_mode rebuilds them):
 * BLEND and DARK keep their merged line and table, TBLEND the held lines. */
static union {
    struct {
        uint16_t spare_line[160];
        uint32_t merge2[256]; /* a0 | a1 << 2 | b0 << 4 | b1 << 6 -> two merged pixels */
    } m;
    struct {
        /* Line 8k+7 of each merged pair, packed, kept across frames so that
         * it is rendered only when it changes; the line merged into it. */
        uint16_t hold[15][20];
        uint16_t lower[20];
    } t;
} mode_ram __attribute__((aligned(4)));
#define spare_line mode_ram.m.spare_line
#define merge2 mode_ram.m.merge2
#define tb_hold mode_ram.t.hold
#define tb_lower mode_ram.t.lower
#endif
static uint8_t index_palette; /* PPU currently emits shade indices */

#if META_LINE_SKIP
/* Signature of what each line last put on screen, by line (the modes that
 * keep whole lines index it like their rows).  A mode change voids them
 * all, so the mode needs no place in the signature. */
static uint32_t line_sig[144];
static uint8_t line_valid[144];
static uint32_t pending_sig; /* merge modes: signature of the held line 7 */
#endif

/* SMOOTH colours, generated at compile time: row k of a group takes
 * (8 - k)/9 of shade a (upper line) and (k + 1)/9 of shade b (lower line),
 * rounded per channel, byte-swapped RGB565.  Row 7 - k is row k with the
 * lines swapped, so four tables of two-pixel entries cover the eight rows.
 * The index is two 4-bit groups of the packed lines (gbl_ppu_packed: left
 * pixel in bits 3:2): upper | lower << 4; the entry holds the left pixel in
 * its low half (4 KiB of flash).  tests/display_sim.c checks them against
 * the formula. */
#define DMG_CH(s, sh) (((s) == 0 ? 0xE0F8D0u : (s) == 1 ? 0x88C070u : (s) == 2 ? 0x346856u : 0x081820u) >> (sh) & 255u)
#define SM_CH(k, a, b, sh) (((8u - (k)) * DMG_CH(a, sh) + ((k) + 1u) * DMG_CH(b, sh) + 4u) / 9u)
#define SM_565(k, a, b) ((SM_CH(k, a, b, 16) >> 3) << 11 | (SM_CH(k, a, b, 8) >> 2) << 5 | SM_CH(k, a, b, 0) >> 3)
#define SM_COL(k, a, b) ((SM_565(k, a, b) >> 8 | SM_565(k, a, b) << 8) & 0xFFFFu)
#define SM_E(k, i) (SM_COL(k, ((i) >> 2) & 3u, ((i) >> 6) & 3u) | SM_COL(k, (i) & 3u, ((i) >> 4) & 3u) << 16)
#define SM_E4(k, i) SM_E(k, i), SM_E(k, (i) + 1u), SM_E(k, (i) + 2u), SM_E(k, (i) + 3u)
#define SM_E16(k, i) SM_E4(k, i), SM_E4(k, (i) + 4u), SM_E4(k, (i) + 8u), SM_E4(k, (i) + 12u)
#define SM_E64(k, i) SM_E16(k, i), SM_E16(k, (i) + 16u), SM_E16(k, (i) + 32u), SM_E16(k, (i) + 48u)
#define SM_E256(k) {SM_E64(k, 0u), SM_E64(k, 64u), SM_E64(k, 128u), SM_E64(k, 192u)}
const uint32_t meta_smooth_pairs[4][256] = {SM_E256(0u), SM_E256(1u), SM_E256(2u), SM_E256(3u)};

#if META_MERGE_MODES
/* TBLEND: the same layout of entries with both lines weighted 1/2 (per
 * channel average rounded down, like BLEND), 1 KiB of flash. */
#define HF_CH(a, b, sh) ((DMG_CH(a, sh) + DMG_CH(b, sh)) / 2u)
#define HF_565(a, b) ((HF_CH(a, b, 16) >> 3) << 11 | (HF_CH(a, b, 8) >> 2) << 5 | HF_CH(a, b, 0) >> 3)
#define HF_COL(a, b) ((HF_565(a, b) >> 8 | HF_565(a, b) << 8) & 0xFFFFu)
#define HF_E(i) (HF_COL(((i) >> 2) & 3u, ((i) >> 6) & 3u) | HF_COL((i) & 3u, ((i) >> 4) & 3u) << 16)
#define HF_E4(i) HF_E(i), HF_E((i) + 1u), HF_E((i) + 2u), HF_E((i) + 3u)
#define HF_E16(i) HF_E4(i), HF_E4((i) + 4u), HF_E4((i) + 8u), HF_E4((i) + 12u)
#define HF_E64(i) HF_E16(i), HF_E16((i) + 16u), HF_E16((i) + 32u), HF_E16((i) + 48u)
const uint32_t meta_half_pairs[256] = {HF_E64(0u), HF_E64(64u), HF_E64(128u), HF_E64(192u)};

/* TBLEND: the held line was rendered this frame. */
static uint8_t tb_hold_fresh;
/* Pair of held line ly (8k+7, not 63, 95 or 143). */
#define TB_PAIR(ly) (((ly) >> 3) - ((ly) > 63) - ((ly) > 95))
#endif

/* SMOOTH state for the frame being drawn. */
/* The packed lines (twenty 16-bit groups): the upper line of the next row
 * and the line being rendered, swapped after each line. */
static uint16_t smooth_lines[2][20];
static uint8_t smooth_cur;         /* index of the line being rendered */
static uint8_t smooth_prev_ok;     /* the other packed line holds line ly - 1 */
static uint8_t smooth_prev_changed; /* line ly - 1 differs from what is on screen */
static uint8_t smooth_row;         /* row the current line completes (0xFF: none) */
static uint8_t smooth_force[18];   /* lines to render next frame even if unchanged */
static uint8_t smooth_redo[16];    /* rows drawn from their lower line alone: redraw */
#define BIT_GET(a, i) (((a)[(i) >> 3] >> ((i) & 7)) & 1u)
#define BIT_SET(a, i) ((a)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7)))
#define BIT_CLR(a, i) ((a)[(i) >> 3] &= (uint8_t)~(1u << ((i) & 7)))

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
    if (mode >= META_MODE_COUNT) mode = META_MODE_TBLEND;
#if !META_MERGE_MODES
    if (mode == META_MODE_BLEND || mode == META_MODE_DARK) mode = META_MODE_CROP;
    if (mode == META_MODE_TBLEND) mode = META_MODE_TILE;
#endif
    display_mode = mode;
    gbl_ppu_packed = display_mode == META_MODE_SMOOTH;
    index_palette = 1;
    set_index_palette(0);
    gbl_frame_invalidate();
    build_layout();
#if META_LINE_SKIP
    memset(line_valid, 0, sizeof(line_valid));
#endif
    memset(smooth_force, 0, sizeof(smooth_force));
    memset(smooth_redo, 0, sizeof(smooth_redo));
#if META_MERGE_MODES
    if (display_mode == META_MODE_BLEND || display_mode == META_MODE_DARK)
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
        case META_MODE_TBLEND:
            /* gm0 (ESPboy): TILE's layout, but the last line of each tile
             * row is merged into the first line of the next one instead of
             * dropped.  Line 143 has no next line and is dropped. */
            kind = ly == 143 ? L_DROP
                 : ((ly & 7) == 7 && ly != 63 && ly != 95) ? L_HOLD
                 : ((ly & 7) == 0 && ly && ly != 64 && ly != 96) ? L_MERGE : L_ROW;
            break;
        case META_MODE_SMOOTH: kind = L_ROW; break; /* rows handled apart */
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

/* SMOOTH: line ly is the lower line of row ly - 1 - group (j = ly % 9 >= 1)
 * and the upper line of row ly - group (j <= 7). */
/* ly / 9 for ly < 144 without a division (the M0+ has none). */
#define DIV9(ly) (((unsigned)(ly) * 57u) >> 9)

static int smooth_check(uint8_t ly, uint32_t sig) {
    unsigned group = DIV9(ly), j = ly - group * 9u;
    if (j == 0) smooth_prev_ok = smooth_prev_changed = 0; /* rows never span groups */
#if META_LINE_SKIP
    unsigned changed = !line_valid[ly] || line_sig[ly] != sig;
    line_sig[ly] = sig;
    line_valid[ly] = 1;
#else
    (void)sig;
    unsigned changed = 1;
#endif
    unsigned render = changed || BIT_GET(smooth_force, ly);
#if META_SMOOTH_SPECULATE
    /* A line that changed is rendered again next frame: moving things keep
     * changing the lines next to the ones that changed. */
    if (changed) BIT_SET(smooth_force, ly);
    else BIT_CLR(smooth_force, ly);
#else
    BIT_CLR(smooth_force, ly);
#endif
    smooth_row = 0xFF;
    if (j) {
        unsigned row = ly - 1u - group;
        unsigned redo = BIT_GET(smooth_redo, row);
        if (changed || smooth_prev_changed || redo) {
            smooth_row = (uint8_t)row;
            render = 1;
            if (!smooth_prev_ok) {
                /* The upper line was not rendered (it had not changed): draw
                 * from this line alone now, blended on the next frame. */
                BIT_SET(smooth_force, ly - 1u);
                BIT_SET(smooth_redo, row);
            } else {
                BIT_CLR(smooth_redo, row);
            }
        } else {
            meta_display_rows_skipped++;
        }
    }
    smooth_prev_changed = (uint8_t)changed;
    if (!render) smooth_prev_ok = 0;
    return (int)render;
}

int gbl_platform_line_check(uint8_t ly, uint32_t sig) {
    if (ly >= 144 || line_kind[ly] == L_DROP) return 0;
    if (display_mode == META_MODE_SMOOTH) return smooth_check(ly, sig);
#if META_LINE_SKIP && META_MERGE_MODES
    if (display_mode == META_MODE_TBLEND && line_kind[ly] != L_ROW) {
        /* The held line is rendered only when it changed (its packed copy
         * stays valid); the merged row is redrawn when either line did. */
        unsigned changed = !line_valid[ly] || line_sig[ly] != sig;
        line_sig[ly] = sig;
        line_valid[ly] = 1;
        if (line_kind[ly] == L_HOLD) return (int)(tb_hold_fresh = (uint8_t)changed);
        if (changed || tb_hold_fresh) return 1;
        meta_display_rows_skipped++;
        return 0;
    }
#endif
#if META_LINE_SKIP
    if (line_kind[ly] == L_HOLD) { pending_sig = sig; return 1; } /* needed if its pair changed */
    if (line_kind[ly] == L_MERGE) sig = gbl_sig_combine(pending_sig, sig);
    if (line_valid[ly] && line_sig[ly] == sig) { meta_display_rows_skipped++; return 0; }
    line_sig[ly] = sig;
    line_valid[ly] = 1;
#else
    (void)sig;
#endif
    return 1;
}

/* The strip slot for row `row`, starting a new strip at a gap. */
static uint16_t *strip_slot(unsigned row) {
    if (strip_fill && row != (unsigned)strip_row0 + strip_fill) meta_display_flush();
    if (!strip_fill) {
        claim_strip();
        strip_row0 = (uint8_t)row;
    }
    return &strip[strip_cur][strip_fill * 160];
}

uint16_t *gbl_platform_line_begin(uint8_t ly) {
    if (ly >= 144 || line_kind[ly] == L_DROP) return 0;
    if (display_mode == META_MODE_SMOOTH) return smooth_lines[smooth_cur]; /* packed */
    uint8_t kind = line_kind[ly];
#if META_MERGE_MODES
    if (display_mode == META_MODE_TBLEND) {
        /* The merged pair is rendered packed, every other line as colours. */
        gbl_ppu_packed = kind != L_ROW;
        if (kind == L_HOLD) return tb_hold[TB_PAIR(ly)];
        if (kind == L_MERGE) return tb_lower;
        return strip_slot(line_row[ly]);
    }
#endif
    /* The merged pair is rendered as shade indices, every other line as
     * final colours. */
    set_index_palette(kind == L_HOLD || kind == L_MERGE);
#if META_MERGE_MODES
    if (kind == L_MERGE) return spare_line;
#endif
    return strip_slot(line_row[ly]);
}

#if META_SMOOTH_ASM
/* meta_display_asm.S (Thumb-1): the loop below. */
void meta_smooth_blend_asm(uint32_t *row, const uint16_t *upper, const uint16_t *lower, const uint32_t *pairs,
                           uint32_t swap);
#endif

/* Row k of a group into `row` (160 colours) from the packed upper and lower
 * lines; rows 4-7 use the table of row 7 - k with the lines swapped. */
static void smooth_blend(uint32_t *row, const uint16_t *upper, const uint16_t *lower, unsigned k) {
    const uint32_t *pairs = meta_smooth_pairs[k < 4 ? k : 7u - k];
    if (k >= 4) {
        const uint16_t *t = upper;
        upper = lower;
        lower = t;
    }
#if META_SMOOTH_ASM
    meta_smooth_blend_asm(row, upper, lower, pairs, 0);
#else
    for (unsigned g = 0; g < 20; g++) {
        uint32_t u = upper[g], l = lower[g];
        for (int sh = 12; sh >= 0; sh -= 4) *row++ = pairs[((u >> sh) & 15u) | ((l >> sh) & 15u) << 4];
    }
#endif
}

void gbl_platform_line_end(uint8_t ly) {
    if (display_mode == META_MODE_SMOOTH) {
        unsigned j = ly - DIV9(ly) * 9u;
        const uint16_t *lower = smooth_lines[smooth_cur];
        if (smooth_row != 0xFF) {
            /* Without the upper line, the lower one stands for both. */
            const uint16_t *upper = smooth_prev_ok ? smooth_lines[smooth_cur ^ 1u] : lower;
            smooth_blend((uint32_t *)strip_slot(smooth_row), upper, lower, j - 1u);
            if (++strip_fill == META_STRIP_LINES) meta_display_flush();
        }
        smooth_cur ^= 1u; /* this line is the upper one of the next row */
        smooth_prev_ok = 1;
        return;
    }
    uint8_t kind = line_kind[ly];
    if (kind == L_HOLD) return; /* completed by the merged line */
#if META_MERGE_MODES
    if (display_mode == META_MODE_TBLEND && kind == L_MERGE) {
        uint32_t *row = (uint32_t *)strip_slot(line_row[ly]);
        const uint16_t *upper = tb_hold[TB_PAIR(ly - 1u)];
#if META_SMOOTH_ASM
        meta_smooth_blend_asm(row, upper, tb_lower, meta_half_pairs, 0);
#else
        for (unsigned g = 0; g < 20; g++) {
            uint32_t u = upper[g], l = tb_lower[g];
            for (int sh = 12; sh >= 0; sh -= 4) *row++ = meta_half_pairs[((u >> sh) & 15u) | ((l >> sh) & 15u) << 4];
        }
#endif
        if (++strip_fill == META_STRIP_LINES) meta_display_flush();
        return;
    }
#endif
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
