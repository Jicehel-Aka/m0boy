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
static unsigned sprite_pixel(const MdFull *m, unsigned x, unsigned y, unsigned *priority) {
    unsigned base = (m->vdp_reg[5] & 0x7eu) << 9;
    unsigned link = 0;
    for (unsigned i = 0; i < 80; ++i) {
        unsigned a = base + link * 8u;
        unsigned sy = vram_word(m, a) & 0x1ffu;
        unsigned shape = vram_word(m, a + 2u);
        unsigned tile = vram_word(m, a + 4u);
        unsigned sx = vram_word(m, a + 6u) & 0x1ffu;
        unsigned w = (((shape >> 10) & 3u) + 1u) * 8u;
        unsigned h = (((shape >> 8) & 3u) + 1u) * 8u;
        int dx = (int)x - (int)sx + 128, dy = (int)y - (int)sy + 128;
        if (dx >= 0 && dy >= 0 && (unsigned)dx < w && (unsigned)dy < h) {
            unsigned col = tile & 0x0800u ? w - 1u - (unsigned)dx : (unsigned)dx;
            unsigned row = tile & 0x1000u ? h - 1u - (unsigned)dy : (unsigned)dy;
            unsigned pattern = (tile & 0x7ffu) + (col >> 3) * (h >> 3) + (row >> 3);
            unsigned nibble = pattern_pixel(m, pattern, col & 7u, row & 7u);
            if (nibble) { *priority = !!(tile & 0x8000u); return ((tile >> 9) & 0x30u) | nibble; }
        }
        link = shape & 0x7fu;
        if (!link) break;
    }
    return 0;
}

void md_full_render(const MdFull *m, uint32_t *pixels, unsigned stride) {
    unsigned width = m->vdp_reg[12] & 1u ? 320u : 256u;
    unsigned backdrop = m->vdp_reg[7] & 63u;
    for (unsigned y = 0; y < 224; ++y) {
        for (unsigned x = 0; x < 320; ++x) {
            unsigned index = backdrop;
            if (x < width && (m->vdp_reg[1] & 0x40u)) {
                unsigned pa = 0, pb = 0, ps = 0;
                unsigned a = plane_pixel(m, 0, x, y, &pa);
                unsigned b = plane_pixel(m, 1, x, y, &pb);
                unsigned s = sprite_pixel(m, x, y, &ps);
                if (b && !pb) index = b;
                if (a && !pa) index = a;
                if (s && !ps) index = s;
                if (b && pb) index = b;
                if (a && pa) index = a;
                if (s && ps) index = s;
            }
            pixels[y * stride + x] = colour(m, index);
        }
    }
}
