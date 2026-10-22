/* --rom-trace-out FILE: every ROM and cartridge RAM access of a host run, in
 * order, for tools/profile/m0romsim (the card cache, flash residency and
 * the cost of each access simulated offline).
 * Header line "m0boy.romtrace.v2 <rom bytes> <cartridge RAM bytes>\n", then
 * LEB128 tokens: 0 ends a frame; otherwise token - 1 = value << 2 | type:
 *   type 0  a ROM data read,             value = zigzag(granule delta)
 *   type 1  a ROM instruction byte fetched by the interpreter, likewise
 *           (granule = ROM offset >> 4, 16 bytes, one delta for both types)
 *   type 2  the access before was repeated value more times (same granule,
 *           same type: a cache that held it holds it, each one still costs
 *           the firmware a read)
 *   type 3  a cartridge RAM access, value = zigzag(delta of its granule from
 *           the previous cartridge RAM access) << 1 | write
 * A path ending in ".gz" goes through gzip -1 (POSIX popen).  Version 1
 * had no repeat nor cartridge RAM tokens, value << 1 | type, 32-byte
 * granules. */
#ifndef M0_ROM_TRACE_H
#define M0_ROM_TRACE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    FILE *f;
    int piped;
    int32_t last, cart_last; /* previous granules, -1 none */
    unsigned last_type;      /* type of the previous access (3 + write for cartridge RAM) */
    uint64_t repeat;         /* repeats of it not yet written */
    uint64_t events, written, frames;
} m0_rom_trace_t;

static inline int m0_rom_trace_open(m0_rom_trace_t *t, const char *path, uint32_t rom_bytes, uint32_t cart_bytes) {
    const size_t n = strlen(path);
    t->piped = n > 3 && !strcmp(path + n - 3, ".gz");
    if (t->piped) {
        char command[4200];
        if (n > 4000 || strchr(path, '\'')) return 0;
        snprintf(command, sizeof command, "gzip -1 > '%s'", path);
        t->f = popen(command, "w");
    } else t->f = fopen(path, "wb");
    if (!t->f) return 0;
    setvbuf(t->f, 0, _IOFBF, 1 << 20);
    t->last = t->cart_last = -1;
    t->last_type = ~0u;
    t->repeat = 0;
    t->events = t->written = t->frames = 0;
    fprintf(t->f, "m0boy.romtrace.v2 %u %u\n", rom_bytes, cart_bytes);
    return 1;
}
static inline void m0_rom_trace_token(m0_rom_trace_t *t, uint64_t v) {
    do {
        const uint8_t b = (uint8_t)(v & 127);
        v >>= 7;
        putc(v ? b | 128 : b, t->f);
    } while (v);
}
static inline void m0_rom_trace_flush_repeat(m0_rom_trace_t *t) {
    if (!t->repeat) return;
    m0_rom_trace_token(t, (t->repeat << 2 | 2) + 1);
    t->repeat = 0;
}
static inline uint64_t m0_rom_trace_zigzag(int64_t d) { return d < 0 ? ((uint64_t)(-d) << 1) - 1 : (uint64_t)d << 1; }
static inline void m0_rom_trace_access(m0_rom_trace_t *t, uint32_t offset, unsigned kind) {
    if (!t->f) return;
    const int32_t g = (int32_t)(offset >> 4);
    t->events++;
    if (g == t->last && kind == t->last_type) { t->repeat++; return; }
    m0_rom_trace_flush_repeat(t);
    const int64_t d = t->last < 0 ? g : (int64_t)g - t->last;
    m0_rom_trace_token(t, (m0_rom_trace_zigzag(d) << 2 | kind) + 1);
    t->last = g;
    t->last_type = kind;
    t->written++;
}
static inline void m0_rom_trace_cart(m0_rom_trace_t *t, uint32_t offset, unsigned write) {
    if (!t->f) return;
    const int32_t g = (int32_t)(offset >> 4);
    t->events++;
    if (g == t->cart_last && 3 + write == t->last_type) { t->repeat++; return; }
    m0_rom_trace_flush_repeat(t);
    const int64_t d = t->cart_last < 0 ? g : (int64_t)g - t->cart_last;
    m0_rom_trace_token(t, ((m0_rom_trace_zigzag(d) << 1 | write) << 2 | 3) + 1);
    t->cart_last = g;
    t->last_type = 3 + write;
    t->written++;
}
static inline void m0_rom_trace_frame(m0_rom_trace_t *t) {
    if (!t->f) return;
    m0_rom_trace_flush_repeat(t);
    putc(0, t->f);
    t->frames++;
}
static inline int m0_rom_trace_close(m0_rom_trace_t *t) {
    if (!t->f) return 0;
    m0_rom_trace_flush_repeat(t);
    const int error = ferror(t->f) | (t->piped ? pclose(t->f) != 0 : fclose(t->f));
    t->f = 0;
    return error;
}
#endif
