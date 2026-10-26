#include "md_full.h"
#include <stdint.h>

static uint16_t vram_word(const MdFull *m, unsigned a) {
    return (uint16_t)(m->vram[a & 0xffffu] << 8 | m->vram[(a + 1u) & 0xffffu]);
}
static int16_t vsram_word(const MdFull *m, unsigned a) {
    a %= 80u;
    return (int16_t)(m->vsram[a] << 8 | m->vsram[(a + 1u) % 80u]);
}
static uint32_t colour(const MdFull *m, unsigned index) {
    unsigned c = m->cram[index & 63u];
    unsigned r = (c & 14u) >> 1, g = (c >> 5) & 7u, b = (c >> 9) & 7u;
    return 0xff000000u | ((r * 255u / 7u) << 16) |
        ((g * 255u / 7u) << 8) | (b * 255u / 7u);
}
static unsigned pattern_pixel(const MdFull *m, unsigned tile, unsigned x, unsigned y) {
    unsigned a = ((tile & 0x7ffu) * 32u + y * 4u + (x >> 1)) & 0xffffu;
    return (x & 1u) ? m->vram[a] & 15u : m->vram[a] >> 4;
}
static unsigned plane_pixel(const MdFull *m, unsigned plane, unsigned x, unsigned y,
                            unsigned *priority) {
    unsigned wcode = m->vdp_reg[16] & 3u, hcode = (m->vdp_reg[16] >> 4) & 3u;
    unsigned width = 32u << (wcode == 2 ? 0 : wcode);
    unsigned height = 32u << (hcode == 2 ? 0 : hcode);
    unsigned scroll_mode = m->vdp_reg[11] & 3u;
    unsigned hrow = scroll_mode == 3 ? y : scroll_mode == 2 ? y & ~7u : 0;
    unsigned hbase = (m->vdp_reg[13] & 0x3fu) << 10;
    int16_t hs = (int16_t)vram_word(m, hbase + hrow * 4u + plane * 2u);
    unsigned vs_index = m->vdp_reg[11] & 4u ? ((x >> 4) * 4u + plane * 2u) : plane * 2u;
    int16_t vs = vsram_word(m, vs_index);
    unsigned px = (x - hs) & (width * 8u - 1u);
    unsigned py = (y + vs) & (height * 8u - 1u);
    unsigned base = plane ? (m->vdp_reg[4] & 7u) << 13 : (m->vdp_reg[2] & 0x38u) << 10;
    unsigned entry = vram_word(m, base + (((py >> 3) * width + (px >> 3)) << 1));
    unsigned row = entry & 0x1000u ? 7u - (py & 7u) : py & 7u;
    unsigned col = entry & 0x0800u ? 7u - (px & 7u) : px & 7u;
    *priority = !!(entry & 0x8000u);
    unsigned nibble = pattern_pixel(m, entry, col, row);
    return nibble ? (((entry >> 9) & 0x30u) | nibble) : 0;
}
/* Sprites of line y into idx/pri (0 = none), first in link order wins.
 * Per line at most 20 sprites and 320 pixels in H40 (16 and 256 in H32);
 * a sprite at x = 0 masks the later ones once an earlier one is on the line. */
static void sprite_line(const MdFull *m, unsigned y, unsigned width, uint8_t *idx, uint8_t *pri) {
    const unsigned base = (m->vdp_reg[5] & (width == 320u ? 0x7eu : 0x7fu)) << 9;
    const unsigned max_sprites = width == 320u ? 80u : 64u, line_sprites = width == 320u ? 20u : 16u;
    unsigned link = 0, found = 0, pixels = 0;
    int masked = 0;
    for (unsigned i = 0; i < max_sprites; ++i) {
        const unsigned a = base + link * 8u;
        const unsigned sy = vram_word(m, a) & 0x1ffu, shape = vram_word(m, a + 2u);
        const unsigned tile = vram_word(m, a + 4u), sx = vram_word(m, a + 6u) & 0x1ffu;
        const unsigned w = (((shape >> 10) & 3u) + 1u) * 8u, h = (((shape >> 8) & 3u) + 1u) * 8u;
        const int dy = (int)y - (int)sy + 128;
        if (dy >= 0 && (unsigned)dy < h) {
            if (++found > line_sprites) break;
            if (sx == 0u && found > 1u) masked = 1;
            for (unsigned dx = 0; dx < w && !masked; ++dx) {
                if (++pixels > width) break;
                const int x = (int)sx - 128 + (int)dx;
                if (x < 0 || x >= (int)width || idx[x]) continue;
                const unsigned col = tile & 0x0800u ? w - 1u - dx : dx;
                const unsigned row = tile & 0x1000u ? h - 1u - (unsigned)dy : (unsigned)dy;
                const unsigned pattern = (tile & 0x7ffu) + (col >> 3) * (h >> 3) + (row >> 3);
                const unsigned nibble = pattern_pixel(m, pattern, col & 7u, row & 7u);
                if (nibble) { idx[x] = (uint8_t)(((tile >> 9) & 0x30u) | nibble); pri[x] = !!(tile & 0x8000u); }
            }
            if (masked || pixels > width) break;
        }
        link = shape & 0x7fu;
        if (!link) break;
    }
}

void md_full_render_line(const MdFull *m, unsigned y, uint32_t *row) {
    const unsigned width = m->vdp_reg[12] & 1u ? 320u : 256u;
    uint32_t palette[64];
    for (unsigned i = 0; i < 64; ++i) palette[i] = colour(m, i);
    const uint32_t backdrop = palette[m->vdp_reg[7] & 63u];
    if (!(m->vdp_reg[1] & 0x40u)) { for (unsigned x = 0; x < 320; ++x) row[x] = backdrop; return; }
    uint8_t sprite[320] = {0}, sprite_pri[320] = {0};
    sprite_line(m, y, width, sprite, sprite_pri);
    for (unsigned x = 0; x < 320; ++x) {
        unsigned index = m->vdp_reg[7] & 63u;
        if (x < width) {
            unsigned pa = 0, pb = 0;
            const unsigned a = plane_pixel(m, 0, x, y, &pa), b = plane_pixel(m, 1, x, y, &pb);
            const unsigned s = sprite[x], ps = sprite_pri[x];
            if (b && !pb) index = b;
            if (a && !pa) index = a;
            if (s && !ps) index = s;
            if (b && pb) index = b;
            if (a && pa) index = a;
            if (s && ps) index = s;
        }
        row[x] = index == (m->vdp_reg[7] & 63u) ? backdrop : palette[index];
    }
}

void md_full_render(const MdFull *m, uint32_t *pixels, unsigned stride) {
    for (unsigned y = 0; y < 224; ++y) md_full_render_line(m, y, pixels + y * stride);
}
