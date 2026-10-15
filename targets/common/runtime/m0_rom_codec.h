/* Console-independent, bounded ROM sector codecs. Header-only so generated
 * firmware/host projects only retain the decoders they actually use. */
#ifndef M0_ROM_CODEC_H
#define M0_ROM_CODEC_H
#include <stddef.h>
#include <stdint.h>

/* Raw DEFLATE with independent sectors needs no 32 KiB history: references
 * point into the output sector. Huffman work space is explicit (1032 bytes),
 * never hidden on the already small emulated CPU's C stack. */
typedef struct {
    uint16_t count[2][16];
    uint16_t symbol[320];
    union {
        uint8_t length[320];
        struct { uint16_t literal[128], distance[32]; } lookup;
    };
    uint16_t first[2], index[2];
} m0_inflate_workspace;

/* Fast trusted-ROM decoder, identical to the GB firmware reference. The
 * generator appends two readable padding bytes to its packed flash blob. */
static inline void m0_rom_unpack512_c(const uint8_t *src, uint8_t *dst) {
    uint8_t *d = dst, *end = dst + 512;
    uint32_t bits = 0;
    int32_t count = 0;
    while (d < end) {
        while (count < 13) { bits |= (uint32_t)*src++ << (24 - count); count += 8; }
        uint32_t flag = bits >> 31;
        bits <<= 1;
        if (flag) { *d++ = (uint8_t)(bits >> 24); bits <<= 8; count -= 9; }
        else {
            uint32_t off = (bits >> 24) + 1u, len = ((bits >> 20) & 15u) + 2u;
            bits <<= 12; count -= 13;
            const uint8_t *s = d - off;
            do { *d++ = *s++; } while (--len && d < end);
        }
    }
}

#if defined(__ARM_ARCH_6M__)
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((long_call)) void m0_rom_unpack512_asm(const uint8_t *src, uint8_t *dst);
#ifdef __cplusplus
}
#endif
#define m0_rom_unpack512 m0_rom_unpack512_asm
#else
#define m0_rom_unpack512 m0_rom_unpack512_c
#endif

typedef struct {
    const uint8_t *src, *end;
    uint32_t bits;
    unsigned count;
    int error;
} m0_rom_bits;

static inline unsigned m0_rom_get(m0_rom_bits *b, unsigned n) {
    while (b->count < n) {
        if (b->src == b->end) { b->error = 1; return 0; }
        b->bits |= (uint32_t)*b->src++ << b->count;
        b->count += 8;
    }
    unsigned value = b->bits & ((1u << n) - 1u);
    b->bits >>= n;
    b->count -= n;
    return value;
}

/* Canonical codes ordered by length then symbol. Reject oversubscribed trees;
 * incomplete alphabets are allowed (including DEFLATE's one-symbol trees). */
static inline int m0_rom_tree(uint16_t *count, uint16_t *symbols,
                              const uint8_t *lengths, unsigned n) {
    uint16_t next[16];
    for (unsigned i = 0; i < 16; ++i) count[i] = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (lengths[i] > 15) return 0;
        ++count[lengths[i]];
    }
    int left = 1;
    next[1] = 0;
    for (unsigned len = 1; len < 16; ++len) {
        left = (left << 1) - count[len];
        if (left < 0) return 0;
        if (len < 15) next[len + 1] = next[len] + count[len];
    }
    for (unsigned i = 0; i < n; ++i)
        if (lengths[i]) symbols[next[lengths[i]]++] = (uint16_t)i;
    return 1;
}

static inline int m0_rom_symbol(m0_rom_bits *b, const uint16_t *count, const uint16_t *symbols) {
    unsigned code = 0, first = 0, index = 0;
    for (unsigned len = 1; len < 16; ++len) {
        code = (code << 1) | m0_rom_get(b, 1);
        if (b->error) return -1;
        if (code >= first && code - first < count[len]) return symbols[index + code - first];
        index += count[len];
        first = (first + count[len]) << 1;
    }
    b->error = 1;
    return -1;
}

static inline unsigned m0_rom_reverse(unsigned code, unsigned bits) {
    code = ((code & 0x55u) << 1) | ((code >> 1) & 0x55u);
    code = ((code & 0x33u) << 2) | ((code >> 2) & 0x33u);
    code = ((code & 0x0fu) << 4) | ((code >> 4) & 0x0fu);
    return code >> (8 - bits);
}

/* Reuse the code-length scratch after the two trees have been constructed.
 * Short codes are direct; longer ones resume canonical decoding after the
 * prefix, rather than repeating the first seven bit-by-bit tree steps. */
static inline void m0_rom_lookup(const uint16_t *count, const uint16_t *symbols,
                                 unsigned bits, uint16_t *table, uint16_t *base, uint16_t *start) {
    for (unsigned i = 0; i < (1u << bits); ++i) table[i] = 0;
    unsigned first = 0, index = 0;
    for (unsigned len = 1; len <= bits; ++len) {
        for (unsigned n = 0; n < count[len]; ++n) {
            unsigned reversed = m0_rom_reverse(first + n, len);
            uint16_t entry = (uint16_t)((len << 9) | symbols[index + n]);
            for (unsigned p = reversed; p < (1u << bits); p += 1u << len) table[p] = entry;
        }
        index += count[len];
        first = (first + count[len]) << 1;
    }
    *base = (uint16_t)first; *start = (uint16_t)index;
}

static inline int m0_rom_fast_symbol(m0_rom_bits *b, const uint16_t *count, const uint16_t *symbols,
                                    unsigned bits, const uint16_t *table, unsigned first, unsigned index) {
    while (b->count < bits && b->src < b->end) {
        b->bits |= (uint32_t)*b->src++ << b->count; b->count += 8;
    }
    if (b->count < bits) return m0_rom_symbol(b, count, symbols);
    unsigned prefix = b->bits & ((1u << bits) - 1u), entry = table[prefix];
    if (entry) {
        unsigned len = entry >> 9;
        b->bits >>= len; b->count -= len;
        return entry & 511u;
    }
    unsigned code = m0_rom_reverse(prefix, bits);
    b->bits >>= bits; b->count -= bits;
    for (unsigned len = bits + 1; len < 16; ++len) {
        code = (code << 1) | m0_rom_get(b, 1);
        if (b->error) return -1;
        if (code >= first && code - first < count[len]) return symbols[index + code - first];
        index += count[len]; first = (first + count[len]) << 1;
    }
    b->error = 1;
    return -1;
}

/* Returns 1 only for an entire valid raw-DEFLATE stream producing size bytes.
 * No allocation, no recursion, no output write outside [dst,dst+size). */
static inline int m0_rom_inflate(const uint8_t *src, size_t packed, uint8_t *dst,
                                size_t size, m0_inflate_workspace *w) {
    static const uint16_t length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const uint8_t length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const uint16_t distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
        4097,6145,8193,12289,16385,24577
    };
    static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    m0_rom_bits b = {src, src + packed, 0, 0, 0};
    size_t out = 0;
    unsigned final;
    do {
        final = m0_rom_get(&b, 1);
        unsigned type = m0_rom_get(&b, 2);
        if (b.error || type == 3) return 0;
        if (type == 0) {
            unsigned padding = b.count & 7u;
            b.bits >>= padding; b.count -= padding; /* preserve prefetched whole bytes */
            unsigned n = m0_rom_get(&b, 16), inverse = m0_rom_get(&b, 16);
            if (b.error || (n ^ inverse) != 65535u || n > size - out) return 0;
            while (n--) dst[out++] = (uint8_t)m0_rom_get(&b, 8);
            if (b.error) return 0;
            continue;
        }
        unsigned nl = 288, nd = 32;
        if (type == 1) {
            for (unsigned i = 0; i < 288; ++i)
                w->length[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            for (unsigned i = 288; i < 320; ++i) w->length[i] = 5;
        } else {
            nl = m0_rom_get(&b, 5) + 257;
            nd = m0_rom_get(&b, 5) + 1;
            unsigned nc = m0_rom_get(&b, 4) + 4;
            if (b.error || nl > 286) return 0;
            for (unsigned i = 0; i < 19; ++i) w->length[i] = 0;
            for (unsigned i = 0; i < nc; ++i) w->length[order[i]] = (uint8_t)m0_rom_get(&b, 3);
            if (b.error || !m0_rom_tree(w->count[0], w->symbol, w->length, 19)) return 0;
            unsigned i = 0;
            while (i < nl + nd) {
                int s = m0_rom_symbol(&b, w->count[0], w->symbol);
                if (s < 0) return 0;
                if (s < 16) w->length[i++] = (uint8_t)s;
                else {
                    if (s == 16 && !i) return 0;
                    unsigned repeat = s == 16 ? m0_rom_get(&b, 2) + 3 :
                                      s == 17 ? m0_rom_get(&b, 3) + 3 : m0_rom_get(&b, 7) + 11;
                    uint8_t value = s == 16 ? w->length[i - 1] : 0;
                    if (b.error || repeat > nl + nd - i) return 0;
                    while (repeat--) w->length[i++] = value;
                }
            }
        }
        if (!w->length[256] || !m0_rom_tree(w->count[0], w->symbol, w->length, nl) ||
            !m0_rom_tree(w->count[1], w->symbol + 288, w->length + nl, nd)) return 0;
        m0_rom_lookup(w->count[0], w->symbol, 7, w->lookup.literal, &w->first[0], &w->index[0]);
        m0_rom_lookup(w->count[1], w->symbol + 288, 5, w->lookup.distance, &w->first[1], &w->index[1]);
        for (;;) {
            int s = m0_rom_fast_symbol(&b, w->count[0], w->symbol, 7, w->lookup.literal, w->first[0], w->index[0]);
            if (s < 0) return 0;
            if (s < 256) {
                if (out == size) return 0;
                dst[out++] = (uint8_t)s;
            } else if (s == 256) break;
            else {
                if (s > 285) return 0;
                unsigned len = length_base[s - 257] + m0_rom_get(&b, length_extra[s - 257]);
                int d = m0_rom_fast_symbol(&b, w->count[1], w->symbol + 288, 5, w->lookup.distance, w->first[1], w->index[1]);
                if (d < 0 || d > 29) return 0;
                unsigned extra = d < 4 ? 0 : (unsigned)d / 2 - 1;
                unsigned distance = distance_base[d] + m0_rom_get(&b, extra);
                if (b.error || distance > out || len > size - out) return 0;
                while (len--) { dst[out] = dst[out - distance]; ++out; }
            }
        }
    } while (!final);
    return !b.error && out == size && b.src == b.end && b.count < 8;
}

/* The original Heatshrink-style 8/4 stream (MSB first). Bounded reference
 * decoder; the firmware's Thumb-1 version accepts the same encoded bytes. */
static inline int m0_rom_lzss(const uint8_t *src, size_t packed, uint8_t *dst, size_t size) {
    size_t bit = 0, out = 0;
    while (out < size) {
        if (bit >= packed * 8) return 0;
        unsigned literal = (src[bit >> 3] >> (7 - (bit & 7))) & 1u;
        ++bit;
        unsigned n = literal ? 8 : 12, value = 0;
        if (bit + n > packed * 8) return 0;
        while (n--) {
            value = (value << 1) | ((src[bit >> 3] >> (7 - (bit & 7))) & 1u);
            ++bit;
        }
        if (literal) dst[out++] = (uint8_t)value;
        else {
            unsigned distance = (value >> 4) + 1, len = (value & 15) + 2;
            if (distance > out || len > size - out) return 0;
            while (len--) { dst[out] = dst[out - distance]; ++out; }
        }
    }
    return 1;
}
#endif
