#include "ppu.h"

/* NES master palette as RGB565: the classic 2C02 mix used by nes-py
 * (FCEUX-style), converted from its 24-bit table. */
static const uint16_t nes_colors[64] = {
    0x7BEF, 0x001F, 0x0017, 0x4157, 0x9010, 0xA804, 0xA880, 0x88A0,
    0x5180, 0x03C0, 0x0340, 0x02C0, 0x020B, 0x0000, 0x0000, 0x0000,
    0xBDF7, 0x03DF, 0x02DF, 0x6A3F, 0xD819, 0xE00B, 0xF9C0, 0xE2E2,
    0xABE0, 0x05C0, 0x0540, 0x0548, 0x0451, 0x0000, 0x0000, 0x0000,
    0xFFDF, 0x3DFF, 0x6C5F, 0x9BDF, 0xFBDF, 0xFAD3, 0xFBCB, 0xFD08,
    0xFDC0, 0xBFC3, 0x5ECA, 0x5FD3, 0x075B, 0x7BCF, 0x0000, 0x0000,
    0xFFFF, 0xA73F, 0xBDDF, 0xDDDF, 0xFDDF, 0xFD38, 0xF696, 0xFF15,
    0xFECF, 0xDFCF, 0xBFD7, 0xBFDB, 0x07FF, 0xFEDF, 0x0000, 0x0000,
};

#define CTRL_NMI 0x80
#define CTRL_SPR16 0x20
#define CTRL_BG_PAGE 0x10 /* bit 4: background pattern table */
#define CTRL_SPR_PAGE 0x08 /* bit 3: sprite pattern table */
#define CTRL_INC 0x04
#define MASK_BG 0x08
#define MASK_SPR 0x10
#define MASK_BG_LEFT 0x02
#define MASK_SPR_LEFT 0x04
#define ST_VBLANK 0x80
#define ST_SPRITE0 0x40

static int pair_init; /* the pair tables wait for the palette RAM to exist */
static void pair_write(nes_ppu_t *ppu, unsigned i);

void nes_ppu_reset(nes_ppu_t *ppu) {
    ppu->ctrl = ppu->mask = ppu->status = 0;
    ppu->v = ppu->t = 0;
    ppu->x = ppu->w = 0;
    ppu->buffer = 0;
    ppu->frame = 0;
}

/* $2000-$2FFF lands in the 2 KiB of nametable RAM per the cartridge's
 * mirroring (vertical mirroring mirrors horizontally, and conversely). */
static uint16_t mirror(nes_ppu_t *ppu, uint16_t at) {
    at &= 0x0FFF;
    if (ppu->vertical_mirror) return (uint16_t)(at & 0x07FF);
    return (uint16_t)((at & 0x03FF) | ((at & 0x0800) >> 1));
}

/* CHR ROM (NROM has no CHR RAM): both pattern tables are addressable, the
 * page bit of $2000 picks one per sprite or background fetch. */
static uint8_t chr_at(const nes_ppu_t *ppu, unsigned at) {
    return ppu->chr ? ppu->chr[at & 0x1FFF] : 0;
}

uint8_t nes_ppu_read(nes_ppu_t *ppu, unsigned reg) {
    switch (reg) {
    case 2: {
        const uint8_t out = (uint8_t)((ppu->status & (ST_VBLANK | ST_SPRITE0 | 0x20)) |
                                      (ppu->open_bus & 0x1F));
        ppu->status &= (uint8_t)~ST_VBLANK;
        ppu->w = 0;
        return out;
    }
    case 4: return ppu->oam[ppu->oam_addr];
    case 7: {
        const uint16_t at = ppu->v & 0x3FFF;
        const uint8_t out = ppu->buffer;
        if ((at & 0x3F00) == 0x3F00) {
            ppu->buffer = ppu->nametable[mirror(ppu, at & 0x2FFF)];
        } else if (at >= 0x2000) ppu->buffer = ppu->nametable[mirror(ppu, at)];
        else ppu->buffer = chr_at(ppu, at);
        ppu->v = (uint16_t)(ppu->v + ((ppu->ctrl & CTRL_INC) ? 32 : 1));
        return out;
    }
    default: return ppu->open_bus;
    }
}

void nes_ppu_write(nes_ppu_t *ppu, unsigned reg, uint8_t value) {
    ppu->open_bus = value;
    switch (reg) {
    case 0:
        ppu->ctrl = value;
        ppu->t = (uint16_t)((ppu->t & 0x73FF) | ((value & 3) << 10));
        break;
    case 1: ppu->mask = value; break;
    case 3: ppu->oam_addr = value; break;
    case 4: ppu->oam[ppu->oam_addr++] = value; break;
    case 5:
        if (!ppu->w) {
            ppu->x = value & 7;
            ppu->t = (uint16_t)((ppu->t & 0x7FE0) | (value >> 3));
            ppu->w = 1;
        } else {
            ppu->t = (uint16_t)((ppu->t & 0x0C1F) | ((value & 7) << 12) | ((value & 0xF8) << 2));
            ppu->w = 0;
        }
        break;
    case 6:
        if (!ppu->w) {
            ppu->t = (uint16_t)((ppu->t & 0x00FF) | ((value & 0x3F) << 8));
            ppu->w = 1;
        } else {
            ppu->t = (uint16_t)((ppu->t & 0x7F00) | value);
            ppu->v = ppu->t;
            ppu->w = 0;
        }
        break;
    case 7: {
        const uint16_t at = ppu->v & 0x3FFF;
        if ((at & 0x3F00) == 0x3F00) {
            uint8_t idx = at & 0x1F;
            if ((idx & 0x13) == 0x10) idx &= 0x0F; /* the mirrors of $3F10 */
            ppu->palette[idx] = value & 0x3F;
            if (pair_init) pair_write(ppu, idx);
        } else if (at >= 0x2000) ppu->nametable[mirror(ppu, at)] = value;
        /* $0000-$1FFF is CHR ROM on NROM: writes are ignored */
        ppu->v = (uint16_t)(ppu->v + ((ppu->ctrl & CTRL_INC) ? 32 : 1));
        break;
    }
    default: break;
    }
}

static unsigned palette_color(nes_ppu_t *ppu, uint8_t index) {
    return nes_colors[ppu->palette[index] & 0x3F];
}

/* The background's tile expansion: two planar pixels and the tile's palette
 * go through one 64-entry table that holds the composed colour pair and the
 * palette-space index pair (0 = backdrop, else (pal << 2) | colour, what
 * sprite priority and sprite-0 test against).  Entry c of a palette p is
 * c = lo_pair | hi_pair << 2 | p << 4.  The tables are pure functions of the
 * palette RAM, rebuilt around the entries a $2007 write touches (four if the
 * written colour is one of the mirrored ones the renderer never reads). */
static uint32_t pair_rgb[64]; /* two RGB565 pixels, low pixel first */
static uint8_t pair_idx[64]; /* two palette-space indices, low nibble first */

static void pair_build(nes_ppu_t *ppu, unsigned c) {
    const unsigned pal = c >> 4, pair = c & 15;
    /* the four bits are two pixels' plane bits: bit1 left lo, bit0 right lo,
     * bit3 left hi, bit2 right hi (the walk takes its pairs MSB first) */
    const unsigned c0 = (pair >> 3 & 1) << 1 | (pair >> 1 & 1);
    const unsigned c1 = (pair >> 2 & 1) << 1 | (pair & 1);
    const unsigned i0 = c0 ? (pal << 2) | c0 : 0;
    const unsigned i1 = c1 ? (pal << 2) | c1 : 0;
    pair_idx[c] = (uint8_t)(i0 | (i1 << 4));
    pair_rgb[c] = nes_colors[ppu->palette[i0]] | nes_colors[ppu->palette[i1]] << 16;
}

static void pair_write(nes_ppu_t *ppu, unsigned i) {
    /* i: the palette-space index $2007 wrote (its mirrors already folded).
     * The sprite palettes (i >= $10) don't feed these tables — their colours
     * are read per pixel — and 4, 8 and 12 are never read by the renderer.
     * A colour sits in the pair's left pixel (bits 1 and 3 of the key) or
     * its right pixel (bits 0 and 2). */
    const unsigned col = i & 3;
    if (i >= 0x10 || !((i & 3) || i == 0)) return;
    unsigned base = col ? (i >> 2) << 4 : 0;
    const unsigned end = col ? base + 16 : 64;
    for (; base < end; base += 16) {
        if (col) {
            for (unsigned free_bits = 0; free_bits < 4; free_bits++) {
                pair_build(ppu, base | (col & 1) << 1 | (col >> 1) << 3 |
                                    (free_bits & 1) | (free_bits >> 1) << 2);
                pair_build(ppu, base | (col & 1) | (col >> 1) << 2 |
                                    (free_bits & 1) << 1 | (free_bits >> 1) << 3);
            }
        } else { /* the backdrop is colour 0 of every palette's table */
            for (unsigned free_bits = 0; free_bits < 4; free_bits++) {
                pair_build(ppu, base | (free_bits & 1) | (free_bits >> 1) << 2);
                pair_build(ppu, base | (free_bits & 1) << 1 | (free_bits >> 1) << 3);
            }
        }
    }
}

static void pair_build_all(nes_ppu_t *ppu) {
    for (unsigned c = 0; c < 64; c++) pair_build(ppu, c);
    pair_init = 1;
}

/* one visible scanline: the background's colour indices first (0 means
 * the backdrop), the sprites over them, sprite 0's hit against them.
 * compose is false for a line the panel drops: the fetches still run (the
 * sprite-0 test needs the background indices) but the row is not built.
 * bg_pack holds the palette-space indices two to a byte for the 264 pixels
 * the fetch walk covers; the row keeps the same stream order. */
static void render_line(nes_ppu_t *ppu, unsigned line, int compose) {
    const int bg_on = (ppu->mask & MASK_BG) != 0;
    const int spr_on = (ppu->mask & MASK_SPR) != 0;
    static uint8_t bg_pack[132]; /* 264 pixels, two 4-bit indices a byte */
    const uint32_t backdrop = palette_color(ppu, 0);
    uint32_t *const row32 = (uint32_t *)ppu->row;

    if (!pair_init) pair_build_all(ppu);
    if (!bg_on) {
        if (compose) {
            const uint32_t both = backdrop | backdrop << 16;
            for (unsigned x = 0; x < 132; x++) row32[x] = both;
        }
        for (unsigned b = 0; b < 132; b++) bg_pack[b] = 0;
    } else {
        /* the walk starts at v's nametable, coarse and fine position; the 33
         * fetched tiles form the pixel stream the fine x of $2005 shifts.
         * The pointers and the mirroring are the line's own: cached once
         * here, the nametable pointer walked instead of re-derived. */
        uint8_t *const nt = ppu->nametable;
        const uint8_t *const chr = ppu->chr;
        const unsigned vm = ppu->vertical_mirror;
        uint32_t *out = row32;
        uint8_t *pack = bg_pack;
        uint16_t v = ppu->v;
        unsigned coarse_x = v & 0x1F;
        unsigned table = v & 0x0C00;
        const unsigned coarse_y = (v >> 5) & 0x1F;
        const unsigned fine_y = (v >> 12) & 7;
        const unsigned page = (ppu->ctrl & CTRL_BG_PAGE) ? 0x1000 : 0;
        for (unsigned t = 0; t < 33; t++) {
            const unsigned at = (unsigned)((0x2000 + table) | (coarse_y << 5) | coarse_x);
            const uint8_t tile = nt[vm ? at & 0x07FF : (at & 0x03FF) | ((at & 0x0800) >> 1)];
            const unsigned aa = (at & 0x2C00) | 0x03C0 | ((coarse_y & 0x1C) << 1) | ((coarse_x & 0x1C) >> 2);
            const uint8_t attr = nt[vm ? aa & 0x07FF : (aa & 0x03FF) | ((aa & 0x0800) >> 1)];
            const unsigned shift = ((coarse_x & 2) | ((coarse_y & 2) << 1));
            const unsigned c = ((attr >> shift) & 3) << 4;
            const unsigned pattern = page + tile * 16 + fine_y;
            const uint8_t lo = chr ? chr[pattern & 0x1FFF] : 0;
            const uint8_t hi = chr ? chr[(pattern + 8) & 0x1FFF] : 0;
            if (compose) {
                out[0] = pair_rgb[c | (lo >> 6 & 3) | (hi >> 6 & 3) << 2];
                out[1] = pair_rgb[c | (lo >> 4 & 3) | (hi >> 4 & 3) << 2];
                out[2] = pair_rgb[c | (lo >> 2 & 3) | (hi >> 2 & 3) << 2];
                out[3] = pair_rgb[c | (lo & 3) | (hi & 3) << 2];
            }
            pack[0] = pair_idx[c | (lo >> 6 & 3) | (hi >> 6 & 3) << 2];
            pack[1] = pair_idx[c | (lo >> 4 & 3) | (hi >> 4 & 3) << 2];
            pack[2] = pair_idx[c | (lo >> 2 & 3) | (hi >> 2 & 3) << 2];
            pack[3] = pair_idx[c | (lo & 3) | (hi & 3) << 2];
            out += 4;
            pack += 4;
            if (++coarse_x == 32) { coarse_x = 0; table ^= 0x0400; }
        }
        /* the left column may be masked off (stream pixels x..x+7) */
        if (!(ppu->mask & MASK_BG_LEFT))
            for (unsigned s = ppu->x; s < ppu->x + 8; s++) {
                bg_pack[s >> 1] &= (s & 1) ? 0x0F : 0xF0;
                if (compose) ppu->row[s] = backdrop;
            }
    }

    /* compose the sprites over the row, sprite 0's hit against the background */
    if (spr_on) {
        const unsigned height = (ppu->ctrl & CTRL_SPR16) ? 16 : 8;
        unsigned found = 0;
        for (unsigned sprite = 0; sprite < 64; sprite++) {
            const uint8_t *oam = ppu->oam + sprite * 4;
            const unsigned sy = oam[0] + 1; /* a sprite at y covers y+1.. */
            if (line < sy || line >= sy + height) continue;
            if (++found > 8) break; /* the over-the-limit drop, no overflow flag yet */
            const unsigned row = line - sy;
            const unsigned flip_v = (oam[2] & 0x80) ? height - 1 - row : row;
            unsigned pattern = oam[1] * 16 + (flip_v & 7) + (flip_v & 8 ? 16 : 0);
            if (ppu->ctrl & CTRL_SPR16)
                pattern = (uint16_t)(((oam[1] & 1) ? 0x1000 : 0) + (oam[1] & 0xFE) * 16 +
                                     (flip_v & 15));
            else if (ppu->ctrl & CTRL_SPR_PAGE) pattern += 0x1000;
            const uint8_t lo = chr_at(ppu, pattern);
            const uint8_t hi = chr_at(ppu, pattern + 8);
            const unsigned flip_h = (oam[2] & 0x40) ? 7 : 0;
            const unsigned pal = (oam[2] & 3) << 2;
            uint16_t colors[4]; /* the sprite's palette, transparent is never read */
            for (unsigned idx = 1; idx < 4; idx++)
                colors[idx] = palette_color(ppu, (uint8_t)(0x10 | pal | idx));
            const int behind = (oam[2] & 0x20) != 0;
            for (unsigned bit = 0; bit < 8; bit++) {
                const unsigned px = oam[3] + bit;
                if (px >= 256) break;
                if (!(ppu->mask & MASK_SPR_LEFT) && px < 8) continue;
                const unsigned shift_bit = flip_h ? bit : 7 - bit;
                const unsigned index = (((hi >> shift_bit) & 1) << 1) | ((lo >> shift_bit) & 1);
                if (!index) continue;
                const unsigned spx = px + ppu->x; /* the stream pixel behind it */
                const uint8_t pack = bg_pack[spx >> 1];
                const unsigned bg_index = (spx & 1) ? pack >> 4 : pack & 15;
                if (sprite == 0 && bg_index && px < 255 && bg_on)
                    ppu->status |= ST_SPRITE0; /* the status bar's split */
                if (!behind || !bg_index)
                    if (compose) ppu->row[spx] = colors[index];
            }
        }
    }
}

int nes_ppu_line(nes_ppu_t *ppu, unsigned line) {
    if (line == 241) {
        ppu->status |= ST_VBLANK;
        return (ppu->ctrl & CTRL_NMI) ? 1 : 0;
    }
    if (line == 261) {
        ppu->status &= (uint8_t)~(ST_VBLANK | ST_SPRITE0);
        /* the pre-render line reloads v from t only while rendering is on:
         * with the screen off, v keeps whatever $2006 set, which is how
         * Super Mario Bros streams a column across the frame boundary */
        if (ppu->mask & (MASK_BG | MASK_SPR)) ppu->v = ppu->t;
        ppu->frame++;
        return 0;
    }
    if (line >= 240) return 0;
    const int compose = !ppu->want_row || ppu->want_row(ppu->host, line);
    if (ppu->mask & (MASK_BG | MASK_SPR)) {
        render_line(ppu, line, compose);
        /* fine y, then coarse y with its wraps */
        if ((ppu->v & 0x7000) != 0x7000) ppu->v = (uint16_t)(ppu->v + 0x1000);
        else {
            ppu->v &= (uint16_t)~0x7000;
            unsigned y = (ppu->v >> 5) & 0x1F;
            if (y == 29) { y = 0; ppu->v ^= 0x0800; }
            else if (y == 31) y = 0;
            else y++;
            ppu->v = (uint16_t)((ppu->v & ~0x03E0) | (y << 5));
        }
        /* the horizontal bits reload from t at the line's end */
        ppu->v = (uint16_t)((ppu->v & ~0x041F) | (ppu->t & 0x041F));
    } else {
        if (compose)
            for (unsigned x = 0; x < 256; x++) ppu->row[x] = palette_color(ppu, 0);
    }
    if (compose && ppu->render_row) ppu->render_row(ppu->host, line);
    return 0;
}
