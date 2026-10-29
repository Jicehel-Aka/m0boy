#ifndef M0BOY_M0_NATIVE_KERNELS_H
#define M0BOY_M0_NATIVE_KERNELS_H
/* Hand-written replacements for hot guest loops (chosen by profile).  The
 * generator calls one at the head of the loop it replaces, only when the ROM
 * holds exactly the instructions it was written for.  A kernel runs as many
 * whole iterations as it can and leaves registers, cycles and store counts
 * exactly as the translated loop would; the translated loop then runs the
 * remainder (and its exit), so a kernel only declines (returns 0) when
 * something is unusual: an address outside SRAM/flash, overlapping buffers,
 * not enough room before the next deadline.  Cycle costs follow m0_cpu.c. */
#include "m0_cpu.h"
#include "m0_machine.h"

/* host pointer to a guest range that lies wholly in SRAM (or, read-only, in flash) */
static inline uint8_t *m0k_ptr(M0Cpu *m, uint32_t a, uint32_t len, int writable) {
    M0Machine *b = (M0Machine *)m->board;
    uint32_t o = a - b->ram_base;
    if (o <= M0_MACHINE_RAM_BYTES && len <= M0_MACHINE_RAM_BYTES - o) return b->ram + o;
    if (!writable) {
        o = a - b->flash_base;
        if (o <= b->flash_size && len <= b->flash_size - o) return (uint8_t *)b->flash + o;
    }
    return 0;
}
static inline int m0k_word(M0Cpu *m, uint32_t a, uint32_t *v) {
    const uint8_t *p = (a & 3u) ? 0 : m0k_ptr(m, a, 4, 0);
    if (!p) return 0;
    memcpy(v, p, 4);
    return 1;
}
static inline int m0k_overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
    return a < b + blen && b < a + alen;
}

/* 0xae08: for (i = r3; i < [r7+0x24]; ++i) { h = src[base + i]; dst[2*i] = dst[2*i+1] = bswap16(h); }
 * (the game's line doubling: every source pixel twice, bytes swapped for the LCD).  25 cycles a turn. */
static inline int m0k_ae08(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) {
    uint32_t *r = m->r, count, base, obj, src, i = r[3];
    if (!m0k_word(m, r[7] + 0x24u, &count) || !m0k_word(m, r[7] + 0x2cu, &base) ||
        !m0k_word(m, r[7] + 0x28u, &obj) || !m0k_word(m, obj + 0x10u, &src)) return 0;
    if (i >= count || count > 0xffffu) return 0;                 /* also: negative or odd counts go the long way */
    uint32_t n = count - i, left = room - *c;
    if (left < bound + 25u) return 0;
    if (n > (left - bound) / 25u) n = (left - bound) / 25u;
    const uint32_t from = src + ((base + i) << 1), to = r[5] + (i << 2);
    if ((from & 1u) || (to & 1u)) return 0;
    const uint8_t *s = m0k_ptr(m, from, n << 1, 0);
    uint8_t *d = m0k_ptr(m, to, n << 2, 1);
    if (!s || !d || m0k_overlap(from, n << 1, to, n << 2)) return 0;
    /* the loop's own variables must stay untouched by its stores */
    if (m0k_overlap(to, n << 2, r[7] + 0x24u, 4) || m0k_overlap(to, n << 2, r[7] + 0x28u, 4) ||
        m0k_overlap(to, n << 2, r[7] + 0x2cu, 4) || m0k_overlap(to, n << 2, obj + 0x10u, 4)) return 0;
    uint32_t h = 0;
    if (!(to & 3u)) {
        for (uint32_t k = 0; k < n; ++k) {
            uint16_t x; memcpy(&x, s + (k << 1), 2);
            h = (uint32_t)(uint16_t)((x << 8) | (x >> 8));
            const uint32_t w = h | (h << 16);
            memcpy(d + (k << 2), &w, 4);
        }
    } else {
        for (uint32_t k = 0; k < n; ++k) {
            uint16_t x; memcpy(&x, s + (k << 1), 2);
            const uint16_t y = (uint16_t)((x << 8) | (x >> 8));
            h = y;
            memcpy(d + (k << 2), &y, 2);
            memcpy(d + (k << 2) + 2, &y, 2);
        }
    }
    const uint32_t last = i + n - 1u;
    r[0] = r[5] + (last << 2); r[1] = h; r[2] = last << 2; r[3] = last + 1u;
    *c += 25u * n;
    m->stores += (r[5] + (i << 2) >= m->dead_below) ? 2u * n : 0u;
    return 1;
}
#endif
