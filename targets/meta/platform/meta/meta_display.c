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
 * is blended by SMOOTH's loop with a 1/2 table.  META_TBLEND 2 keeps one held
 * line only, in SMOOTH's buffers: it is rendered on every drawn frame, and
 * the picture is the same.  With META_LINE_SKIP a row whose
 * inputs did not change since it was drawn is neither rendered nor sent;
 * strips therefore hold runs of consecutive rows, each sent to its own
 * window.
 */
#include "meta_display.h"

#include "gbl.h"
#include "gbl_config.h"
#include "gbl_options.h"
#include "meta_lcd.h"
#include "meta_target.h"

#include <string.h>

/* Switches (values in gbl_profile.h):
 * META_STRIP_LINES       rows per strip; META_LCD_BUFFERS strips in flight
 *                        (with META_LCD_DMA the DMA interrupt chains queued
 *                        strips, so rendering only waits when all are busy)
 * META_SMOOTH_ASM        the SMOOTH/TBLEND row blend in Thumb-1
 *                        (meta_display_asm.S)
 * META_SMOOTH_SPECULATE  SMOOTH: render again on the next frame the lines
 *                        that changed, so that a row whose lower line keeps
 *                        moving finds its upper line rendered
 * META_MERGE_MODES       BLEND and DARK (1 KiB table, a spare line); TBLEND
 *                        blends two pixels per lookup in a 1 KiB table
 * META_TBLEND            TBLEND: 1 keeps the 15 held lines (672 bytes: the
 *                        held lines and, without META_MERGE_MODES, a
 *                        16-colour table looked up once per pixel instead of
 *                        the 1 KiB one); 2 one held line in SMOOTH's buffers
 *                        (32 bytes), rendered again on every drawn frame
 * META_DISPLAY_COMPACT   flash layouts and bitmap line-valid flags (0 restores RAM arrays)
 * META_LINE_SKIP         rows whose inputs did not change since they were
 *                        drawn are neither rendered nor sent (exact: see
 *                        GBL_LINE_SIGNATURE)
 * In builds with a ROM sector cache or cartridge RAM on the card the
 * profile keeps two strips and leaves out the merge modes (META_STREAM_RAM).
 */
#if META_SMOOTH_ASM && !defined(__ARM_ARCH_6M__)
#undef META_SMOOTH_ASM
#define META_SMOOTH_ASM 0
#endif
#if !defined(META_STRIP_LINES) || !defined(META_LCD_BUFFERS) || !defined(META_SMOOTH_ASM) || \
    !defined(META_SMOOTH_SPECULATE) || !defined(META_MERGE_MODES) || !defined(META_LINE_SKIP) || \
    !defined(META_TBLEND) || !defined(META_DISPLAY_COMPACT)
#error "gbl_profile.h: a META display switch is missing"
#endif
#if META_TBLEND < 0 || META_TBLEND > 2
#error "META_TBLEND must be 0, 1 or 2"
#endif
#if META_STRIP_LINES < 1 || META_STRIP_LINES > 128 || META_LCD_BUFFERS < 1
#error "META_STRIP_LINES must be 1..128 and META_LCD_BUFFERS must be positive"
#endif


/* The palettes themselves (meta_palettes) are in meta_palettes.c; SMOOTH's
 * colour tables are generated at compile time from the first one. */
static uint8_t palette_index;
uint16_t meta_dmg_palette[4] = {
    META_COLOR(0xE0F8D0), META_COLOR(0x88C070),
    META_COLOR(0x346856), META_COLOR(0x081820),
};
#if META_MERGE_MODES || META_TBLEND
static uint32_t dmg_rgb[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};
#endif

uint16_t meta_display_lcd_waits;
uint16_t meta_display_rows_skipped;

static uint16_t strip[META_LCD_BUFFERS][160 * META_STRIP_LINES] __attribute__((aligned(4)));
static uint32_t strip_seq[META_LCD_BUFFERS]; /* transfer that last used each buffer */
static uint8_t strip_cur, strip_fill, strip_row0;

static uint8_t display_mode = META_MODE_TILE; /* until meta_display_set_mode */
#if META_MERGE_MODES || META_TBLEND
/* Buffers of one mode at a time (meta_display_set_mode rebuilds them):
 * BLEND and DARK keep their merged line and table, TBLEND the held lines. */
static union {
#if META_MERGE_MODES
    struct {
        uint16_t spare_line[160];
        uint32_t merge2[256]; /* a0 | a1 << 2 | b0 << 4 | b1 << 6 -> two merged pixels */
    } m;
#endif
#if META_TBLEND
    struct {
#if META_TBLEND == 1
        /* Line 8k+7 of each merged pair, packed, kept across frames so that
         * it is rendered only when it changes; the line merged into it.
         * META_TBLEND 2 uses smooth_lines for both. */
        uint16_t hold[15][20];
        uint16_t lower[20];
#endif
#if META_MERGE_MODES
        /* Two merged pixels per entry, in the layout of meta_smooth_pairs
         * (built from the palette by meta_display_set_mode). */
        uint32_t half[256];
#else
        /* One merged pixel per entry: upper shade | lower shade << 2. */
        uint16_t half1[16];
#endif
    } t;
#endif
} mode_ram __attribute__((aligned(4)));
#define spare_line mode_ram.m.spare_line
#define merge2 mode_ram.m.merge2
#if META_TBLEND == 1
#define tb_lower mode_ram.t.lower
#define TB_HOLD(ly) mode_ram.t.hold[TB_PAIR(ly)]
#else
#define tb_lower smooth_lines[1]
#define TB_HOLD(ly) smooth_lines[0]
#endif
#define tb_half mode_ram.t.half
#define tb_half1 mode_ram.t.half1
#endif
static uint8_t index_palette; /* PPU currently emits shade indices */

#if META_LINE_SKIP
/* Signature of what each line last put on screen, by line (the modes that
 * keep whole lines index it like their rows).  A mode change voids them
 * all, so the mode needs no place in the signature. */
static uint32_t line_sig[144];
#if META_DISPLAY_COMPACT
static uint8_t line_valid[18];
#define LINE_VALID(ly) ((line_valid[(ly) >> 3] >> ((ly) & 7)) & 1u)
#define LINE_MARK(ly) (line_valid[(ly) >> 3] |= (uint8_t)(1u << ((ly) & 7)))
#else
static uint8_t line_valid[144];
#define LINE_VALID(ly) line_valid[ly]
#define LINE_MARK(ly) (line_valid[ly] = 1)
#endif
static uint32_t pending_sig; /* merge modes: signature of the held line 7 */
#endif

/* SMOOTH colours, generated at compile time: row k of a group takes
 * (8 - k)/9 of shade a (upper line) and (k + 1)/9 of shade b (lower line),
 * rounded per channel, in the panel's format (META_COLOR).  Row 7 - k is row k with the
 * lines swapped, so four tables of two-pixel entries cover the eight rows.
 * The index is two 4-bit groups of the packed lines (gbl_ppu_packed: left
 * pixel in bits 3:2): upper | lower << 4; the entry holds the left pixel in
 * its low half (4 KiB of flash).  tests/display_sim.c checks them against
 * the formula. */
#define DMG_CH(s, sh) (((s) == 0 ? 0xE0F8D0u : (s) == 1 ? 0x88C070u : (s) == 2 ? 0x346856u : 0x081820u) >> (sh) & 255u)
#define SM_CH(k, a, b, sh) (((8u - (k)) * DMG_CH(a, sh) + ((k) + 1u) * DMG_CH(b, sh) + 4u) / 9u)
#define SM_RGB(k, a, b) (SM_CH(k, a, b, 16) << 16 | SM_CH(k, a, b, 8) << 8 | SM_CH(k, a, b, 0))
#define SM_COL(k, a, b) ((uint32_t)META_COLOR(SM_RGB(k, a, b)))
#define SM_E(k, i) (SM_COL(k, ((i) >> 2) & 3u, ((i) >> 6) & 3u) | SM_COL(k, (i) & 3u, ((i) >> 4) & 3u) << 16)
#define SM_E4(k, i) SM_E(k, i), SM_E(k, (i) + 1u), SM_E(k, (i) + 2u), SM_E(k, (i) + 3u)
#define SM_E16(k, i) SM_E4(k, i), SM_E4(k, (i) + 4u), SM_E4(k, (i) + 8u), SM_E4(k, (i) + 12u)
#define SM_E64(k, i) SM_E16(k, i), SM_E16(k, (i) + 16u), SM_E16(k, (i) + 32u), SM_E16(k, (i) + 48u)
#define SM_E256(k) {SM_E64(k, 0u), SM_E64(k, 64u), SM_E64(k, 128u), SM_E64(k, 192u)}
const uint32_t meta_smooth_pairs[4][256] = {SM_E256(0u), SM_E256(1u), SM_E256(2u), SM_E256(3u)};

#if META_TBLEND

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

#if META_MERGE_MODES || META_TBLEND
/* Shades a and b averaged per channel, rounded down. */
static uint16_t average_color(unsigned a, unsigned b) {
    uint32_t x = dmg_rgb[a], y = dmg_rgb[b];
    uint32_t avg = (((x >> 16) + (y >> 16)) / 2) << 16 |
                   ((((x >> 8) & 255) + ((y >> 8) & 255)) / 2) << 8 |
                   (((x & 255) + (y & 255)) / 2);
    return META_COLOR(avg);
}
#endif

#if META_MERGE_MODES
static uint16_t merged_color(unsigned a, unsigned b) {
    if (display_mode == META_MODE_DARK) return meta_dmg_palette[a > b ? a : b];
    return average_color(a, b);
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
#endif
#if !META_TBLEND
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
#if META_TBLEND
    if (display_mode == META_MODE_TBLEND) {
#if META_MERGE_MODES
        for (unsigned k = 0; k < 256; k++) /* packed index: a0 a1 a2 a3 | b0 b1 b2 b3 << 4 */
            tb_half[k] = (uint32_t)average_color((k >> 2) & 3, (k >> 6) & 3) |
                         ((uint32_t)average_color(k & 3, (k >> 4) & 3) << 16);
#else
        for (unsigned k = 0; k < 16; k++) tb_half1[k] = average_color(k & 3, k >> 2);
#endif
    }
#endif
}

uint8_t meta_display_palette(void) { return palette_index; }

void meta_display_set_palette(uint8_t index) {
    if (index >= META_PALETTE_COUNT) index = 0;
    palette_index = index;
    meta_display_set_palette_colors(meta_palettes[index]);
}

void meta_display_set_palette_colors(const uint32_t pal[4]) {
    for (unsigned i = 0; i < 4; i++) {
        uint32_t rgb = pal[i];
        meta_dmg_palette[i] = META_COLOR(rgb);
#if META_MERGE_MODES || META_TBLEND
        dmg_rgb[i] = rgb;
#endif
    }
    meta_display_set_mode(display_mode); /* tables, the PPU's colours, a full redraw */
}

uint16_t *meta_display_scratch(unsigned *rows) {
    meta_display_flush(); /* the caller waits for the display (meta_lcd_wait) */
    *rows = META_LCD_BUFFERS * META_STRIP_LINES;
    return strip[0];
}

/* Per-line layout of the current mode: what each DMG line becomes. */
enum { L_ROW, L_HOLD, L_MERGE, L_DROP };
#if META_DISPLAY_COMPACT
#include "meta_display_layout.h"
static const uint8_t *line_kind = display_layout[META_MODE_TILE][0];
static const uint8_t *line_row = display_layout[META_MODE_TILE][1];
#else
static uint8_t line_kind[144], line_row[144];
#endif

static void build_layout(void) {
#if META_DISPLAY_COMPACT
    line_kind = display_layout[display_mode][0];
    line_row = display_layout[display_mode][1];
#else
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
#endif
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
    unsigned changed = !LINE_VALID(ly) || line_sig[ly] != sig;
    line_sig[ly] = sig;
    LINE_MARK(ly);
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
#if META_LINE_SKIP && META_TBLEND
    if (display_mode == META_MODE_TBLEND && line_kind[ly] != L_ROW) {
        /* The held line is rendered only when it changed (its packed copy
         * stays valid); the merged row is redrawn when either line did. */
        unsigned changed = !LINE_VALID(ly) || line_sig[ly] != sig;
        line_sig[ly] = sig;
        LINE_MARK(ly);
#if META_TBLEND == 1
        if (line_kind[ly] == L_HOLD) return (int)(tb_hold_fresh = (uint8_t)changed);
#else
        /* One held line for all pairs: render it, its pair may change. */
        if (line_kind[ly] == L_HOLD) { tb_hold_fresh = (uint8_t)changed; return 1; }
#endif
        if (changed || tb_hold_fresh) return 1;
        meta_display_rows_skipped++;
        return 0;
    }
#endif
#if META_LINE_SKIP
    if (line_kind[ly] == L_HOLD) { pending_sig = sig; return 1; } /* needed if its pair changed */
    if (line_kind[ly] == L_MERGE) sig = gbl_sig_combine(pending_sig, sig);
    if (LINE_VALID(ly) && line_sig[ly] == sig) { meta_display_rows_skipped++; return 0; }
    line_sig[ly] = sig;
    LINE_MARK(ly);
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
#if META_TBLEND
    if (display_mode == META_MODE_TBLEND) {
        /* The merged pair is rendered packed, every other line as colours. */
        gbl_ppu_packed = kind != L_ROW;
        if (kind == L_HOLD) return TB_HOLD(ly);
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
#if META_TBLEND
    if (display_mode == META_MODE_TBLEND && kind == L_MERGE) {
        const uint16_t *upper = TB_HOLD(ly - 1u);
#if META_MERGE_MODES
        uint32_t *row = (uint32_t *)strip_slot(line_row[ly]);
#if META_SMOOTH_ASM
        meta_smooth_blend_asm(row, upper, tb_lower, tb_half, 0);
#else
        for (unsigned g = 0; g < 20; g++) {
            uint32_t u = upper[g], l = tb_lower[g];
            for (int sh = 12; sh >= 0; sh -= 4) *row++ = tb_half[((u >> sh) & 15u) | ((l >> sh) & 15u) << 4];
        }
#endif
#else
        /* packed groups: eight pixels, the leftmost in bits 15:14 */
        uint16_t *row = strip_slot(line_row[ly]);
        for (unsigned g = 0; g < 20; g++) {
            uint32_t u = upper[g], l = tb_lower[g];
            for (int sh = 14; sh >= 0; sh -= 2) *row++ = tb_half1[((u >> sh) & 3u) | ((l >> sh) & 3u) << 2];
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
