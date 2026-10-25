/* The 2C02: registers $2000-$2007, the loopy v/t scroll model, background
 * and sprite rendering per scanline, sprite-0 hit (Super Mario Bros'
 * status-bar split depends on it).  The machine runs the CPU a scanline at
 * a time, then renders the line; mid-frame register writes apply from the
 * next line, which is where SMB changes them anyway.
 *
 * Two renderers share the register model:
 *   the panel renderer (every build): colour indices per line, scaled to
 *     160x128 rows in panel byte order (ppu.c, "the panel picture");
 *   the reference renderer (NES_PPU_REFERENCE, the host harness): the
 *     256-pixel RGB565 line the panel picture is checked against. */
#ifndef NES_PPU_H
#define NES_PPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    /* state the program reaches (the registers and the loopy pair) */
    uint8_t ctrl, mask, status, oam_addr;
    uint16_t v, t;
    uint8_t x, w;
    uint8_t buffer; /* $2007's read-ahead */
    uint8_t open_bus;
    /* memory the PPU owns or borrows */
    uint8_t *nametable;  /* 2 KiB, mirrored per the cartridge */
    uint8_t *palette;    /* 32 bytes */
    uint8_t *oam;        /* 256 bytes (the machine's, DMA'd) */
    const uint8_t *chr;  /* 8 KiB CHR (ROM on NROM) */
    unsigned vertical_mirror;
    uint8_t oam_dirty;   /* OAM or the sprite size changed: the per-line
                          * sprite masks are rebuilt before the next use
                          * (the machine's OAM DMA sets it too) */
    /* the panel picture: 240 lines -> rows of 160 pixels, panel_plan's
     * (below).  Each row is written to panel_out, then panel(host, row) is
     * called; it may point panel_out elsewhere for the next row.  skip:
     * this frame is not shown (only v, the flags and sprite-0 run). */
    uint8_t skip;
    uint16_t panel_plan;
    uint16_t *panel_out;
    void (*panel)(void *host, unsigned row);
#ifdef NES_PPU_REFERENCE
    /* the reference line: the fetched stream, 264 pixels — screen pixel c
     * is row[c + x] — then render_row(host, y) */
    uint16_t row[264];
    void (*render_row)(void *, unsigned y);
    uint32_t hit_mismatch; /* lines where the two renderers' sprite-0 differ */
#endif
    uint16_t frame;
    void *host;
} nes_ppu_t;

void nes_ppu_reset(nes_ppu_t *ppu);
uint8_t nes_ppu_read(nes_ppu_t *ppu, unsigned reg);
void nes_ppu_write(nes_ppu_t *ppu, unsigned reg, uint8_t value);
/* One scanline: sets vblank/sprite0 at their lines, renders y < 240 when
 * enabled, advances v; returns 1 when an NMI must be raised. */
int nes_ppu_line(nes_ppu_t *ppu, unsigned line);

/* The panel's vertical plan: the 240 lines in 16 groups of 15, bit k of
 * panel_plan set when line k of a group closes a row; a row is one line or
 * two blended (no two clear bits in a row, bit 14 set).  16 rows per set
 * bit:
 *   NES_PLAN_128  seven pairs then one line alone, 15 -> 8 (the META's
 *                 160x128, nes_init's default);
 *   NES_PLAN_176  [0] [1] [2,3] [4] [5] [6,7] [8] [9] [10,11] [12] [13,14],
 *                 15 -> 11 (the Pokitto's 176 rows).
 * Horizontally, m0_box58_fast: 160 pixels. */
#define NES_PANEL_W 160u
#define NES_PLAN_128 0x6AAAu
#define NES_PLAN_176 0x5BBBu
#define NES_PANEL_ROWS(plan) (16u * (unsigned)__builtin_popcount(plan))

#endif


#ifdef __cplusplus
}
#endif
