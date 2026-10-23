/* Screen-shrink primitives shared by the consoles' runtimes.
 *
 * Downscales in the tree, and where they live:
 *   256 -> 160   the 5-in-8 box filter, two methods (m0_box58_exact,
 *                m0_box58_fast): the SG-1000's TMS9918A (tms9918.c) and the
 *                Game Gear/Master System VDP's mode 4 and TMS9918 modes
 *                (gg_vdp.c) both send five device pixels per eight screen
 *                pixels, each the box of the source pixels it covers;
 *   160 -> 128   nearest neighbour, m0_nearest (the ESPboy's ST7735 adapter,
 *                espboy_hw.cpp, whatever the core that feeds it);
 *   vertical     each frontend's line policy (meta_display.c's modes,
 *                gg_display.h's tables, sg_main.cpp's display_row); where
 *                two source lines land on one row they are blended with
 *                m0_box58_pair (lines summed before the filter) or
 *                m0_avg2_panel (two rendered rows).
 *
 * The device renderer and the reference renderer of a console must agree
 * pixel for pixel, so both go through these: a console picks its method
 * once and every path of that console takes it.
 *
 * Colours blend as spread RGB565: the three channels carried apart in one
 * 32-bit word, weighted sums added in parallel, rounded, folded back.
 */
#ifndef M0_SCALE_H
#define M0_SCALE_H
#include <stdint.h>

/* Panel byte order: 1 when the LCD takes the high byte first (REV16); the
 * glass's BGR wiring is corrected by the panel's MADCTL, so the values stay
 * native RGB565.  The user defines it from its own option before including. */
#ifndef M0_PANEL_SWAP
#define M0_PANEL_SWAP 0
#endif
static inline uint16_t m0_panel(uint32_t c) {
#if M0_PANEL_SWAP
    return __builtin_bswap16((uint16_t)c);
#else
    return (uint16_t)c;
#endif
}

/* spread a RGB565 colour for parallel channel arithmetic, and fold a sum of
 * eighths (m0_fold565) or sixteenths (m0_fold16_565, two lines summed)
 * back, rounded, to panel order */
static inline uint32_t m0_spread565(uint32_t c) { return (c | (c << 16)) & 0x07e0f81fu; }
static inline uint16_t m0_fold565(uint32_t s) {
    s = ((s + 0x00802004u) >> 3) & 0x07e0f81fu;
    return (uint16_t)(s | (s >> 16));
}
static inline uint16_t m0_fold16_565(uint32_t s) {
    s = ((s + 0x01004008u) >> 4) & 0x07e0f81fu;
    return (uint16_t)(s | (s >> 16));
}
/* (a + b) / 2 per channel, rounded down: spread colours in, panel order out */
static inline uint16_t m0_blend2_565(uint32_t a, uint32_t b) {
    const uint32_t s = ((a + b) >> 1) & 0x07e0f81fu;
    return m0_panel((uint16_t)(s | (s >> 16)));
}
/* the same of two panel-order colours (native order for the channel mask,
 * then back) */
static inline uint16_t m0_avg2_panel(uint16_t a, uint16_t b) {
    a = m0_panel(a); b = m0_panel(b);
    return m0_panel((uint16_t)((a & b) + (((a ^ b) & 0xf7deu) >> 1)));
}

/* Five output pixels of eight source ones, in eighths:
 *   exact: each output pixel is the box of the source pixels it covers,
 *          (5,3) (2,5,1) (4,4) (1,5,2) (3,5);
 *   fast:  the same without its smallest weights, (5,3) (3,5) (4,4) (5,3)
 *          (3,5) with the middle rounded down, so a tile row of one palette
 *          is table lookups of pixel pairs (gg_vdp.c's pair53).
 * s: eight spread colours; o: five panel-order pixels.  m0_box58_pair takes
 * the two lines of a row already summed (weights in sixteenths). */
static inline void m0_box58_exact(const uint32_t *s, uint16_t *o) {
    o[0] = m0_panel(m0_fold565(5 * s[0] + 3 * s[1]));
    o[1] = m0_panel(m0_fold565(2 * s[1] + 5 * s[2] + s[3]));
    o[2] = m0_panel(m0_fold565(4 * s[3] + 4 * s[4]));
    o[3] = m0_panel(m0_fold565(s[4] + 5 * s[5] + 2 * s[6]));
    o[4] = m0_panel(m0_fold565(3 * s[6] + 5 * s[7]));
}
static inline void m0_box58_pair(const uint32_t *s, uint16_t *o) {
    o[0] = m0_panel(m0_fold16_565(5 * s[0] + 3 * s[1]));
    o[1] = m0_panel(m0_fold16_565(2 * s[1] + 5 * s[2] + s[3]));
    o[2] = m0_panel(m0_fold16_565(4 * s[3] + 4 * s[4]));
    o[3] = m0_panel(m0_fold16_565(s[4] + 5 * s[5] + 2 * s[6]));
    o[4] = m0_panel(m0_fold16_565(3 * s[6] + 5 * s[7]));
}
static inline void m0_box58_fast(const uint32_t *s, uint16_t *o) {
    o[0] = m0_panel(m0_fold565(5 * s[0] + 3 * s[1]));
    o[1] = m0_panel(m0_fold565(3 * s[1] + 5 * s[2]));
    o[2] = m0_blend2_565(s[3], s[4]);
    o[3] = m0_panel(m0_fold565(5 * s[5] + 3 * s[6]));
    o[4] = m0_panel(m0_fold565(3 * s[6] + 5 * s[7]));
}

/* The source pixel nearest the centre of destination pixel x (a fraction of
 * src over dst: 160 -> 128 gives (x * 5 + 2) / 4). */
static inline unsigned m0_nearest(unsigned x, unsigned src, unsigned dst) {
    return (x * src + (dst >> 1)) / dst;
}

#endif
