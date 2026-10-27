#include "m0_cpu.h"
#include <string.h>

static uint32_t rd(M0Cpu *m, uint32_t a, unsigned n) { return m->read(m->board, a, n); }
static void wr(M0Cpu *m, uint32_t a, uint32_t v, unsigned n) { m->write(m->board, a, v, n); }
static uint32_t reg(const M0Cpu *m, unsigned r, uint32_t pc) { return r == 15 ? pc + 4u : m->r[r]; }
static void nz(M0Cpu *m, uint32_t v) { m->n = (uint8_t)(v >> 31); m->z = (uint8_t)(v == 0); }
static uint32_t add(M0Cpu *m, uint32_t a, uint32_t b, unsigned carry) {
    uint64_t wide = (uint64_t)a + b + carry;
    uint32_t result = (uint32_t)wide;
    m->c = (uint8_t)(wide >> 32);
    m->v = (uint8_t)((~(a ^ b) & (a ^ result)) >> 31);
    nz(m, result);
    return result;
}
static uint32_t sub(M0Cpu *m, uint32_t a, uint32_t b, unsigned carry) {
    return add(m, a, ~b, carry);
}
static void exception_return(M0Cpu *m, uint32_t code) {
    if (!m->exception || (code != 0xfffffff1u && code != 0xfffffff9u)) {
        m->fault = 1; return;
    }
    uint32_t sp = m->r[13];
    for (unsigned i = 0; i < 4; ++i) { m->r[i] = rd(m, sp, 4); sp += 4; }
    m->r[12] = rd(m, sp, 4); sp += 4;
    m->r[14] = rd(m, sp, 4); sp += 4;
    uint32_t pc = rd(m, sp, 4); sp += 4;
    uint32_t psr = rd(m, sp, 4); sp += 4;
    if (psr & (1u << 9)) sp += 4;
    m->r[13] = sp;
    m->n = (uint8_t)(psr >> 31); m->z = (uint8_t)((psr >> 30) & 1u);
    m->c = (uint8_t)((psr >> 29) & 1u); m->v = (uint8_t)((psr >> 28) & 1u);
    m->exception = (uint8_t)(psr & 0x3fu);
    if (!(psr & (1u << 24))) m->fault = 1;
    m->r[15] = pc & ~1u;
}
static void branch(M0Cpu *m, uint32_t target) {
    if (target == 0xfffffff1u || target == 0xfffffff9u) {
        exception_return(m, target); return;
    }
    if (!(target & 1u)) { m->fault = 1; return; }
    m->r[15] = target & ~1u;
}
static void branch_local(M0Cpu *m, uint32_t target) { m->r[15] = target & ~1u; }
static unsigned count(uint32_t v) {
    unsigned n = 0;
    while (v) { n += v & 1u; v >>= 1; }
    return n;
}
static unsigned cond(const M0Cpu *m, unsigned code) {
    switch (code) {
    case 0: return m->z; case 1: return !m->z;
    case 2: return m->c; case 3: return !m->c;
    case 4: return m->n; case 5: return !m->n;
    case 6: return m->v; case 7: return !m->v;
    case 8: return m->c && !m->z; case 9: return !m->c || m->z;
    case 10: return m->n == m->v; case 11: return m->n != m->v;
    case 12: return !m->z && m->n == m->v;
    case 13: return m->z || m->n != m->v;
    default: return 0;
    }
}
static uint32_t asr(uint32_t v, unsigned n) {
    if (n >= 32) return v & 0x80000000u ? 0xffffffffu : 0;
    return (uint32_t)((int32_t)v >> n);
}

void m0_cpu_exception(M0Cpu *m, unsigned vector) {
    if (m->fault || vector < 2 || vector > 47) { m->fault = 1; return; }
    uint32_t handler = rd(m, m->vector_base + 4u * vector, 4);
    if (!(handler & 1u)) { m->fault = 1; return; }
    uint32_t align = (m->r[13] & 4u) ? 4u : 0u;
    uint32_t sp = m->r[13] - 32u - align, cursor = sp;
    for (unsigned i = 0; i < 4; ++i) { wr(m, cursor, m->r[i], 4); cursor += 4; }
    wr(m, cursor, m->r[12], 4); cursor += 4;
    wr(m, cursor, m->r[14], 4); cursor += 4;
    wr(m, cursor, m->r[15], 4); cursor += 4;
    uint32_t psr = (uint32_t)m->n << 31 | (uint32_t)m->z << 30 |
                   (uint32_t)m->c << 29 | (uint32_t)m->v << 28 |
                   (1u << 24) | (align ? (1u << 9) : 0u) | m->exception;
    wr(m, cursor, psr, 4);
    m->r[13] = sp;
    m->r[14] = m->exception ? 0xfffffff1u : 0xfffffff9u;
    m->exception = (uint8_t)vector;
    m->r[15] = handler & ~1u;
    m->cycles += 16;
}

void m0_cpu_reset(M0Cpu *m, void *board, M0Read read, M0Write write,
                  M0Event event, uint32_t vector_base) {
    memset(m, 0, sizeof *m);
    m->board = board; m->read = read; m->write = write; m->event = event;
    m->vector_base = vector_base;
    m->r[13] = rd(m, vector_base, 4);
    uint32_t reset = rd(m, vector_base + 4, 4);
    if (!(reset & 1u)) m->fault = 1;
    m->r[15] = reset & ~1u;
}

void m0_cpu_step(M0Cpu *m) {
    if (m->fault) return;
    uint32_t pc = m->r[15];
    uint16_t op = (uint16_t)rd(m, pc, 2);
    uint16_t op2 = (op & 0xf800u) >= 0xe800u ? (uint16_t)rd(m, pc + 2, 2) : 0;
    m0_cpu_execute(m, pc, op, op2);
}

void m0_cpu_execute(M0Cpu *m, uint32_t pc, uint16_t op, uint16_t op2) {
    if (m->fault || m->r[15] != pc) { m->fault = 1; return; }
    unsigned d = op & 7u, a = (op >> 3) & 7u, b = (op >> 6) & 7u;
    unsigned cycles = 1;
    m->r[15] = pc + 2u;
    if (op < 0x1800u) {
        uint32_t value = m->r[a], amount = (op >> 6) & 31u;
        if (op < 0x0800u) {
            if (amount) { m->c = (uint8_t)((value >> (32 - amount)) & 1u); value <<= amount; }
        } else if (op < 0x1000u) {
            if (!amount) amount = 32;
            m->c = (uint8_t)((value >> (amount - 1)) & 1u);
            value = amount == 32 ? 0 : value >> amount;
        } else {
            if (!amount) amount = 32;
            m->c = (uint8_t)((value >> (amount - 1)) & 1u);
            value = asr(value, amount);
        }
        m->r[d] = value; nz(m, value);
    } else if (op < 0x2000u) {
        uint32_t rhs = op & 0x400u ? b : m->r[b];
        m->r[d] = op & 0x200u ? sub(m, m->r[a], rhs, 1) : add(m, m->r[a], rhs, 0);
    } else if (op < 0x4000u) {
        unsigned dst = (op >> 8) & 7u, imm = op & 255u;
        switch ((op >> 11) & 3u) {
        case 0: m->r[dst] = imm; nz(m, imm); break;
        case 1: sub(m, m->r[dst], imm, 1); break;
        case 2: m->r[dst] = add(m, m->r[dst], imm, 0); break;
        default: m->r[dst] = sub(m, m->r[dst], imm, 1); break;
        }
    } else if (op < 0x4400u) {
        uint32_t lhs = m->r[d], rhs = m->r[a], value = lhs, amount;
        unsigned flags = 1, write_back = 1;
        switch ((op >> 6) & 15u) {
        case 0: value = lhs & rhs; break;
        case 1: value = lhs ^ rhs; break;
        case 2: amount = rhs & 255u;
            if (amount) { m->c = amount <= 32 ? (uint8_t)((lhs >> (32 - (amount < 32 ? amount : 32))) & 1u) : 0;
                          value = amount < 32 ? lhs << amount : 0; } break;
        case 3: amount = rhs & 255u;
            if (amount) { m->c = amount <= 32 ? (uint8_t)((lhs >> (amount <= 32 ? amount - 1 : 31)) & 1u) : 0;
                          value = amount < 32 ? lhs >> amount : 0; } break;
        case 4: amount = rhs & 255u;
            if (amount) { m->c = (uint8_t)((lhs >> ((amount < 32 ? amount : 32) - 1)) & 1u);
                          value = asr(lhs, amount); } break;
        case 5: value = add(m, lhs, rhs, m->c); flags = 0; break;
        case 6: value = sub(m, lhs, rhs, m->c); flags = 0; break;
        case 7: amount = rhs & 255u;
            if (amount) { unsigned rot = amount & 31u;
                value = rot ? (lhs >> rot) | (lhs << (32 - rot)) : lhs;
                m->c = (uint8_t)(value >> 31); } break;
        case 8: nz(m, lhs & rhs); flags = 0; write_back = 0; break;
        case 9: value = sub(m, 0, rhs, 1); flags = 0; break;
        case 10: sub(m, lhs, rhs, 1); flags = 0; write_back = 0; break;
        case 11: add(m, lhs, rhs, 0); flags = 0; write_back = 0; break;
        case 12: value = lhs | rhs; break;
        case 13: value = lhs * rhs; break;
        case 14: value = lhs & ~rhs; break;
        default: value = ~rhs; break;
        }
        if (write_back) m->r[d] = value;
        if (flags) nz(m, value);
    } else if (op < 0x4800u) {
        unsigned dst = d | ((op >> 4) & 8u), src = a | ((op >> 3) & 8u);
        uint32_t rhs = reg(m, src, pc);
        switch ((op >> 8) & 3u) {
        case 0: { uint32_t value = reg(m, dst, pc) + rhs;
            if (dst == 15) { branch_local(m, value); cycles = 2; } else m->r[dst] = value; break; }
        case 1: sub(m, reg(m, dst, pc), rhs, 1); break;
        case 2:
            if (dst == 15) { branch_local(m, rhs); cycles = 2; }
            else { m->r[dst] = rhs; }
            break;
        default:
            if (op & 0x80u) m->r[14] = (pc + 2u) | 1u;
            branch(m, rhs); cycles = 2; break;
        }
    } else if (op < 0x5000u) {
        m->r[(op >> 8) & 7u] = rd(m, ((pc + 4u) & ~3u) + ((op & 255u) << 2), 4); cycles = 2;
    } else if (op < 0x6000u) {
        uint32_t addr = m->r[a] + m->r[b];
        switch ((op >> 9) & 7u) {
        case 0: wr(m, addr, m->r[d], 4); break;
        case 1: wr(m, addr, m->r[d], 2); break;
        case 2: wr(m, addr, m->r[d], 1); break;
        case 3: m->r[d] = (uint32_t)(int32_t)(int8_t)rd(m, addr, 1); break;
        case 4: m->r[d] = rd(m, addr, 4); break;
        case 5: m->r[d] = rd(m, addr, 2); break;
        case 6: m->r[d] = rd(m, addr, 1); break;
        default: m->r[d] = (uint32_t)(int32_t)(int16_t)rd(m, addr, 2); break;
        }
        cycles = 2;
    } else if (op < 0x8000u) {
        unsigned width = op & 0x1000u ? 1 : 4;
        uint32_t addr = m->r[a] + ((op >> 6) & 31u) * width;
        if (op & 0x0800u) m->r[d] = rd(m, addr, width);
        else wr(m, addr, m->r[d], width);
        cycles = 2;
    } else if (op < 0x9000u) {
        uint32_t addr = m->r[a] + (((op >> 6) & 31u) << 1);
        if (op & 0x0800u) m->r[d] = rd(m, addr, 2);
        else wr(m, addr, m->r[d], 2);
        cycles = 2;
    } else if (op < 0xa000u) {
        uint32_t addr = m->r[13] + ((op & 255u) << 2);
        if (op & 0x0800u) m->r[(op >> 8) & 7u] = rd(m, addr, 4);
        else wr(m, addr, m->r[(op >> 8) & 7u], 4);
        cycles = 2;
    } else if (op < 0xb000u) {
        m->r[(op >> 8) & 7u] = (op & 0x0800u ? m->r[13] : ((pc + 4u) & ~3u)) + ((op & 255u) << 2);
    } else if ((op & 0xff00u) == 0xb000u) {
        unsigned amount = (op & 127u) << 2;
        m->r[13] += op & 0x80u ? -amount : amount;
    } else if ((op & 0xfe00u) == 0xb400u) {
        uint32_t mask = (op & 255u) | (op & 0x100u ? (1u << 14) : 0);
        unsigned n = count(mask); uint32_t sp = m->r[13] - 4u * n;
        m->r[13] = sp;
        for (unsigned r = 0; r < 15; ++r) if (mask & (1u << r)) { wr(m, sp, m->r[r], 4); sp += 4; }
        cycles = 1 + n;
    } else if ((op & 0xfe00u) == 0xbc00u) {
        uint32_t mask = (op & 255u) | (op & 0x100u ? (1u << 15) : 0);
        unsigned n = count(mask); uint32_t sp = m->r[13];
        for (unsigned r = 0; r < 16; ++r) if (mask & (1u << r)) {
            uint32_t value = rd(m, sp, 4); sp += 4;
            if (r == 15) branch(m, value); else m->r[r] = value;
        }
        m->r[13] = sp; cycles = (mask & (1u << 15) ? 3 : 1) + n;
    } else if ((op & 0xf500u) == 0xb100u) {
        uint32_t offset = ((op >> 9) & 1u) << 6 | ((op >> 3) & 31u) << 1;
        if (!!m->r[d] == !!(op & 0x0800u)) { branch_local(m, pc + 4u + offset); cycles = 2; }
    } else if ((op & 0xff00u) == 0xb200u) {
        uint32_t value = m->r[a];
        switch ((op >> 6) & 3u) {
        case 0: value = (uint32_t)(int32_t)(int16_t)value; break;
        case 1: value = (uint32_t)(int32_t)(int8_t)value; break;
        case 2: value &= 0xffffu; break;
        default: value &= 0xffu; break;
        }
        m->r[d] = value;
    } else if ((op & 0xff00u) == 0xba00u) {
        uint32_t value = m->r[a];
        switch ((op >> 6) & 3u) {
        case 0: value = (value >> 24) | ((value >> 8) & 0xff00u) | ((value << 8) & 0xff0000u) | (value << 24); break;
        case 1: value = ((value & 0xff00ff00u) >> 8) | ((value & 0x00ff00ffu) << 8); break;
        case 3: value = (uint32_t)(int32_t)(int16_t)(((value & 255u) << 8) | ((value >> 8) & 255u)); break;
        default: m->fault = 1; break;
        }
        m->r[d] = value;
    } else if ((op & 0xffefu) == 0xb662u) {
        m->primask = (uint8_t)((op >> 4) & 1u);
    } else if ((op & 0xf000u) == 0xc000u) {
        unsigned base = (op >> 8) & 7u, mask = op & 255u, n = count(mask);
        uint32_t addr = m->r[base], cursor = addr;
        if (op & 0x0800u) {
            for (unsigned r = 0; r < 8; ++r) if (mask & (1u << r)) { m->r[r] = rd(m, cursor, 4); cursor += 4; }
            if (!(mask & (1u << base))) m->r[base] = addr + 4u * n;
        } else {
            for (unsigned r = 0; r < 8; ++r) if (mask & (1u << r)) { wr(m, cursor, m->r[r], 4); cursor += 4; }
            m->r[base] = addr + 4u * n;
        }
        cycles = 1 + n;
    } else if ((op & 0xff00u) == 0xdf00u) {
        m0_cpu_exception(m, 11); cycles = 0;
    } else if ((op & 0xf000u) == 0xd000u && ((op >> 8) & 15u) < 14u) {
        if (cond(m, (op >> 8) & 15u)) {
            branch_local(m, pc + 4u + (uint32_t)((int32_t)(int8_t)(op & 255u) * 2)); cycles = 2;
        }
    } else if ((op & 0xf800u) == 0xe000u) {
        int32_t offset = (int32_t)((op & 0x7ffu) << 21) >> 20;
        branch_local(m, pc + 4u + (uint32_t)offset); cycles = 2;
    } else if ((op & 0xf800u) == 0xf000u && (op2 & 0xd000u) == 0xd000u) {
        uint32_t s = (op >> 10) & 1u;
        uint32_t i1 = !(((op2 >> 13) & 1u) ^ s), i2 = !(((op2 >> 11) & 1u) ^ s);
        uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((op & 0x3ffu) << 12) | ((op2 & 0x7ffu) << 1);
        int32_t offset = (int32_t)(imm << 7) >> 7;
        m->r[14] = (pc + 4u) | 1u;
        branch_local(m, pc + 4u + (uint32_t)offset); cycles = 3;
    } else if ((op & 0xf800u) >= 0xe800u) {
        m->r[15] = pc + 4u; cycles = 4;
        if (op == 0xf3efu && (op2 & 0xf000u) == 0x8000u) {
            uint32_t sys = op2 & 255u, value = 0;
            if (sys <= 3) value = (uint32_t)m->n << 31 | (uint32_t)m->z << 30 |
                                  (uint32_t)m->c << 29 | (uint32_t)m->v << 28 |
                                  ((sys & 1u) ? m->exception : 0u);
            else if (sys >= 5 && sys <= 7) value = (sys & 1u) ? m->exception : 0;
            else if (sys == 8 || sys == 9) value = m->r[13];
            else if (sys == 16) value = m->primask;
            m->r[(op2 >> 8) & 15u] = value;
        } else if ((op & 0xfff0u) == 0xf380u && (op2 & 0xff00u) == 0x8800u) {
            uint32_t sys = op2 & 255u, value = m->r[op & 15u];
            if (sys <= 3) { m->n = (uint8_t)(value >> 31); m->z = (uint8_t)((value >> 30) & 1u);
                            m->c = (uint8_t)((value >> 29) & 1u); m->v = (uint8_t)((value >> 28) & 1u); }
            else if (sys == 8 || sys == 9) m->r[13] = value & ~3u;
            else if (sys == 16) m->primask = (uint8_t)(value & 1u);
        }
    } else if ((op & 0xff00u) == 0xbf00u || (op & 0xff00u) == 0xbe00u) {
        /* NOP, hints and BKPT. WFI can be fast-forwarded by the board. */
    } else {
        m->fault = 1; /* reserved ARMv6-M encoding */
    }
    m->cycles += cycles;
    if (m->event) m->event(m->board, m, cycles);
}
