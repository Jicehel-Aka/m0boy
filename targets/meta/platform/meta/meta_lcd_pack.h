/*
 * ST7735 12-bit colour (COLMOD 03h, RGB 4-4-4: two pixels in three bytes,
 * datasheet 9.7.19-20).  With META_LCD_12BPP the display pipeline renders
 * rows of 0x0RGB halfwords (META_COLOR); each row is packed in place just
 * before it is sent: 240 bytes on the SPI instead of 320.
 *
 * meta_lcd_pack444 is the reference (host tests, tests/lcd_pack.c); on the
 * META meta_lcd_pack444_asm (meta_lcd_pack_asm.S) does eight pixels per
 * iteration, about 5 cycles per pixel.
 */
#ifndef META_LCD_PACK_H
#define META_LCD_PACK_H

#include <stdint.h>

/* `pixels` 0x0RGB halfwords at p (an even count) become R0G0 B0R1 G1B1 ...
 * bytes at p.  The writes never overtake the reads. */
static inline void meta_lcd_pack444(uint8_t *p, unsigned pixels) {
    const uint16_t *s = (const uint16_t *)p;
    for (unsigned i = 0; i < pixels; i += 2, p += 3) {
        unsigned a = s[i] & 0xFFFu, b = s[i + 1] & 0xFFFu;
        p[0] = (uint8_t)(a >> 4);
        p[1] = (uint8_t)((a << 4) | (b >> 8));
        p[2] = (uint8_t)b;
    }
}

#if defined(__ARM_ARCH_6M__)
/* p 4-byte aligned, pixels a multiple of 8. */
#ifdef __cplusplus
extern "C"
#endif
void meta_lcd_pack444_asm(uint8_t *p, unsigned pixels);
#endif

#endif
