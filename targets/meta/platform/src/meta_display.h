/*
 * META display pipeline: maps the DMG's 144 scanlines onto the ST7735's 128
 * rows, skips rows whose inputs did not change, and hands strips of rows to
 * the LCD driver.  Plain C with no Arduino dependency, so the host tests run
 * exactly this code against a simulated panel.
 *
 * Implements gbl_platform_line_check / line_begin / line_end.
 */
#ifndef META_DISPLAY_H
#define META_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    META_MODE_SMOOTH, /* every row is two DMG lines weighted by area (9 -> 8) */
    META_MODE_TILE,  /* drop line 7 of each tile row (gm0), keep 63 and 95 */
    META_MODE_BLEND, /* merge lines 8 and 9 of every nine, averaged colours */
    META_MODE_DARK,  /* same merge, darker shade wins */
    META_MODE_CROP,  /* no scaling: DMG lines 8..135 */
    META_MODE_SKIP,  /* drop the ninth line of every nine */
    META_MODE_TBLEND, /* default: TILE's rows, line 7 of each tile row merged into the next (gm0 ESPboy) */
    META_MODE_COUNT
};

#define META_PALETTE_COUNT 5
extern const uint32_t meta_palettes[META_PALETTE_COUNT][4]; /* 0xRRGGBB, lightest first */
extern const char *const meta_palette_names[META_PALETTE_COUNT];
extern uint16_t meta_dmg_palette[4]; /* current palette, byte-swapped RGB565 */
/* Palette of every mode but SMOOTH (compile-time tables, first palette). */
void meta_display_set_palette(uint8_t index);
uint8_t meta_display_palette(void);
/* The strip buffers, free while emulation is stopped (the option menu):
 * flushes and returns 160-pixel rows of scratch, usable once the display
 * is idle (meta_lcd_wait). */
uint16_t *meta_display_scratch(unsigned *rows);

void meta_display_set_mode(uint8_t mode); /* call at VBlank */
uint8_t meta_display_mode(void);
void meta_display_flush(void);            /* send the partial strip */

extern uint16_t meta_display_lcd_waits;    /* renders that waited for a buffer */
extern uint16_t meta_display_rows_skipped; /* rows left unchanged on screen */

#ifdef __cplusplus
}
#endif

#endif
