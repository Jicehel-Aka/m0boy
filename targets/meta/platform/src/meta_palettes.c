/*
 * The option menu's palettes, shared by the consoles' display pipelines
 * (meta_display.h).
 */
#include "meta_display.h"

/* Lightest shade first (0xRRGGBB). */
const uint32_t meta_palettes[META_PALETTE_COUNT][4] = {
    {0xE0F8D0, 0x88C070, 0x346856, 0x081820}, /* green (DMG-like) */
    {0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000}, /* grey */
    {0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F}, /* Pocket */
    {0xF8E8C8, 0xD8A060, 0x985020, 0x301808}, /* amber */
    {0xE0F0FF, 0x80A8E0, 0x3050A0, 0x101830}, /* blue */
};
const char *const meta_palette_names[META_PALETTE_COUNT] = {"Vert", "Gris", "Pocket", "Ambre", "Bleu"};
