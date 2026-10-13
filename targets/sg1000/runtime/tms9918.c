/* TMS9918A, one line at a time.
 *
 * vdp_line runs once per source line: sprite status (fifth sprite,
 * coincidence) for every line the display shows, then the pixels.  Device
 * output is SG_OUT_WIDTH RGB565 pixels taken straight from VRAM (Graphics I
 * and II: 5 of the 8 pixels of each tile, the nearest-neighbour 256 -> 160
 * columns); text and multicolour modes, and the host's reference image, go
 * through the 256-pixel colour-index renderer. */
#include "sg1000.h"
#include <string.h>

typedef struct {
    int16_t left;
    uint16_t bits;  /* pattern row, bit 15 = leftmost pattern pixel */
    uint8_t color, width, shift;
} sprite_t;

/* Device pixels come in words of two (low half first, little endian).
 * color_pairs[c][k]: the word for colour byte c, k bit 0/1 = first/second
 * pixel in the foreground colour (colour 0 = backdrop), rebuilt when the
 * backdrop or the palette changes.
 * tile_pairs[pattern]: indices into it for the sampled pixels 7 6 4 3 1:
 * first tile of a pair, words 76 and 43 (bits 0-1, 2-3), pixel 1 as 0 or 3
 * (bits 4-5); second tile, pixel 7 as 0 or 3 (bits 6-7), words 64 and 31
 * (bits 8-9, 10-11). */
#if !SG_LOW_RAM
static uint32_t color_pairs[256][4];
static uint16_t pairs_palette[16];
static uint8_t pairs_ready;
static void color_pairs_build(const uint16_t *pal, uint16_t bd) {
    for (unsigned c = 0; c < 256; c++) {
        uint32_t fg = (c >> 4) ? pal[c >> 4] : bd, bg = (c & 15) ? pal[c & 15] : bd;
        color_pairs[c][0] = bg | bg << 16;
        color_pairs[c][1] = fg | bg << 16;
        color_pairs[c][2] = bg | fg << 16;
        color_pairs[c][3] = fg | fg << 16;
    }
    for (unsigned i = 1; i < 16; i++) pairs_palette[i] = pal[i];
    pairs_palette[0] = bd;
    pairs_ready = 1;
}
static const uint16_t tile_pairs[256] = {
    0x000, 0x000, 0x830, 0x830, 0x000, 0x000, 0x830, 0x830, 0x408, 0x408, 0xc38, 0xc38,
    0x408, 0x408, 0xc38, 0xc38, 0x204, 0x204, 0xa34, 0xa34, 0x204, 0x204, 0xa34, 0xa34,
    0x60c, 0x60c, 0xe3c, 0xe3c, 0x60c, 0x60c, 0xe3c, 0xe3c, 0x000, 0x000, 0x830, 0x830,
    0x000, 0x000, 0x830, 0x830, 0x408, 0x408, 0xc38, 0xc38, 0x408, 0x408, 0xc38, 0xc38,
    0x204, 0x204, 0xa34, 0xa34, 0x204, 0x204, 0xa34, 0xa34, 0x60c, 0x60c, 0xe3c, 0xe3c,
    0x60c, 0x60c, 0xe3c, 0xe3c, 0x102, 0x102, 0x932, 0x932, 0x102, 0x102, 0x932, 0x932,
    0x50a, 0x50a, 0xd3a, 0xd3a, 0x50a, 0x50a, 0xd3a, 0xd3a, 0x306, 0x306, 0xb36, 0xb36,
    0x306, 0x306, 0xb36, 0xb36, 0x70e, 0x70e, 0xf3e, 0xf3e, 0x70e, 0x70e, 0xf3e, 0xf3e,
    0x102, 0x102, 0x932, 0x932, 0x102, 0x102, 0x932, 0x932, 0x50a, 0x50a, 0xd3a, 0xd3a,
    0x50a, 0x50a, 0xd3a, 0xd3a, 0x306, 0x306, 0xb36, 0xb36, 0x306, 0x306, 0xb36, 0xb36,
    0x70e, 0x70e, 0xf3e, 0xf3e, 0x70e, 0x70e, 0xf3e, 0xf3e, 0x0c1, 0x0c1, 0x8f1, 0x8f1,
    0x0c1, 0x0c1, 0x8f1, 0x8f1, 0x4c9, 0x4c9, 0xcf9, 0xcf9, 0x4c9, 0x4c9, 0xcf9, 0xcf9,
    0x2c5, 0x2c5, 0xaf5, 0xaf5, 0x2c5, 0x2c5, 0xaf5, 0xaf5, 0x6cd, 0x6cd, 0xefd, 0xefd,
    0x6cd, 0x6cd, 0xefd, 0xefd, 0x0c1, 0x0c1, 0x8f1, 0x8f1, 0x0c1, 0x0c1, 0x8f1, 0x8f1,
    0x4c9, 0x4c9, 0xcf9, 0xcf9, 0x4c9, 0x4c9, 0xcf9, 0xcf9, 0x2c5, 0x2c5, 0xaf5, 0xaf5,
    0x2c5, 0x2c5, 0xaf5, 0xaf5, 0x6cd, 0x6cd, 0xefd, 0xefd, 0x6cd, 0x6cd, 0xefd, 0xefd,
    0x1c3, 0x1c3, 0x9f3, 0x9f3, 0x1c3, 0x1c3, 0x9f3, 0x9f3, 0x5cb, 0x5cb, 0xdfb, 0xdfb,
    0x5cb, 0x5cb, 0xdfb, 0xdfb, 0x3c7, 0x3c7, 0xbf7, 0xbf7, 0x3c7, 0x3c7, 0xbf7, 0xbf7,
    0x7cf, 0x7cf, 0xfff, 0xfff, 0x7cf, 0x7cf, 0xfff, 0xfff, 0x1c3, 0x1c3, 0x9f3, 0x9f3,
    0x1c3, 0x1c3, 0x9f3, 0x9f3, 0x5cb, 0x5cb, 0xdfb, 0xdfb, 0x5cb, 0x5cb, 0xdfb, 0xdfb,
    0x3c7, 0x3c7, 0xbf7, 0xbf7, 0x3c7, 0x3c7, 0xbf7, 0xbf7, 0x7cf, 0x7cf, 0xfff, 0xfff,
    0x7cf, 0x7cf, 0xfff, 0xfff,
};

#endif

/* Device column of each source pixel inside a tile (0xff: not sampled). */
static const uint8_t out_column[8] = { 0, 1, 0xff, 2, 3, 0xff, 4, 0xff };

/* Sprites covering each line, as a mask of sprite numbers, rebuilt when a
 * sprite Y, the terminator, the size or the table moves (sprites_dirty). */
static uint32_t line_mask[SG_HEIGHT];

static void build_masks(void) {
    sg1000_t *m = &machine;
    for (unsigned y = 0; y < SG_HEIGHT; y++) line_mask[y] = 0;
    const uint8_t *table = m->vram + m->sat_base;
    unsigned span = ((m->vdp[1] & 2) ? 16u : 8u) << (m->vdp[1] & 1);
    for (unsigned i = 0; i < 32; i++) {
        unsigned sy = table[i * 4];
        if (sy == 208) break;
        int top = (int)sy + 1;
        if (sy > 208) top -= 256;
        int first = top < 0 ? 0 : top, last = top + (int)span;
        if (last > SG_HEIGHT) last = SG_HEIGHT;
        for (int y = first; y < last; y++) line_mask[y] |= 1u << i;
    }
    m->sprites_dirty = 0;
}

/* The first four sprites on line y (colour 0 ones count but are dropped);
 * with status, also the fifth-sprite flag and number. */
static unsigned line_sprites(unsigned y, sprite_t *out, int status) {
    sg1000_t *m = &machine;
    if (m->sprites_dirty) build_masks();
    uint32_t mask = line_mask[y];
    if (!mask) return 0;
    const uint8_t *vram = m->vram;
    unsigned base = (m->vdp[6] & 7u) << 11;
    unsigned size = (m->vdp[1] & 2) ? 16 : 8, shift = m->vdp[1] & 1;
    unsigned count = 0, n = 0;
    for (unsigned i = 0; mask; i++, mask >>= 1) {
        while (!(mask & 0xff)) { mask >>= 8; i += 8; }
        if (!(mask & 1)) continue;
        const uint8_t *e = vram + m->sat_base + i * 4;
        if (++count == 5) {
            if (status && !(m->vdp_status & 64)) m->vdp_status = (uint8_t)((m->vdp_status & 0xa0) | 64 | i);
            break;
        }
        unsigned attr = e[3], color = attr & 15;
        if (!color) continue;
        int top = (int)e[0] + 1;
        if (e[0] > 208) top -= 256;
        unsigned prow = (unsigned)((int)y - top) >> shift;
        unsigned tile = e[2];
        if (size == 16) tile &= ~3u;
        unsigned a = base + tile * 8 + (prow & 7) + (prow >= 8 ? 8 : 0);
        sprite_t *s = &out[n++];
        s->left = (int16_t)((int)e[1] - ((attr & 128) ? 32 : 0));
        s->bits = (uint16_t)((vram[a] << 8) | (size == 16 ? vram[a + 16] : 0));
        s->color = (uint8_t)color;
        s->width = (uint8_t)size;
        s->shift = (uint8_t)shift;
    }
    return n;
}

/* Coincidence: two non-transparent sprite pixels on the same screen pixel.
 * Rows as 32-bit masks (bit 31 = leftmost pixel), 256 pixels in 8 words. */
static uint32_t sprite_row_mask(const sprite_t *s) {
    uint32_t m = (uint32_t)s->bits << 16;
    if (s->shift) { /* magnified: each pattern pixel twice */
        uint32_t w = 0;
        for (unsigned b = 0; b < s->width; b++)
            if (s->bits & (0x8000u >> b)) w |= 0xc0000000u >> (2 * b);
        m = w;
    }
    return m;
}
static void collisions(const sprite_t *s, unsigned n) {
    if (n < 2) return;
    uint32_t used[9] = { 0 }; /* used[8]: pixels past the right edge */
    for (unsigned k = 0; k < n; k++) {
        uint32_t m = sprite_row_mask(&s[k]);
        int x = s[k].left;
        if (x < 0) { if (x <= -32) continue; m <<= -x; x = 0; }
        unsigned w = (unsigned)x >> 5, off = (unsigned)x & 31;
        uint32_t a = m >> off, b = off ? m << (32 - off) : 0;
        if ((used[w] & a) || (w < 7 && (used[w + 1] & b))) { machine.vdp_status |= 32; return; }
        used[w] |= a;
        if (w < 7) used[w + 1] |= b;
    }
}

/* Reference renderer: 256 colour indices, sprites with first-wins priority. */
static void ref_line(unsigned y, uint8_t *pixels, const sprite_t *spr, unsigned n) {
    sg1000_t *m = &machine;
    const uint8_t *vram = m->vram;
    unsigned backdrop = m->vdp[7] & 15;
    if (!(m->vdp[1] & 64)) { memset(pixels, (int)backdrop, SG_WIDTH); return; }
    unsigned text = !!(m->vdp[1] & 16), multi = !!(m->vdp[1] & 8), graphics2 = !!(m->vdp[0] & 2);
    unsigned name = (m->vdp[2] & 15u) << 10, pattern = (m->vdp[4] & 7u) << 11, color = (unsigned)m->vdp[3] << 6;
    if (text) {
        memset(pixels, (int)backdrop, 8);
        memset(pixels + 248, (int)backdrop, 8);
    }
    unsigned columns = text ? 40 : 32, width = text ? 6 : 8;
    unsigned rowname = name + (y / 8) * columns;
    uint8_t *dest = pixels + (text ? 8 : 0);
    for (unsigned tx = 0; tx < columns; tx++) {
        unsigned tile = vram[(rowname + tx) & 0x3fff], bits = 0, colors;
        if (text) {
            bits = vram[(pattern + tile * 8 + (y & 7)) & 0x3fff];
            colors = m->vdp[7];
        } else if (multi) {
            colors = vram[(pattern + tile * 8 + ((y & 31) >> 2)) & 0x3fff];
            unsigned hi = colors >> 4, lo = colors & 15;
            memset(dest, (int)(hi ? hi : backdrop), 4);
            memset(dest + 4, (int)(lo ? lo : backdrop), 4);
            dest += 8;
            continue;
        } else {
            unsigned offset = tile * 8 + (y & 7);
            if (graphics2) {
                offset |= (y & 0xc0) << 5;
                unsigned pmask = ((m->vdp[4] & 3u) << 11) | 0x7ff, cmask = ((m->vdp[3] & 127u) << 6) | 63;
                bits = vram[((m->vdp[4] & 4u) << 11) | (offset & pmask)];
                colors = vram[((m->vdp[3] & 128u) << 6) | (offset & cmask)];
            } else {
                bits = vram[(pattern + offset) & 0x3fff];
                colors = vram[(color + (tile >> 3)) & 0x3fff];
            }
        }
        unsigned hi = colors >> 4, lo = text ? backdrop : colors & 15;
        if (!hi && !text) hi = backdrop;
        if (!lo) lo = backdrop;
        for (unsigned xx = 0; xx < width; xx++) { *dest++ = (uint8_t)((bits & 128) ? hi : lo); bits <<= 1; }
    }
    if (text) return;
    for (unsigned k = n; k-- > 0;) /* lower numbers drawn last: they win */
        for (unsigned b = 0; b < spr[k].width; b++) {
            if (!(spr[k].bits & (0x8000u >> b))) continue;
            for (unsigned z = 0; z <= spr[k].shift; z++) {
                int x = spr[k].left + (int)((b << spr[k].shift) + z);
                if (x >= 0 && x < SG_WIDTH) pixels[x] = spr[k].color;
            }
        }
}

/* Device renderer: SG_OUT_WIDTH RGB565 pixels. */
static void out_line(unsigned y, uint16_t *out, const sprite_t *spr, unsigned n) {
    sg1000_t *m = &machine;
    const uint16_t *pal = m->palette;
    const uint8_t *vram = m->vram;
    uint16_t bd = pal[m->vdp[7] & 15];
    if (!(m->vdp[1] & 64)) {
        for (unsigned i = 0; i < SG_OUT_WIDTH; i++) out[i] = bd;
        return;
    }
    if (m->vdp[1] & 24) { /* text or multicolour: sample the reference line */
        uint8_t px[SG_WIDTH];
        ref_line(y, px, spr, n);
        for (unsigned i = 0, x = 0; i < SG_OUT_WIDTH; x += 8) {
            out[i++] = pal[px[x]]; out[i++] = pal[px[x + 1]]; out[i++] = pal[px[x + 3]];
            out[i++] = pal[px[x + 4]]; out[i++] = pal[px[x + 6]];
        }
        return;
    }
#if SG_LOW_RAM
    /* One tile at a time: its 5 sampled pixels, foreground or background. */
    const uint8_t *names = vram + (((m->vdp[2] & 15u) << 10) + (y >> 3) * 32);
    const unsigned g2 = m->vdp[0] & 2;
    const uint8_t *pt = vram + (g2 ? (m->vdp[4] & 4u) << 11 : ((m->vdp[4] & 7u) << 11) + (y & 7));
    const uint8_t *ct = vram + (g2 ? (m->vdp[3] & 128u) << 6 : (unsigned)m->vdp[3] << 6);
    const unsigned pmask = ((m->vdp[4] & 3u) << 11) | 0x7ff, cmask = ((m->vdp[3] & 127u) << 6) | 63;
    const unsigned third = ((y & 0xc0) << 5) | (y & 7);
    uint16_t *o = out;
    for (unsigned tx = 0; tx < 32; tx++, o += 5) {
        unsigned t = names[tx], bits, colors;
        if (g2) { unsigned off = t * 8u | third; bits = pt[off & pmask]; colors = ct[off & cmask]; }
        else { bits = pt[t * 8]; colors = ct[t >> 3]; }
        uint16_t fg = (colors >> 4) ? pal[colors >> 4] : bd, bg = (colors & 15) ? pal[colors & 15] : bd;
        o[0] = (bits & 0x80) ? fg : bg; o[1] = (bits & 0x40) ? fg : bg; o[2] = (bits & 0x10) ? fg : bg;
        o[3] = (bits & 0x08) ? fg : bg; o[4] = (bits & 0x02) ? fg : bg;
    }
#else
    /* Two tiles give 10 device pixels: 5 aligned words from color_pairs. */
    int stale = !pairs_ready || pairs_palette[0] != bd;
    for (unsigned i = 1; i < 16 && !stale; i++) stale = pairs_palette[i] != pal[i];
    if (stale) color_pairs_build(pal, bd);
    const uint8_t *names = vram + (((m->vdp[2] & 15u) << 10) + (y >> 3) * 32);
    uint32_t *o = (uint32_t *)out;
    if (m->vdp[0] & 2) { /* Graphics II: three pattern/colour thirds, masked */
        const uint8_t *pt = vram + ((m->vdp[4] & 4u) << 11), *ct = vram + ((m->vdp[3] & 128u) << 6);
        const unsigned pmask = ((m->vdp[4] & 3u) << 11) | 0x7ff, cmask = ((m->vdp[3] & 127u) << 6) | 63;
        const unsigned third = ((y & 0xc0) << 5) | (y & 7);
        for (unsigned tx = 0; tx < 32; tx += 2, o += 5) {
            unsigned oa = names[tx] * 8u | third, ob = names[tx + 1] * 8u | third;
            const uint32_t *a = color_pairs[ct[oa & cmask]], *b = color_pairs[ct[ob & cmask]];
            unsigned ka = tile_pairs[pt[oa & pmask]], kb = tile_pairs[pt[ob & pmask]];
            o[0] = a[ka & 3];
            o[1] = a[(ka >> 2) & 3];
            o[2] = (a[(ka >> 4) & 3] & 0xffffu) | (b[(kb >> 6) & 3] & 0xffff0000u);
            o[3] = b[(kb >> 8) & 3];
            o[4] = b[(kb >> 10) & 3];
        }
    } else {
        const uint8_t *pt = vram + ((m->vdp[4] & 7u) << 11) + (y & 7), *ct = vram + ((unsigned)m->vdp[3] << 6);
        for (unsigned tx = 0; tx < 32; tx += 2, o += 5) {
            unsigned ta = names[tx], tb = names[tx + 1];
            const uint32_t *a = color_pairs[ct[ta >> 3]], *b = color_pairs[ct[tb >> 3]];
            unsigned ka = tile_pairs[pt[ta * 8]], kb = tile_pairs[pt[tb * 8]];
            o[0] = a[ka & 3];
            o[1] = a[(ka >> 2) & 3];
            o[2] = (a[(ka >> 4) & 3] & 0xffffu) | (b[(kb >> 6) & 3] & 0xffff0000u);
            o[3] = b[(kb >> 8) & 3];
            o[4] = b[(kb >> 10) & 3];
        }
    }
#endif
    for (unsigned k = n; k-- > 0;) {
        uint16_t c = pal[spr[k].color];
        for (unsigned b = 0; b < spr[k].width; b++) {
            if (!(spr[k].bits & (0x8000u >> b))) continue;
            for (unsigned z = 0; z <= spr[k].shift; z++) {
                int x = spr[k].left + (int)((b << spr[k].shift) + z);
                if (x < 0 || x >= SG_WIDTH) continue;
                unsigned col = out_column[x & 7];
                if (col != 0xff) out[(unsigned)(x >> 3) * 5 + col] = c;
            }
        }
    }
}

static void vdp_line(unsigned y) {
    sg1000_t *m = &machine;
    sprite_t spr[4];
    unsigned n = 0;
    if ((m->vdp[1] & 64) && !(m->vdp[1] & 16)) { /* sprites: display on, not text */
        n = line_sprites(y, spr, 1);
        collisions(spr, n);
        if (!(m->vdp_status & 64)) m->vdp_status = (uint8_t)((m->vdp_status & 0xe0) | 31);
    }
    if (m->row && m->row_out && (!m->draw_line || m->draw_line(m->platform, y))) {
        out_line(y, m->row_out, spr, n);
        m->row(m->platform, y, m->row_out);
    }
    if (m->ref_row) {
        uint8_t px[SG_WIDTH];
        ref_line(y, px, spr, n);
        m->ref_row(m->platform, y, px);
    }
}

void sg1000_vdp_catchup(void) {
    sg1000_t *m = &machine;
    while (m->render_line < SG_HEIGHT && (int32_t)(m->cyc - m->render_at) >= 0) {
        unsigned y = m->render_line++;
        m->render_at += SG_LINE_CYCLES;
        vdp_line(y);
    }
}

void sg1000_render_line(unsigned y, uint8_t *pixels) {
    sprite_t spr[4];
    unsigned n = (machine.vdp[1] & 64) && !(machine.vdp[1] & 16) ? line_sprites(y, spr, 0) : 0;
    ref_line(y, pixels, spr, n);
}

void sg1000_render_out(unsigned y, uint16_t *out) {
    sprite_t spr[4];
    unsigned n = (machine.vdp[1] & 64) && !(machine.vdp[1] & 16) ? line_sprites(y, spr, 0) : 0;
    out_line(y, out, spr, n);
}
