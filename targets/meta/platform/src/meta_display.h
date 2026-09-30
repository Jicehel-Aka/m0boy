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
    META_MODE_TILE,  /* default: drop line 7 of each tile row (gm0), keep 63 and 95 */
    META_MODE_BLEND, /* merge lines 8 and 9 of every nine, averaged colours */
    META_MODE_DARK,  /* same merge, darker shade wins */
    META_MODE_CROP,  /* no scaling: DMG lines 8..135 */
    META_MODE_SKIP,  /* drop the ninth line of every nine */
    META_MODE_COUNT
};

extern const uint16_t meta_dmg_palette[4]; /* byte-swapped RGB565 */

void meta_display_set_mode(uint8_t mode); /* call at VBlank */
uint8_t meta_display_mode(void);
void meta_display_flush(void);            /* send the partial strip */

extern uint16_t meta_display_lcd_waits;    /* renders that waited for a buffer */
extern uint16_t meta_display_rows_skipped; /* rows left unchanged on screen */

#ifdef __cplusplus
}
#endif

#endif
