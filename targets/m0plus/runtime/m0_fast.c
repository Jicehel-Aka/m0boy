/* A compact Thumb-1 interpreter for the chains the translated pages do not
 * cover (and, on the AKA, instead of them: a few KiB of code that stays in
 * internal RAM, where millions of bytes of translated code thrash the flash
 * cache).  It follows the chain contract of m0_native_glue.h: cycles gather
 * in `c` and reach the clock when the chain ends; `room` (cycles before the
 * next deadline) bounds the chain; an access to the machine ends it; poll
 * loops are spent at once by the same two detectors the generator emits
 * (m0_native_idle / m0_native_loop).  What it does not run itself (SVC,
 * system registers, CPS, hints other than NOP, 32-bit encodings other than
 * BL, anything unknown) is left to m0_cpu_step, one instruction at a time.
 * Same results and cycles as m0_cpu_execute, instruction by instruction. */
#include "m0_native_glue.h"
#include "samd21.h"

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#include "esp_attr.h"
#define M0_HOT IRAM_ATTR
#else
#define M0_HOT
#endif

/* what a conditional backward branch is for the poll-loop detectors, decided once per branch address */
enum { SPIN_NONE = 0, SPIN_LOOP = 1, SPIN_IDLE = 2 };
typedef struct { uint32_t pc; uint8_t kind, rn; uint16_t imm; } M0Spin;
static M0Spin m0_spin_cache[256];

#if defined(ESP_PLATFORM) || defined(ARDUINO)
/* the ROM copy is 4-aligned (m0_aka_native.cpp) and instructions are halfword aligned: one load */
static inline uint16_t rom16(const uint8_t *f, uint32_t o) { return *(const uint16_t *)(f + o); }
#else
static inline uint16_t rom16(const uint8_t *f, uint32_t o) { return (uint16_t)(f[o] | f[o + 1] << 8); }
#endif
uint32_t m0_stat_insn;
/* Hand-written kernels for the hot loops of the games we know (chosen by profile).  A kernel is tried when
 * a backward branch lands on its head and the ROM holds exactly the code it was written for; it runs whole
 * turns of the loop (registers, flags, cycles and store counts as the loop would leave them) and the
 * interpreter does the rest, so a kernel may decline at will. */
static const uint16_t m0_ae08_code[17] = {0x6a79,0x428b,0xda0d,0x6af8,0x009a,0x18c1,0x6ab8,0x0049,0x6900,0x3301,0x5a09,0x18a8,0xba49,0x8041,0xb29b,0x52a9,0xe7ee};
typedef struct { uint32_t pc; uint16_t len; int8_t ok; const uint16_t *code;
                 int (*fn)(M0Cpu *, uint32_t *, uint32_t, uint32_t); } M0Kernel;
static int m0k_call_ae08(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) { return m0k_ae08(m, c, room, bound); }
static int m0k_call_lp4b18(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) { return m0k_lp4b18(m, c, room, bound); }
static int m0k_call_lp864a(M0Cpu *m, uint32_t *c, uint32_t room, uint32_t bound) { return m0k_lp864a(m, c, room, bound); }
static M0Kernel m0_kernels[] = {
    {0xae08u, 17, -1, m0_ae08_code, m0k_call_ae08},
    {0x4b18u, 34, -1, m0_lp_copy_code, m0k_call_lp4b18},
    {0x864au, 50, -1, m0_lp_mix_code, m0k_call_lp864a},
};
static M0_HOT int kernel_at(M0Cpu *m, uint32_t pc, uint32_t *c, uint32_t room,
                            const uint8_t *flash, uint32_t fbase, uint32_t fsize) {
    for (unsigned i = 0; i < sizeof m0_kernels / sizeof m0_kernels[0]; ++i) {
        M0Kernel *k = &m0_kernels[i];
        if (k->pc != pc) continue;
        if (k->ok < 0) {
            const uint32_t o = pc - fbase;
            k->ok = o + 2u * k->len <= fsize;
            for (unsigned j = 0; k->ok && j < k->len; ++j) if (rom16(flash, o + 2u * j) != k->code[j]) k->ok = 0;
        }
        return k->ok > 0 && k->fn(m, c, room, 16u);
    }
    return 0;
}

static M0_HOT const M0Spin *spin_of(const uint8_t *f, uint32_t fbase, uint32_t fsize, uint32_t pc, uint32_t to) {
    M0Spin *s = &m0_spin_cache[(pc >> 1) & 255u];
    if (s->pc == pc) return s;
    s->pc = pc; s->kind = SPIN_NONE; s->rn = 0; s->imm = 0;
    if (to < pc && to >= fbase && pc - fbase + 2u <= fsize) {
        if (to + 4u == pc) {
            const uint16_t ld = rom16(f, to - fbase), cmp = rom16(f, to + 2u - fbase);
            const unsigned kind = ld & 0xf800u;
            if ((kind == 0x6800u || kind == 0x7800u || kind == 0x8800u) && (cmp & 0xf800u) == 0x2800u &&
                ((cmp >> 8) & 7u) == (ld & 7u) && (ld & 7u) != ((ld >> 3) & 7u)) {
                const unsigned imm5 = (ld >> 6) & 31u, scale = kind == 0x6800u ? 4u : kind == 0x8800u ? 2u : 1u;
                s->kind = SPIN_IDLE; s->rn = (uint8_t)((ld >> 3) & 7u); s->imm = (uint16_t)(imm5 * scale);
                return s;
            }
        }
        if (pc - to <= 40u) {                       /* a short loop with no store of its own (calls aside) */
            int stores = 0;
            for (uint32_t at = to; at < pc; at += 2) {
                const uint16_t w = rom16(f, at - fbase);
                if ((w & 0xe000u) == 0x6000u && !(w & 0x0800u)) stores = 1;
                if ((w & 0xf000u) == 0x8000u && !(w & 0x0800u)) stores = 1;
                if ((w & 0xf000u) == 0x9000u && !(w & 0x0800u)) stores = 1;
                if ((w & 0xf000u) == 0x5000u && ((w >> 9) & 7u) < 3u) stores = 1;
                if ((w & 0xf800u) == 0xc000u || (w & 0xfe00u) == 0xb400u) stores = 1;
            }
            if (!stores) s->kind = SPIN_LOOP;
        }
    }
    return s;
}

static inline uint32_t asr32(uint32_t v, unsigned n) {
    if (n >= 32) return v & 0x80000000u ? 0xffffffffu : 0;
    return (uint32_t)((int32_t)v >> n);
}
static inline unsigned popcnt8(unsigned v) {
    v = v - ((v >> 1) & 0x55u); v = (v & 0x33u) + ((v >> 2) & 0x33u);
    return (v + (v >> 4)) & 15u;
}
static inline int cond_ok(const M0Cpu *m, unsigned code) {
    switch (code) {
    case 0: return m->z; case 1: return !m->z;
    case 2: return m->c; case 3: return !m->c;
    case 4: return m->n; case 5: return !m->n;
    case 6: return m->v; case 7: return !m->v;
    case 8: return m->c && !m->z; case 9: return !m->c || m->z;
    case 10: return m->n == m->v; case 11: return m->n != m->v;
    case 12: return !m->z && m->n == m->v;
    default: return m->z || m->n != m->v;
    }
}
#define ADDF(a, b, k) m0_native_addm(m, (a), (b), (k), 15u)
#define SUBF(a, b, k) m0_native_addm(m, (a), ~(b), (k), 15u)
#define NZ(v) m0_native_nzm(m, (v), 12u)

#ifdef M0_FAST_PROF
volatile uint32_t m0_fast_pc;     /* the guest PC being interpreted, for the AKA sampler (0 outside) */
#define FAST_PC(v) (m0_fast_pc = (v))
#else
#define FAST_PC(v) ((void)0)
#endif
/* returns 0: nothing run; 1: run and the clock has been ticked; 2: run, cycles on the clock */
int M0_HOT m0_fast_run(M0Cpu *m, uint32_t room) {
    M0Machine *bd = (M0Machine *)m->board;
    const uint8_t *flash = bd->flash;
    const uint32_t fbase = bd->flash_base, fsize = bd->flash_size;
    const uint8_t *const cw = bd->code_win;
    const uint32_t cwlo = bd->code_lo, cwlen = bd->code_len;
    uint32_t *r = m->r;
    uint32_t c = 0, pc = r[15], insn = 0;
    int hw = 0, ran = 0;
    if (m->fault) return 0;
    for (;;) {
        if ((int32_t)(room - c) < 12) break;
        const uint32_t o = pc - fbase;
        if (o > fsize - 4u || (o & 1u)) break;
        FAST_PC(pc);
        const uint32_t wo = pc - cwlo;
        const uint8_t *const fp = wo < cwlen ? cw + wo : flash + o;
        const uint32_t op = rom16(fp, 0);
        const unsigned d = op & 7u, a = (op >> 3) & 7u, b = (op >> 6) & 7u;
        unsigned cyc = 1;
        uint32_t next = pc + 2u;
        switch (op >> 11) {
        case 0: { /* LSLS imm */
            uint32_t v = r[a]; const unsigned n = (op >> 6) & 31u;
            if (n) { m->c = (uint8_t)((v >> (32 - n)) & 1u); v <<= n; }
            r[d] = v; NZ(v); break; }
        case 1: { /* LSRS imm */
            uint32_t v = r[a]; unsigned n = (op >> 6) & 31u;
            if (!n) n = 32;
            m->c = (uint8_t)((v >> (n - 1)) & 1u);
            v = n == 32 ? 0 : v >> n;
            r[d] = v; NZ(v); break; }
        case 2: { /* ASRS imm */
            uint32_t v = r[a]; unsigned n = (op >> 6) & 31u;
            if (!n) n = 32;
            m->c = (uint8_t)((v >> (n - 1)) & 1u);
            v = asr32(v, n);
            r[d] = v; NZ(v); break; }
        case 3: { /* ADDS/SUBS reg or imm3 */
            const uint32_t rhs = op & 0x400u ? b : r[b];
            r[d] = op & 0x200u ? SUBF(r[a], rhs, 1u) : ADDF(r[a], rhs, 0u); break; }
        case 4: case 5: case 6: case 7: { /* MOVS/CMP/ADDS/SUBS imm8 */
            const unsigned rd = (op >> 8) & 7u; const uint32_t imm = op & 255u;
            switch ((op >> 11) & 3u) {
            case 0: r[rd] = imm; NZ(imm); break;
            case 1: (void)SUBF(r[rd], imm, 1u); break;
            case 2: r[rd] = ADDF(r[rd], imm, 0u); break;
            default: r[rd] = SUBF(r[rd], imm, 1u); break;
            }
            break; }
        case 8: {
            if (op < 0x4400u) { /* data processing */
                const uint32_t lhs = r[d], rhs = r[a];
                uint32_t v = lhs, amount; int flags = 1, wb = 1;
                switch ((op >> 6) & 15u) {
                case 0: v = lhs & rhs; break;
                case 1: v = lhs ^ rhs; break;
                case 2: amount = rhs & 255u;
                    if (amount) { m->c = amount <= 32 ? (uint8_t)((lhs >> (32 - (amount < 32 ? amount : 32))) & 1u) : 0;
                                  v = amount < 32 ? lhs << amount : 0; } break;
                case 3: amount = rhs & 255u;
                    if (amount) { m->c = amount <= 32 ? (uint8_t)((lhs >> (amount <= 32 ? amount - 1 : 31)) & 1u) : 0;
                                  v = amount < 32 ? lhs >> amount : 0; } break;
                case 4: amount = rhs & 255u;
                    if (amount) { m->c = (uint8_t)((lhs >> ((amount < 32 ? amount : 32) - 1)) & 1u);
                                  v = asr32(lhs, amount); } break;
                case 5: v = ADDF(lhs, rhs, m->c); flags = 0; break;
                case 6: v = SUBF(lhs, rhs, m->c); flags = 0; break;
                case 7: amount = rhs & 255u;
                    if (amount) { const unsigned rot = amount & 31u;
                        v = rot ? (lhs >> rot) | (lhs << (32 - rot)) : lhs;
                        m->c = (uint8_t)(v >> 31); } break;
                case 8: NZ(lhs & rhs); flags = 0; wb = 0; break;
                case 9: v = SUBF(0u, rhs, 1u); flags = 0; break;
                case 10: (void)SUBF(lhs, rhs, 1u); flags = 0; wb = 0; break;
                case 11: (void)ADDF(lhs, rhs, 0u); flags = 0; wb = 0; break;
                case 12: v = lhs | rhs; break;
                case 13: v = lhs * rhs; break;
                case 14: v = lhs & ~rhs; break;
                default: v = ~rhs; break;
                }
                if (wb) r[d] = v;
                if (flags) NZ(v);
                break;
            }
            /* hi-register ops and BX/BLX */
            const unsigned dst = d | ((op >> 4) & 8u), src = a | ((op >> 3) & 8u);
            const uint32_t rhs = src == 15 ? pc + 4u : r[src];
            const uint32_t lhs = dst == 15 ? pc + 4u : r[dst];
            switch ((op >> 8) & 3u) {
            case 0: if (dst == 15) { next = (lhs + rhs) & ~1u; cyc = 2; } else r[dst] = lhs + rhs; break;
            case 1: (void)SUBF(lhs, rhs, 1u); break;
            case 2: if (dst == 15) { next = rhs & ~1u; cyc = 2; } else r[dst] = rhs; break;
            default:
                if (op & 0x80u) r[14] = (pc + 2u) | 1u;
                cyc = 2;
                if (rhs >= 0xfffffff0u) {          /* EXC_RETURN */
                    r[15] = pc; c += 0;
                    m0_native_branch(m, rhs); hw = 1; c += cyc; goto out_noreload;
                }
                next = rhs & ~1u; break;
            }
            break; }
        case 9: { /* LDR literal */
            const uint32_t addr = ((pc + 4u) & ~3u) + ((op & 255u) << 2);
            const uint32_t fo = addr - fbase;
            if (fo <= fsize - 4u) memcpy(&r[(op >> 8) & 7u], flash + fo, 4);
            else r[(op >> 8) & 7u] = LD32(addr);
            cyc = 2; break; }
        case 10: case 11: { /* register offset */
            const uint32_t addr = r[a] + r[b];
            switch ((op >> 9) & 7u) {
            case 0: ST32(addr, r[d]); break;
            case 1: ST16(addr, r[d]); break;
            case 2: ST8(addr, r[d]); break;
            case 3: r[d] = (uint32_t)(int32_t)(int8_t)LD8(addr); break;
            case 4: r[d] = LD32(addr); break;
            case 5: r[d] = LD16(addr); break;
            case 6: r[d] = LD8(addr); break;
            default: r[d] = (uint32_t)(int32_t)(int16_t)LD16(addr); break;
            }
            cyc = 2; break; }
        case 12: case 13: case 14: case 15: { /* word/byte immediate offset */
            const int byte = (op & 0x1000u) != 0;
            const uint32_t addr = r[a] + (((op >> 6) & 31u) << (byte ? 0 : 2));
            if (op & 0x0800u) r[d] = byte ? LD8(addr) : LD32(addr);
            else if (byte) ST8(addr, r[d]); else ST32(addr, r[d]);
            cyc = 2; break; }
        case 16: case 17: { /* halfword immediate offset */
            const uint32_t addr = r[a] + (((op >> 6) & 31u) << 1);
            if (op & 0x0800u) r[d] = LD16(addr); else ST16(addr, r[d]);
            cyc = 2; break; }
        case 18: case 19: { /* SP relative */
            const uint32_t addr = r[13] + ((op & 255u) << 2); const unsigned rt = (op >> 8) & 7u;
            if (op & 0x0800u) r[rt] = LD32(addr); else ST32(addr, r[rt]);
            cyc = 2; break; }
        case 20: case 21: /* ADR / ADD rd, sp, #imm */
            r[(op >> 8) & 7u] = (op & 0x0800u ? r[13] : ((pc + 4u) & ~3u)) + ((op & 255u) << 2); break;
        case 22: case 23: {
            if ((op & 0xff00u) == 0xb000u) {
                const unsigned amount = (op & 127u) << 2;
                r[13] += op & 0x80u ? -amount : amount;
            } else if ((op & 0xfe00u) == 0xb400u) { /* PUSH */
                uint32_t mask = (op & 255u) | (op & 0x100u ? (1u << 14) : 0);
                const unsigned n = popcnt8(op & 255u) + ((op >> 8) & 1u);
                uint32_t sp = r[13] - 4u * n;
                r[13] = sp;
                for (; mask; mask &= mask - 1u, sp += 4u) ST32(sp, r[__builtin_ctz(mask)]);
                cyc = 1 + n;
            } else if ((op & 0xfe00u) == 0xbc00u) { /* POP */
                uint32_t mask = op & 255u, sp = r[13];
                const unsigned n = popcnt8(op & 255u) + ((op >> 8) & 1u);
                for (; mask; mask &= mask - 1u, sp += 4u) r[__builtin_ctz(mask)] = LD32(sp);
                if (op & 0x100u) {
                    const uint32_t value = LD32(sp); sp += 4u;
                    r[13] = sp;
                    cyc = 3 + n;
                    if (value >= 0xfffffff0u) { r[15] = pc; m0_native_branch(m, value); hw = 1; c += cyc; goto out_noreload; }
                    next = value & ~1u;
                } else { r[13] = sp; cyc = 1 + n; }
            } else if ((op & 0xf500u) == 0xb100u) { /* CBZ / CBNZ */
                if (!!r[d] == !!(op & 0x0800u)) { next = pc + 4u + ((((op >> 9) & 1u) << 6) | (((op >> 3) & 31u) << 1)); cyc = 2; }
            } else if ((op & 0xff00u) == 0xb200u) { /* SXTH SXTB UXTH UXTB */
                uint32_t v = r[a];
                switch ((op >> 6) & 3u) {
                case 0: v = (uint32_t)(int32_t)(int16_t)v; break;
                case 1: v = (uint32_t)(int32_t)(int8_t)v; break;
                case 2: v &= 0xffffu; break;
                default: v &= 0xffu; break;
                }
                r[d] = v;
            } else if ((op & 0xff00u) == 0xba00u && ((op >> 6) & 3u) != 2u) { /* REV REV16 REVSH */
                uint32_t v = r[a];
                switch ((op >> 6) & 3u) {
                case 0: v = m0_bswap32(v); break;
                case 1: v = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8); break;
                default: v = (uint32_t)(int32_t)(int16_t)(((v & 255u) << 8) | ((v >> 8) & 255u)); break;
                }
                r[d] = v;
            } else if (op == 0xbf00u) { /* NOP */
            } else if ((op & 0xffefu) == 0xb662u) { /* CPSID i / CPSIE i */
                primask = (uint8_t)((op >> 4) & 1u);
                if (!(op & 0x10u)) hw = 1;                 /* unmasked: the board looks at the pending lines */
            } else goto bail;
            break; }
        case 24: { /* STMIA */
            const unsigned base = (op >> 8) & 7u; uint32_t mask = op & 255u, cursor = r[base];
            const unsigned n = popcnt8(mask);
            const uint32_t addr = cursor;
            for (; mask; mask &= mask - 1u, cursor += 4u) ST32(cursor, r[__builtin_ctz(mask)]);
            r[base] = addr + 4u * n; cyc = 1 + n; break; }
        case 25: { /* LDMIA */
            const unsigned base = (op >> 8) & 7u; uint32_t mask = op & 255u, cursor = r[base];
            const unsigned n = popcnt8(mask);
            const uint32_t addr = cursor, listed = mask;
            for (; mask; mask &= mask - 1u, cursor += 4u) r[__builtin_ctz(mask)] = LD32(cursor);
            if (!(listed & (1u << base))) r[base] = addr + 4u * n;
            cyc = 1 + n; break; }
        case 26: case 27: { /* conditional branch, SVC */
            const unsigned code = (op >> 8) & 15u;
            if (code >= 14u) goto bail;
            if (cond_ok(m, code)) {
                const uint32_t to = pc + 4u + (uint32_t)((int32_t)(int8_t)(op & 255u) * 2);
                cyc = 2;
                if (to < pc) {
                    const M0Spin *s = spin_of(flash, fbase, fsize, pc, to);
                    if (s->kind == SPIN_LOOP) { r[15] = pc; c += m0_native_loop(m, pc, c); }
                    else if (s->kind == SPIN_IDLE) {
                        r[15] = pc;
                        m->cycles += c; c = 0; hw = 1; m0_native_idle(m, pc, r[s->rn] + s->imm);
                    }
                }
                next = to;
            }
            break; }
        case 28: { /* B */
            next = pc + 4u + (uint32_t)(((int32_t)((op & 0x7ffu) << 21)) >> 20); cyc = 2; break; }
        case 30: { /* BL */
            const uint32_t op2 = rom16(fp, 2);
            if ((op2 & 0xd000u) != 0xd000u) {
                if (op == 0xf3bfu) { next = pc + 4u; cyc = 4; break; }              /* DSB DMB ISB */
                if (op == 0xf3efu && (op2 & 0xf000u) == 0x8000u) {                    /* MRS */
                    const uint32_t sys = op2 & 255u;
                    uint32_t value = 0;
                    if (sys <= 3) value = (uint32_t)m->n << 31 | (uint32_t)m->z << 30 | (uint32_t)m->c << 29 |
                                          (uint32_t)m->v << 28 | ((sys & 1u) ? m->exception : 0u);
                    else if (sys >= 5 && sys <= 7) value = (sys & 1u) ? m->exception : 0u;
                    else if (sys == 8 || sys == 9) value = r[13];
                    else if (sys == 16) value = primask;
                    r[(op2 >> 8) & 15u] = value; next = pc + 4u; cyc = 4; break;
                }
                goto bail;
            }
            const uint32_t s = (op >> 10) & 1u;
            const uint32_t i1 = !(((op2 >> 13) & 1u) ^ s), i2 = !(((op2 >> 11) & 1u) ^ s);
            const uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((op & 0x3ffu) << 12) | ((op2 & 0x7ffu) << 1);
            r[14] = (pc + 4u) | 1u;
            next = pc + 4u + (uint32_t)((int32_t)(imm << 7) >> 7); cyc = 3; break; }
        default: goto bail;
        }
        c += cyc; ran = 1; ++insn;
        if (__builtin_expect(next < pc, 0) && !hw) (void)kernel_at(m, next, &c, room, flash, fbase, fsize);
        pc = next;
        if (hw) { r[15] = pc; goto out_noreload; }
        continue;
    bail:
        break;
    }
    r[15] = pc;
out_noreload:
    FAST_PC(0);
    m0_stat_insn += insn;
    if (hw) { m0_native_finish(m, c); return 1; }
    if (!ran) return 0;
    m->cycles += c;
    return 2;
}
