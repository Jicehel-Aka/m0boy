/*
 * The option menu's palettes, shared by the consoles' display pipelines
 * (meta_display.h).
 *
 * Palette 0 is the default and depends on the game: when gbrecomp found
 * the title in the Game Boy Color boot ROM's compatibility table it
 * carries that palette in gbl_config.h (GBL_PALETTE_GBC, resolved at
 * conversion time — the boot ROM's own colouring of DMG games, four
 * colours), and it comes first in the menu; otherwise palette 0 is the
 * DMG green.  The selection itself lives in the converter: the firmware
 * never carries the boot ROM's tables, only the one palette it plays.
 */
#include "meta_display.h"

#ifdef GBL_PALETTE_GBC
const uint32_t meta_palettes[META_PALETTE_COUNT][4] = {
    {GBL_PALETTE_GBC},                        /* the boot ROM's, for this game */
    {0xE0F8D0, 0x88C070, 0x346856, 0x081820}, /* green (DMG-like) */
    {0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000}, /* grey */
    {0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F}, /* Pocket */
    {0xF8E8C8, 0xD8A060, 0x985020, 0x301808}, /* amber */
    {0xE0F0FF, 0x80A8E0, 0x3050A0, 0x101830}, /* blue */
};
const char *const meta_palette_names[META_PALETTE_COUNT] = {
    "GBC", "Vert", "Gris", "Pocket", "Ambre", "Bleu",
};
#else
/* Lightest shade first (0xRRGGBB). */
const uint32_t meta_palettes[META_PALETTE_COUNT][4] = {
    {0xE0F8D0, 0x88C070, 0x346856, 0x081820}, /* green (DMG-like) */
    {0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000}, /* grey */
    {0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F}, /* Pocket */
    {0xF8E8C8, 0xD8A060, 0x985020, 0x301808}, /* amber */
    {0xE0F0FF, 0x80A8E0, 0x3050A0, 0x101830}, /* blue */
};
const char *const meta_palette_names[META_PALETTE_COUNT] = {"Vert", "Gris", "Pocket", "Ambre", "Bleu"};
#endif
