/* Direct per-pixel DMG reference renderer shared by the gbl tests. */
#ifndef DMG_REFERENCE_H
#define DMG_REFERENCE_H

#include "gbl.h"

static uint8_t tile_pixel(uint8_t lcdc, uint8_t idx, unsigned row, unsigned col) {
    unsigned base = (lcdc & 0x10) ? idx * 16u : (unsigned)(0x1000 + (int8_t)idx * 16);
    uint8_t lo = gbl_vram[base + row * 2], hi = gbl_vram[base + row * 2 + 1];
    return (uint8_t)((((hi >> (7 - col)) & 1) << 1) | ((lo >> (7 - col)) & 1));
}

static void reference(uint8_t ref[144][160]) {
    uint8_t lcdc = gbl_io[0x40], scx = gbl_io[0x43], scy = gbl_io[0x42];
    uint8_t wx = gbl_io[0x4B], wy = gbl_io[0x4A], bgp = gbl_io[0x47];
    unsigned win_line = 0;
    for (unsigned ly = 0; ly < 144; ly++) {
        uint8_t bgi[160];
        int win = (lcdc & 0x21) == 0x21 && ly >= wy && wx <= 166;
        for (int x = 0; x < 160; x++) {
            uint8_t c = 0;
            if (lcdc & 1) {
                if (win && x >= (int)wx - 7) {
                    unsigned px = (unsigned)(x - ((int)wx - 7));
                    uint16_t map = (lcdc & 0x40) ? 0x1C00 : 0x1800;
                    c = tile_pixel(lcdc, gbl_vram[map + (win_line / 8) * 32 + px / 8], win_line % 8, px % 8);
                } else {
                    unsigned bx = (unsigned)(x + scx) & 255, by = (ly + scy) & 255;
                    uint16_t map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
                    c = tile_pixel(lcdc, gbl_vram[map + (by / 8) * 32 + bx / 8], by % 8, bx % 8);
                }
            }
            bgi[x] = c;
            ref[ly][x] = (lcdc & 1) ? (uint8_t)((bgp >> (c * 2)) & 3) : 0;
        }
        if (win) win_line++;
        if (!(lcdc & 2)) continue;
        unsigned h = (lcdc & 4) ? 16 : 8;
        int sel[10], n = 0;
        for (int i = 0; i < 40 && n < 10; i++) {
            int y = gbl_oam[i * 4] - 16;
            if ((int)ly >= y && (int)ly < y + (int)h) sel[n++] = i;
        }
        for (int x = 0; x < 160; x++) {
            int best = -1;
            uint8_t bestc = 0;
            for (int k = 0; k < n; k++) {
                const uint8_t *o = gbl_oam + sel[k] * 4;
                int sx = o[1] - 8;
                if (x < sx || x >= sx + 8) continue;
                unsigned row = ly - (unsigned)(o[0] - 16);
                if (o[3] & 0x40) row = h - 1 - row;
                uint8_t t = o[2];
                if (h == 16) t &= 0xFE;
                unsigned col = (unsigned)(x - sx);
                if (o[3] & 0x20) col = 7 - col;
                uint8_t lo = gbl_vram[t * 16 + row * 2], hi = gbl_vram[t * 16 + row * 2 + 1];
                uint8_t c = (uint8_t)((((hi >> (7 - col)) & 1) << 1) | ((lo >> (7 - col)) & 1));
                if (!c) continue;
                if (best < 0 || o[1] < gbl_oam[best * 4 + 1] ||
                    (o[1] == gbl_oam[best * 4 + 1] && sel[k] < best)) {
                    best = sel[k];
                    bestc = c;
                }
            }
            if (best < 0) continue;
            const uint8_t *o = gbl_oam + best * 4;
            if ((o[3] & 0x80) && bgi[x]) continue;
            uint8_t obp = (o[3] & 0x10) ? gbl_io[0x49] : gbl_io[0x48];
            ref[ly][x] = (uint8_t)((obp >> (bestc * 2)) & 3);
        }
    }
}

#endif
