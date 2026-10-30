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
static const uint16_t __attribute__((unused)) m0_lp_copy_code[34] = {0x8810,0x3e04,0x8018,0x8850,0x8058,0x8890,0x8098,0x88d0,0x3208,0x80d8,0x3308,0xe013,0x5a17,0x521f,0x3002,0xe013,0x1949,0x42a1,0xd215,0x8803,0x3002,0x9301,0x000b,0x9a01,0x3a01,0x9201,0xd3f4,0x7846,0x1c82,0x7800,0x0040,0x181b,0x2e04,0xd8dd};
static const uint16_t __attribute__((unused)) m0_lp_mix_code[50] = {0x9806,0x0a1e,0x5636,0x9804,0x195b,0x4346,0x8820,0x1236,0x1836,0x9807,0x8026,0x4298,0xd820,0x9800,0x2800,0xd113,0x4668,0x8800,0x8610,0x19db,0x62d3,0x9b05,0x3258,0x4293,0xd1bb,0x680a,0x9b02,0x4694,0x4463,0x600b,0x9a03,0x684b,0x1a9a,0x604a,0xb009,0xbdf0,0x6a90,0x7b86,0x2002,0x4206,0xd009,0x006e,0x426d,0xb22d,0x1b9b,0x8615,0x4660,0x3402,0x42a0,0xd8cd};

/* ---- Lapinou ------------------------------------------------------------ */
/* Lapinou 0x4b18: for (; n > 4; n -= 4) { dst[0..3] = src[0..3]; src += 4; dst += 4; }  (halfwords)
 * r2 = src, r3 = dst, r6 = n.  Runs whole turns that end back at the head (the last turn, which
 * falls out of the loop, is left to the caller).  24 cycles a turn. */
static inline int m0k_lp4b18(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) {
    uint32_t *r = m->r;
    const uint32_t src = r[2], dst = r[3], n = r[6];
    if (n < 9u || n > 0xffffu || ((src | dst) & 1u)) return 0;
    uint32_t k = (n - 5u) / 4u, left = room - *c;
    if (left < bound + 24u) return 0;
    if (k > (left - bound) / 24u) k = (left - bound) / 24u;
    if (!k) return 0;
    const uint8_t *s = m0k_ptr(m, src, k << 3, 0);
    uint8_t *d = m0k_ptr(m, dst, k << 3, 1);
    if (!s || !d || m0k_overlap(src, k << 3, dst, k << 3)) return 0;
    memcpy(d, s, (size_t)k << 3);
    uint16_t last; memcpy(&last, s + (k << 3) - 2, 2);
    const uint32_t total = k << 2;
    uint32_t dead = 0;                                   /* stores below the watched loop's stack pointer do not count */
    if (m->dead_below > dst) { dead = (m->dead_below - dst + 1u) >> 1; if (dead > total) dead = total; }
    m->stores += total - dead;
    r[0] = last; r[2] = src + (k << 3); r[3] = dst + (k << 3); r[6] = n - (k << 2);
    (void)m0_native_addm(m, r[6], ~4u, 1u, 15u);          /* the flags the loop's last cmp r6,#4 leaves */
    *c += 24u * k;
    return 1;
}

/* Lapinou 0x864a: the audio mixer's inner loop,
 *   do { out[i] += (int8)wave[pos >> 8] * vol >> 8; pos += step; } while (limit > pos && end > ++out)
 * r3 = pos, r4 = out, r5 = step, ip = end, [sp+0x10] = vol, [sp+0x18] = wave, [sp+0x1c] = limit.
 * Turns are run while they end back at the head; 25 cycles a turn. */
static inline int m0k_lp864a(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) {
    uint32_t *r = m->r, vol, wave, limit;
    const uint32_t sp = r[13], end = r[12], step = r[5];
    if (!m0k_word(m, sp + 0x10u, &vol) || !m0k_word(m, sp + 0x18u, &wave) || !m0k_word(m, sp + 0x1cu, &limit)) return 0;
    uint32_t pos = r[3], out = r[4], left = room - *c, k = 0, last = 0;
    if (left < bound + 25u || (out & 1u)) return 0;
    const uint32_t kmax = (left - bound) / 25u, out0 = out;
    while (k < kmax) {
        const uint32_t npos = pos + step;
        if (!(limit > npos) || !(end > out + 2u)) break;                 /* this turn would leave the loop */
        const uint32_t wa = wave + (pos >> 8);
        const uint8_t *w = m0k_ptr(m, wa, 1, 0);
        uint8_t *o = m0k_ptr(m, out, 2, 1);
        if (!w || !o) break;
        if (m0k_overlap(out, 2, sp + 0x10u, 16) || (wa >= out0 && wa < out + 2u)) break;   /* the stores must not feed the loop */
        uint16_t old; memcpy(&old, o, 2);
        const uint32_t prod = (uint32_t)(int32_t)(int8_t)*w * vol;
        last = (uint32_t)((int32_t)prod >> 8) + old;
        const uint16_t lo = (uint16_t)last; memcpy(o, &lo, 2);
        pos = npos; out += 2u; ++k;
    }
    if (!k) return 0;
    uint32_t dead = 0;
    if (m->dead_below > out0) { dead = (m->dead_below - out0 + 1u) >> 1; if (dead > k) dead = k; }
    m->stores += k - dead;
    r[0] = end; r[3] = pos; r[4] = out; r[6] = last;
    (void)m0_native_addm(m, end, ~out, 1u, 15u);                          /* the cmp that closes the turn */
    *c += 25u * k;
    return 1;
}

/* ---- Cat's and Coins: sprite rows ---------------------------------------- */
static const uint16_t __attribute__((unused)) m0_cc_fill_code[8] = {0xb29d,0x428d,0xd204,0x8816,0x005d,0x5346,0x3301,0xe7f7};
static const uint16_t __attribute__((unused)) m0_cc_tcopy_code[15] = {0x69bc,0x42a0,0xd300,0xe1e8,0x69fc,0x6bf9,0x0045,0x5b66,0x890a,0x42b2,0xd000,0x535e,0x3001,0xb2c0,0xe7f0};

/* count of the k halfword stores from `dst` that lie at or above the watched loop's stack pointer */
static inline uint32_t m0k_live_stores(const M0Cpu *m, uint32_t dst, uint32_t k) {
    uint32_t dead = 0;
    if (m->dead_below > dst) { dead = (m->dead_below - dst + 1u) >> 1; if (dead > k) dead = k; }
    return k - dead;
}

/* Cat's 0xe1ca: for (; (uint16)r3 < r1; ++r3) *(uint16 *)(r0 + 2*r3) = *(uint16 *)r2;   (row fill with one colour)
 * 11 cycles a turn.  Leaves the head's registers as the loop would after the last full turn. */
static inline int m0k_cc_fill(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) {
    uint32_t *r = m->r;
    const uint32_t base = r[0], lim = r[1], vp = r[2], i = r[3];
    if (i >= lim || lim > 0xffffu || (base & 1u) || (vp & 1u)) return 0;
    uint32_t k = lim - i, left = room - *c;
    if (left < bound + 11u) return 0;
    if (k > (left - bound) / 11u) k = (left - bound) / 11u;
    const uint32_t dst = base + (i << 1);
    uint8_t *d = m0k_ptr(m, dst, k << 1, 1);
    const uint8_t *vptr = m0k_ptr(m, vp, 2, 0);
    if (!d || !vptr || m0k_overlap(vp, 2, dst, k << 1)) return 0;
    uint16_t v; memcpy(&v, vptr, 2);
    for (uint32_t j = 0; j < k; ++j) memcpy(d + (j << 1), &v, 2);
    m->stores += m0k_live_stores(m, dst, k);
    r[3] = i + k; r[5] = (i + k - 1u) << 1; r[6] = v;
    (void)m0_native_addm(m, i + k - 1u, 1u, 0u, 15u);         /* adds r3, #1 */
    *c += 11u * k;
    return 1;
}

/* Cat's 0xdbaa: for (; (uint8)r0 < [r7+0x18]; ++r0) { v = src[r0]; if (v != key) dst[r0] = v; }  with
 * src = [r7+0x1c], key = *(uint16 *)([r7+0x3c] + 8), dst = r3 (halfwords).  21 cycles a turn, 22 with the store. */
static inline int m0k_cc_tcopy(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) {
    uint32_t *r = m->r, width, srcp, kobj;
    if (!m0k_word(m, r[7] + 0x18u, &width) || !m0k_word(m, r[7] + 0x1cu, &srcp) || !m0k_word(m, r[7] + 0x3cu, &kobj)) return 0;
    const uint32_t i0 = r[0], dstp = r[3];
    if (i0 >= width || width > 0xffu || ((srcp | dstp) & 1u)) return 0;
    const uint8_t *kp = m0k_ptr(m, kobj + 8u, 2, 0);
    if (!kp) return 0;
    uint32_t n = width - i0, left = room - *c;
    if (left < bound + 22u) return 0;
    if (n > (left - bound) / 22u) n = (left - bound) / 22u;
    const uint8_t *s = m0k_ptr(m, srcp + (i0 << 1), n << 1, 0);
    uint8_t *d = m0k_ptr(m, dstp + (i0 << 1), n << 1, 1);
    if (!s || !d) return 0;
    const uint32_t dlo = dstp + (i0 << 1), dlen = n << 1;
    /* the stores must not touch what the loop reads: its own variables, the key, the source */
    if (m0k_overlap(dlo, dlen, r[7] + 0x18u, 0x28u) || m0k_overlap(dlo, dlen, kobj + 8u, 2) ||
        m0k_overlap(dlo, dlen, srcp + (i0 << 1), dlen)) return 0;
    uint16_t key; memcpy(&key, kp, 2);
    uint32_t cyc = 21u * n, live = 0;
    uint16_t v = 0;
    for (uint32_t j = 0; j < n; ++j) {
        memcpy(&v, s + (j << 1), 2);
        if (v != key) { memcpy(d + (j << 1), &v, 2); ++cyc; live += (dlo + (j << 1)) >= m->dead_below; }
    }
    m->stores += live;
    const uint32_t last = i0 + n - 1u;
    r[0] = last + 1u; r[1] = kobj; r[2] = key; r[4] = srcp; r[5] = last << 1; r[6] = v;
    (void)m0_native_addm(m, last, 1u, 0u, 15u);                /* adds r0, #1 */
    *c += cyc;
    return 1;
}

/* Lapinou 0x4b30: the tail of the sprite copy, for (; r0 != r6; r0 += 2) *(u16 *)(r3 + r0) = r7 = *(u16 *)(r2 + r0);
 * (head 0x4b30..0x4b36, then 0x4b60 cmp r6, r0 / bne).  10 cycles a turn; the turn that leaves the loop is the caller's. */
static const uint16_t __attribute__((unused)) m0_lp_tail_code[4] = {0x5a17,0x521f,0x3002,0xe013};
static inline int m0k_lp4b30(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) {
    uint32_t *r = m->r;
    const uint8_t *tail = m0k_ptr(m, 0x4b60u, 4, 0);
    if (!tail || tail[0] != 0x86 || tail[1] != 0x42 || tail[2] != 0xe5 || tail[3] != 0xd1) return 0;   /* cmp r6, r0 ; bne 0x4b30 */
    const uint32_t off = r[0], end = r[6], src = r[2], dst = r[3];
    if ((off | end | src | dst) & 1u || end <= off || end - off < 4u || end > 0xffffu) return 0;
    uint32_t k = ((end - off) >> 1) - 1u, left = room - *c;
    if (left < bound + 10u) return 0;
    if (k > (left - bound) / 10u) k = (left - bound) / 10u;
    if (!k) return 0;
    const uint8_t *s = m0k_ptr(m, src + off, k << 1, 0);
    uint8_t *d = m0k_ptr(m, dst + off, k << 1, 1);
    if (!s || !d || m0k_overlap(src + off, k << 1, dst + off, k << 1)) return 0;
    uint16_t last; memcpy(&last, s + (k << 1) - 2, 2);
    memcpy(d, s, (size_t)k << 1);
    m->stores += m0k_live_stores(m, dst + off, k);
    r[0] = off + (k << 1); r[7] = last;
    (void)m0_native_addm(m, r[6], ~r[0], 1u, 15u);            /* cmp r6, r0 */
    *c += 10u * k;
    return 1;
}
#endif
