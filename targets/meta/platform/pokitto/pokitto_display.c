/*
 * Pokitto display pipeline (meta_display.h): the 160x144 picture fits the
 * 220x176 panel, so every DMG line is one panel row, written as soon as the
 * PPU has rendered it.  Lines whose inputs did not change since they were
 * last drawn are neither rendered nor written (GBL_LINE_SIGNATURE), as on
 * the META.
 */
#include "meta_display.h"

#include <string.h>

#include "gbl.h"
#include "gbl_config.h"
#include "gbl_options.h"
#include "meta_lcd.h"
#include "meta_target.h"
#include "pokitto_hw.h"

#ifndef META_LINE_SKIP
#define META_LINE_SKIP 1
#endif

const uint32_t meta_palettes[META_PALETTE_COUNT][4] = {
    {0xE0F8D0, 0x88C070, 0x346856, 0x081820}, /* green (DMG-like) */
    {0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000}, /* grey */
    {0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F}, /* Pocket */
    {0xF8E8C8, 0xD8A060, 0x985020, 0x301808}, /* amber */
    {0xE0F0FF, 0x80A8E0, 0x3050A0, 0x101830}, /* blue */
};
const char *const meta_palette_names[META_PALETTE_COUNT] = {"Vert", "Gris", "Pocket", "Ambre", "Bleu"};
uint16_t meta_dmg_palette[4] = {
    META_COLOR(0xE0F8D0), META_COLOR(0x88C070), META_COLOR(0x346856), META_COLOR(0x081820),
};
static uint8_t palette_index;

uint16_t meta_display_lcd_waits;
uint16_t meta_display_rows_skipped;

/* Eight rows for the menu's bands and save states' sector buffer; row 0 is
 * also the line the PPU renders into (never at the same time). */
static uint16_t scratch[160 * 8] __attribute__((aligned(4)));

#if META_LINE_SKIP
/* In the 2 KiB RAM1 bank: main RAM is the stack's. */
static uint32_t line_sig[144] __attribute__((section(".bss_ram1.line_sig")));
static uint8_t line_valid[144] __attribute__((section(".bss_ram1.line_valid")));
#endif

uint8_t meta_display_mode(void) { return 0; }

void meta_display_set_mode(uint8_t mode) {
    (void)mode;
    for (unsigned i = 0; i < 4; i++) gbl_lcd_palette[i] = meta_dmg_palette[i];
    gbl_lcd_palette_primary = 1;
    gbl_ppu_packed = 0;
    gbl_ppu_palette_changed();
    gbl_frame_invalidate();
#if META_LINE_SKIP
    memset(line_valid, 0, sizeof(line_valid));
#endif
}

uint8_t meta_display_palette(void) { return palette_index; }

void meta_display_set_palette(uint8_t index) {
    if (index >= META_PALETTE_COUNT) index = 0;
    palette_index = index;
    for (unsigned i = 0; i < 4; i++) meta_dmg_palette[i] = META_COLOR(meta_palettes[index][i]);
    meta_display_set_mode(0); /* the PPU's colours, a full redraw */
}

uint16_t *meta_display_scratch(unsigned *rows) {
    *rows = 8;
    return scratch;
}

void meta_display_flush(void) {}

int gbl_platform_line_check(uint8_t ly, uint32_t sig) {
    if (ly >= 144) return 0;
#if META_LINE_SKIP
    if (line_valid[ly] && line_sig[ly] == sig) {
        meta_display_rows_skipped++;
        return 0;
    }
    line_sig[ly] = sig;
    line_valid[ly] = 1;
#else
    (void)sig;
#endif
    return 1;
}

uint16_t *gbl_platform_line_begin(uint8_t ly) { return ly < 144 ? scratch : 0; }

void gbl_platform_line_end(uint8_t ly) { pokitto_lcd_row(ly, scratch); }
