/*
 * meta_lcd_pack444 (the reference of meta_lcd_pack_asm.S) against the
 * ST7735's RGB444 layout (datasheet 9.7.20): 0x0RGB pixels go out as
 * R0G0 B0R1 G1B1 ..., packed in place without touching what follows.
 */
#include "meta_lcd_pack.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    static uint16_t row[160 + 4];
    static uint8_t want[240];
    uint32_t rng = 7;
    int failures = 0;
    for (int round = 0; round < 1000; round++) {
        for (unsigned i = 0; i < 164; i++) { rng = rng * 1103515245u + 12345u; row[i] = (uint16_t)((rng >> 16) & 0xFFF); }
        for (unsigned x = 0; x < 160; x += 2) {
            unsigned a = row[x], b = row[x + 1];
            want[x / 2 * 3] = (uint8_t)(a >> 4);
            want[x / 2 * 3 + 1] = (uint8_t)((a & 15) << 4 | b >> 8);
            want[x / 2 * 3 + 2] = (uint8_t)b;
        }
        uint16_t tail[4];
        memcpy(tail, row + 160, sizeof tail);
        meta_lcd_pack444((uint8_t *)row, 160);
        if (memcmp(row, want, 240) || memcmp(row + 160, tail, sizeof tail))
            if (failures++ < 3) printf("round %d: packed row differs\n", round);
    }
    printf("lcd_pack: %s\n", failures ? "MISMATCH" : "RGB444 rows exact");
    return failures ? 1 : 0;
}
