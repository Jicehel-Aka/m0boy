/* z80_fast.h (the interpreters' fast path) against the reference
 * z80_decode + z80_execute: for every opcode it covers (plain, DD and FD
 * prefixed), random registers, flags and memory; registers, memory,
 * T-states, the refresh counter and the "control transferred" answer must
 * agree.  The fast path may decline (-1): it is then not compared. */
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[65536], ref_mem[65536];
static uint8_t rd(void *u, uint16_t a) { return ((uint8_t *)u)[a]; }
static void wr(void *u, uint16_t a, uint8_t v) { ((uint8_t *)u)[a] = v; }
static uint8_t io_in(void *u, uint16_t p) { (void)u; (void)p; return 0xff; }
static void io_out(void *u, uint16_t p, uint8_t v) { (void)u; (void)p; (void)v; }

#define Z80_FAST_FETCH(a) (mem[(uint16_t)(a)])
static inline uint8_t native_read(z80_t *c, uint16_t a) { (void)c; return mem[a]; }
static inline void native_write(z80_t *c, uint16_t a, uint8_t v) { (void)c; mem[a] = v; }
#define Z80_NATIVE_READ native_read
#define Z80_NATIVE_WRITE native_write
#include "z80_ops.h"
#include "z80_fast.h"

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void) {
    unsigned checked = 0, declined = 0, failures = 0;
    for (unsigned prefix = 0; prefix < 3; prefix++) {
        for (unsigned op = 0; op < 256; op++) {
            for (unsigned trial = 0; trial < 64; trial++) {
                for (unsigned i = 0; i < 65536; i++) mem[i] = (uint8_t)rnd();
                const uint16_t pc = (uint16_t)(0x4000 + (rnd() & 0x3ff));
                unsigned n = 0;
                if (prefix) mem[(uint16_t)(pc + n++)] = prefix == 1 ? 0xdd : 0xfd;
                mem[(uint16_t)(pc + n++)] = (uint8_t)op;
                z80_t a;
                memset(&a, 0, sizeof a);
                a.pc = pc; a.sp = (uint16_t)(0x8000 + (rnd() & 0xff));
                a.bc = (uint16_t)rnd(); a.de = (uint16_t)rnd(); a.hl = (uint16_t)rnd(); a.ix = (uint16_t)rnd(); a.iy = (uint16_t)rnd();
                a.a = (uint8_t)rnd(); a.f = (uint8_t)rnd(); a.bc2 = (uint16_t)rnd(); a.de2 = (uint16_t)rnd(); a.hl2 = (uint16_t)rnd(); a.af2 = (uint16_t)rnd();
                a.r = (uint8_t)rnd(); a.i = (uint8_t)rnd();
                a.user = mem; a.read = rd; a.write = wr; a.in = io_in; a.out = io_out;
                z80_t b = a;
                b.user = ref_mem; /* the reference runs on its own copy of memory */
                memcpy(ref_mem, mem, sizeof mem);
                /* fast path */
                int jumped;
                const int fast = z80_fast(&a, &jumped);
                if (fast < 0) { declined++; continue; }
                a.r = (uint8_t)((a.r & 128) | ((a.r + 1) & 127));
                /* reference, on ref_mem */
                static uint8_t keep[65536];
                memcpy(keep, mem, sizeof mem);
                memcpy(mem, ref_mem, sizeof mem); /* native_read/write use mem */
                z80_decoded_t d;
                if (!z80_decode(&b, &d)) { printf("decode failed %02x %02x\n", prefix, op); failures++; memcpy(mem, keep, sizeof mem); continue; }
                const unsigned cycles = z80_execute(&b, &d);
                /* a taken JR with a zero displacement ends the block in the fast path only: harmless */
                const int reference_jumped = b.pc != (uint16_t)(pc + d.length);
                memcpy(ref_mem, mem, sizeof mem);
                memcpy(mem, keep, sizeof mem);
                checked++;
                if (memcmp(&a.bc, &b.bc, 6) || a.a != b.a || a.f != b.f || a.bc2 != b.bc2 || a.de2 != b.de2 || a.hl2 != b.hl2 || a.af2 != b.af2 ||
                    a.ix != b.ix || a.iy != b.iy || a.sp != b.sp || a.pc != b.pc || a.r != b.r || a.iff1 != b.iff1 || a.iff2 != b.iff2 ||
                    (unsigned)fast != cycles || (!!jumped != reference_jumped && !(jumped && a.pc == b.pc)) || memcmp(mem, ref_mem, sizeof mem)) {
                    if (failures++ < 20)
                        printf("MISMATCH prefix %u op %02x: cycles %d/%u pc %04x/%04x f %02x/%02x a %02x/%02x r %02x/%02x jumped %d/%d\n",
                               prefix, op, fast, cycles, a.pc, b.pc, a.f, b.f, a.a, b.a, a.r, b.r, jumped, reference_jumped);
                }
            }
        }
    }
    printf("z80_fast: %u cases compared, %u declined, %u failure(s)\n", checked, declined, failures);
    return failures != 0;
}
