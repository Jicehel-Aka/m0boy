/* Line policies shared by the GG/SMS frontend and its host tests: the
 * shared vertical plan (m0_scale.h, m0_row_of and m0_row_first), as tables
 * in flash where the denominator is not a power of two (test_gg_runtime.c
 * checks them against the rule).  Inputs are source lines inside the GG or
 * SMS window. */
#ifndef GG_DISPLAY_H
#define GG_DISPLAY_H
#include "gg.h"
#include "m0_scale.h"

static inline unsigned gg_sms_display_row(unsigned y) { return m0_row_of(y, 8, 5); }
/* The nearest source line to each row centre: five selected out of eight. */
static inline int gg_sms_line_shown(unsigned y) { return (0xb5u >> (y & 7)) & 1u; }

/* META: 144 -> 128, m0_row_of(y, 9, 8): the first two lines in each group
 * of nine averaged.  No repeated software division on the Cortex-M0+. */
static const uint8_t gg_meta_rows[GG_HEIGHT] = {
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 16, 17, 18, 19, 20, 21, 22, 23, 24, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 32, 33, 34, 35, 36, 37, 38, 39, 40, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 48, 49, 50, 51, 52, 53, 54, 55, 56, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 64, 65, 66, 67, 68, 69, 70, 71, 72, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 80, 81, 82, 83, 84, 85, 86, 87, 88, 88, 89, 90, 91, 92, 93, 94, 95,
    96, 96, 97, 98, 99, 100, 101, 102, 103, 104, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 112, 113, 114, 115, 116, 117, 118, 119, 120, 120, 121, 122, 123, 124, 125, 126, 127,
};

static const uint8_t gg_meta_modes[GG_HEIGHT] = {
    3, 2, 1, 1, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1,
    3, 2, 1, 1, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1,
    3, 2, 1, 1, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1,
    3, 2, 1, 1, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1,
    3, 2, 1, 1, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1,
    3, 2, 1, 1, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1,
    3, 2, 1, 1, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1,
    3, 2, 1, 1, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1,
};

static inline unsigned gg_meta_display_row(unsigned y) { return gg_meta_rows[y - GG_TOP]; }
static inline int gg_meta_draw_mode(unsigned y) { return gg_meta_modes[y - GG_TOP]; }

/* Pokitto: 144 -> 176, line l on panel rows gg_pokitto_rows[l] up to the
 * next entry, m0_row_first(l, 9, 11), without a software division per row. */
static const uint8_t gg_pokitto_rows[GG_HEIGHT + 1] = {
    0, 2, 3, 4, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 18, 19, 20, 21,
    22, 24, 25, 26, 27, 29, 30, 31, 32, 33, 35, 36, 37, 38, 40, 41, 42, 43,
    44, 46, 47, 48, 49, 51, 52, 53, 54, 55, 57, 58, 59, 60, 62, 63, 64, 65,
    66, 68, 69, 70, 71, 73, 74, 75, 76, 77, 79, 80, 81, 82, 84, 85, 86, 87,
    88, 90, 91, 92, 93, 95, 96, 97, 98, 99, 101, 102, 103, 104, 106, 107, 108, 109,
    110, 112, 113, 114, 115, 117, 118, 119, 120, 121, 123, 124, 125, 126, 128, 129, 130, 131,
    132, 134, 135, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 150, 151, 152, 153,
    154, 156, 157, 158, 159, 161, 162, 163, 164, 165, 167, 168, 169, 170, 172, 173, 174, 175,
    176,
};
#endif
