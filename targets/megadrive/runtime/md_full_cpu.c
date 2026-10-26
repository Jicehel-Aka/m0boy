#include "md_full.h"
#include <stdint.h>

enum { X = 16, N = 8, Z = 4, V = 2, C = 1 };
typedef struct { unsigned kind, reg, size; uint32_t addr, imm; } Ea;

static uint32_t mask(unsigned size) { return size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu; }
static uint32_t sign(unsigned size) { return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u; }
static uint16_t fetch16(MdFull *m) { uint16_t v = md_full_read16(m, m->pc); m->pc += 2; return v; }
static uint32_t fetch32(MdFull *m) { uint32_t v = md_full_read32(m, m->pc); m->pc += 4; return v; }
static uint32_t read_size(MdFull *m, uint32_t a, unsigned size) {
    return size == 1 ? md_full_read8(m, a) : size == 2 ? md_full_read16(m, a) : md_full_read32(m, a);
}
static void write_size(MdFull *m, uint32_t a, unsigned size, uint32_t v) {
    if (size == 1) md_full_write8(m, a, v);
    else if (size == 2) md_full_write16(m, a, v);
    else md_full_write32(m, a, v);
}
static uint32_t index_disp(MdFull *m, uint32_t base) {
    uint16_t ext = fetch16(m);
    unsigned reg = (ext >> 12) & 7u;
    int32_t index = (ext & 0x8000u) ? (int32_t)m->a[reg] : (int32_t)m->d[reg];
    if (!(ext & 0x0800u)) index = (int16_t)index;
    return base + index + (int8_t)ext;
}
static Ea ea(MdFull *m, unsigned bits, unsigned size) {
    Ea e = {0};
    e.kind = bits >> 3; e.reg = bits & 7u; e.size = size;
    switch (e.kind) {
    case 0: case 1: return e;
    case 2: e.addr = m->a[e.reg]; break;
    case 3:
        e.addr = m->a[e.reg];
        m->a[e.reg] += size == 1 && e.reg == 7 ? 2u : size;
        break;
    case 4:
        m->a[e.reg] -= size == 1 && e.reg == 7 ? 2u : size;
        e.addr = m->a[e.reg];
        break;
    case 5: e.addr = m->a[e.reg] + (int16_t)fetch16(m); break;
    case 6: e.addr = index_disp(m, m->a[e.reg]); break;
    case 7:
        switch (e.reg) {
        case 0: e.addr = (uint32_t)(int32_t)(int16_t)fetch16(m); break;
        case 1: e.addr = fetch32(m); break;
        case 2: { uint32_t pc = m->pc; e.addr = pc + (int16_t)fetch16(m); break; }
        case 3: { uint32_t pc = m->pc; e.addr = index_disp(m, pc); break; }
        case 4: e.kind = 8; e.imm = size == 4 ? fetch32(m) : fetch16(m); break;
        default: m->fault = 2; break;
        }
        break;
    }
    return e;
}
static uint32_t ea_read(MdFull *m, Ea e) {
    if (e.kind == 0) return m->d[e.reg] & mask(e.size);
    if (e.kind == 1) return m->a[e.reg] & mask(e.size);
    if (e.kind == 8) return e.imm & mask(e.size);
    return read_size(m, e.addr, e.size);
}
static void ea_write(MdFull *m, Ea e, uint32_t v) {
    v &= mask(e.size);
    if (e.kind == 0) m->d[e.reg] = (m->d[e.reg] & ~mask(e.size)) | v;
    else if (e.kind == 1) m->a[e.reg] = e.size == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
    else if (e.kind == 8) m->fault = 2;
    else write_size(m, e.addr, e.size, v);
}
static uint32_t ea_address(MdFull *m, unsigned bits) {
    Ea e = ea(m, bits, 4);
    if (e.kind <= 1 || e.kind == 8) m->fault = 2;
    return e.addr;
}
static void flags_logic(MdFull *m, uint32_t v, unsigned size) {
    v &= mask(size);
    m->sr = (m->sr & ~(N | Z | V | C)) | (v ? 0 : Z) | (v & sign(size) ? N : 0);
}
static void flags_add(MdFull *m, uint32_t a, uint32_t b, uint32_t r, unsigned size, int update_x) {
    uint32_t bit = sign(size), ms = mask(size);
    a &= ms; b &= ms; r &= ms;
    unsigned carry = ((uint64_t)a + b) > ms;
    unsigned overflow = (~(a ^ b) & (a ^ r) & bit) != 0;
    m->sr = (m->sr & ~(N | Z | V | C | (update_x ? X : 0))) |
        (r ? 0 : Z) | (r & bit ? N : 0) | (overflow ? V : 0) |
        (carry ? C | (update_x ? X : 0) : 0);
}
static void flags_sub(MdFull *m, uint32_t a, uint32_t b, uint32_t r, unsigned size, int update_x) {
    uint32_t bit = sign(size), ms = mask(size);
    a &= ms; b &= ms; r &= ms;
    unsigned borrow = b > a;
    unsigned overflow = ((a ^ b) & (a ^ r) & bit) != 0;
    m->sr = (m->sr & ~(N | Z | V | C | (update_x ? X : 0))) |
        (r ? 0 : Z) | (r & bit ? N : 0) | (overflow ? V : 0) |
        (borrow ? C | (update_x ? X : 0) : 0);
}
static int condition(MdFull *m, unsigned cc) {
    const unsigned sr = m->sr;
    switch (cc) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !(sr & (C | Z));
    case 3: return !!(sr & (C | Z));
    case 4: return !(sr & C);
    case 5: return !!(sr & C);
    case 6: return !(sr & Z);
    case 7: return !!(sr & Z);
    case 8: return !(sr & V);
    case 9: return !!(sr & V);
    case 10: return !(sr & N);
    case 11: return !!(sr & N);
    case 12: return !!(sr & N) == !!(sr & V);
    case 13: return !!(sr & N) != !!(sr & V);
    case 14: return !(sr & Z) && (!!(sr & N) == !!(sr & V));
    default: return !!(sr & Z) || (!!(sr & N) != !!(sr & V));
    }
}
static void push16(MdFull *m, uint16_t v) { m->a[7] -= 2; md_full_write16(m, m->a[7], v); }
static void push32(MdFull *m, uint32_t v) { m->a[7] -= 4; md_full_write32(m, m->a[7], v); }
static uint16_t pop16(MdFull *m) { uint16_t v = md_full_read16(m, m->a[7]); m->a[7] += 2; return v; }
static uint32_t pop32(MdFull *m) { uint32_t v = md_full_read32(m, m->a[7]); m->a[7] += 4; return v; }

void md_full_execute(MdFull *m, uint16_t op) {
    const unsigned top = op >> 12, reg = (op >> 9) & 7u, bits = op & 63u;
    unsigned size;
    m->last_opcode = op;
    m->cycles += 8;
    m->frame_cycles += 8;
    ++m->instructions;
    if (top == 1 || top == 2 || top == 3) {
        size = top == 1 ? 1 : top == 2 ? 4 : 2;
        Ea src = ea(m, bits, size);
        uint32_t v = ea_read(m, src);
        unsigned destbits = ((op >> 6) & 7u) << 3 | reg;
        if (((op >> 6) & 7u) == 1u) {
            m->a[reg] = size == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
        } else {
            Ea dst = ea(m, destbits, size);
            ea_write(m, dst, v);
            flags_logic(m, v, size);
        }
        return;
    }
    if (top == 7) {
        m->d[reg] = (uint32_t)(int32_t)(int8_t)op;
        flags_logic(m, m->d[reg], 4);
        return;
    }
    if (top == 6) {
        unsigned cc = (op >> 8) & 15u;
        int32_t disp = (int8_t)op;
        if (!(op & 255u)) disp = (int16_t)fetch16(m);
        if (cc == 1) { push32(m, m->pc); m->pc = (uint32_t)(m->fault_pc + 2u + disp); }
        else if (condition(m, cc)) m->pc = (uint32_t)(m->fault_pc + 2u + disp);
        return;
    }
    if (top == 5) {
        if ((op & 0x00c0u) == 0x00c0u) {
            unsigned cc = (op >> 8) & 15u;
            if ((op & 0x0038u) == 0x0008u) {
                int16_t disp = (int16_t)fetch16(m);
                if (!condition(m, cc)) {
                    uint16_t v = (uint16_t)(m->d[bits & 7u] - 1u);
                    m->d[bits & 7u] = (m->d[bits & 7u] & 0xffff0000u) | v;
                    if (v != 0xffffu) m->pc = m->fault_pc + 2u + disp;
                }
            } else {
                Ea dst = ea(m, bits, 1);
                ea_write(m, dst, condition(m, cc) ? 0xffu : 0u);
            }
            return;
        }
        size = (op >> 6) & 3u;
        if (size == 3) { m->fault = 2; return; }
        size = 1u << size;
        uint32_t v = (op >> 9) & 7u; if (!v) v = 8;
        Ea dst = ea(m, bits, size);
        if (dst.kind == 1) {
            if (op & 0x0100u) m->a[dst.reg] -= v;
            else m->a[dst.reg] += v;
        } else {
            uint32_t old = ea_read(m, dst), result = (op & 0x0100u) ? old - v : old + v;
            ea_write(m, dst, result);
            if (op & 0x0100u) flags_sub(m, old, v, result, size, 1);
            else flags_add(m, old, v, result, size, 1);
        }
        return;
    }
    if (top == 0) {
        unsigned opkind = (op >> 8) & 15u;
        /* Static and dynamic bit operations. */
        if ((op & 0x0f00u) == 0x0800u || (op & 0xf100u) == 0x0100u) {
            unsigned bit = (op & 0x0100u) ? m->d[(op >> 9) & 7u] : fetch16(m);
            unsigned kind = (op >> 6) & 3u;
            size = (bits >> 3) == 0u ? 4 : 1;
            Ea dst = ea(m, bits, size);
            bit %= size == 4 ? 32u : 8u;
            uint32_t old = ea_read(m, dst), b = 1u << bit;
            m->sr = (m->sr & ~Z) | (old & b ? 0 : Z);
            if (kind) ea_write(m, dst, kind == 1 ? old ^ b : kind == 2 ? old & ~b : old | b);
            return;
        }
        if (op == 0x003c || op == 0x023c || op == 0x0a3c) {
            uint16_t v = fetch16(m);
            if (op == 0x003c) m->sr |= v & 31u;
            else if (op == 0x023c) m->sr &= (uint16_t)(0xffe0u | (v & 31u));
            else m->sr ^= v & 31u;
            return;
        }
        if (op == 0x007c || op == 0x027c || op == 0x0a7c) {
            uint16_t v = fetch16(m);
            if (op == 0x007c) m->sr |= v;
            else if (op == 0x027c) m->sr &= v;
            else m->sr ^= v;
            return;
        }
        size = (op >> 6) & 3u;
        if (size != 3u && (opkind == 0 || opkind == 2 || opkind == 4 || opkind == 6 ||
                            opkind == 10 || opkind == 12)) {
            size = 1u << size;
            uint32_t imm = size == 4 ? fetch32(m) : fetch16(m);
            Ea dst = ea(m, bits, size);
            uint32_t old = ea_read(m, dst), result = old;
            switch (opkind) {
            case 0: result = old | imm; break;
            case 2: result = old & imm; break;
            case 4: result = old - imm; break;
            case 6: result = old + imm; break;
            case 10: result = old ^ imm; break;
            case 12: result = old - imm; break;
            }
            if (opkind != 12) ea_write(m, dst, result);
            if (opkind == 4 || opkind == 12) flags_sub(m, old, imm, result, size, opkind != 12);
            else if (opkind == 6) flags_add(m, old, imm, result, size, 1);
            else flags_logic(m, result, size);
            return;
        }
    }
    if (top == 4) {
        if (op == 0x4e71) return;
        if (op == 0x4e75) { m->pc = pop32(m); return; }
        if (op == 0x4e73) { m->sr = pop16(m); m->pc = pop32(m); return; }
        if (op == 0x4e72) { m->sr = fetch16(m); m->stopped = 1; return; }
        if (op == 0x4e70) return;
        if ((op & 0xfff8u) == 0x4e50u) {
            push32(m, m->a[bits & 7u]);
            m->a[bits & 7u] = m->a[7];
            m->a[7] += (int16_t)fetch16(m);
            return;
        }
        if ((op & 0xfff8u) == 0x4e58u) {
            m->a[7] = m->a[bits & 7u];
            m->a[bits & 7u] = pop32(m);
            return;
        }
        if ((op & 0xfff0u) == 0x4e60u) {
            if (op & 8u) m->a[bits & 7u] = m->usp;
            else m->usp = m->a[bits & 7u];
            return;
        }
        if ((op & 0xffc0u) == 0x4e80u || (op & 0xffc0u) == 0x4ec0u) {
            uint32_t target = ea_address(m, bits);
            if ((op & 0xffc0u) == 0x4e80u) push32(m, m->pc);
            m->pc = target;
            return;
        }
        if ((op & 0xf1c0u) == 0x41c0u) { m->a[reg] = ea_address(m, bits); return; }
        if ((op & 0xffc0u) == 0x4840u && (op & 0x0038u) != 0u) {
            push32(m, ea_address(m, bits)); return;
        }
        if ((op & 0xfff8u) == 0x4840u) {
            uint32_t v = m->d[bits & 7u];
            m->d[bits & 7u] = v << 16 | v >> 16;
            flags_logic(m, m->d[bits & 7u], 4);
            return;
        }
        if ((op & 0xffb8u) == 0x4880u) {
            unsigned r = bits & 7u;
            if (op & 0x0040u) { m->d[r] = (uint32_t)(int32_t)(int16_t)m->d[r]; flags_logic(m, m->d[r], 4); }
            else { m->d[r] = (m->d[r] & 0xffff0000u) | (uint16_t)(int16_t)(int8_t)m->d[r]; flags_logic(m, m->d[r], 2); }
            return;
        }
        if ((op & 0xfb80u) == 0x4880u) {
            size = op & 0x0040u ? 4 : 2;
            uint16_t list = fetch16(m);
            unsigned mode = (bits >> 3) & 7u, ar = bits & 7u;
            Ea dst = ea(m, bits, size);
            uint32_t addr = dst.addr;
            if (op & 0x0400u) {
                for (unsigned r = 0; r < 16; ++r) if (list & (1u << r)) {
                    uint32_t v = read_size(m, addr, size); addr += size;
                    if (size == 2) v = (uint32_t)(int32_t)(int16_t)v;
                    if (r < 8) m->d[r] = v; else m->a[r - 8] = v;
                }
            } else if (mode == 4) {
                addr = m->a[ar] + size; /* ea() already predecremented once */
                for (unsigned r = 0; r < 16; ++r) if (list & (1u << r)) {
                    addr -= size;
                    write_size(m, addr, size, r < 8 ? m->a[7 - r] : m->d[15 - r]);
                }
            } else {
                for (unsigned r = 0; r < 16; ++r) if (list & (1u << r)) {
                    write_size(m, addr, size, r < 8 ? m->d[r] : m->a[r - 8]);
                    addr += size;
                }
            }
            if (mode == 3 || mode == 4) m->a[ar] = addr;
            return;
        }
        if ((op & 0xffc0u) == 0x40c0u) { Ea dst = ea(m, bits, 2); ea_write(m, dst, m->sr); return; }
        if ((op & 0xffc0u) == 0x44c0u) { Ea src = ea(m, bits, 2); m->sr = (m->sr & 0xffe0u) | (ea_read(m, src) & 31u); return; }
        if ((op & 0xffc0u) == 0x46c0u) { Ea src = ea(m, bits, 2); m->sr = ea_read(m, src); return; }
        if ((op & 0xff00u) == 0x4200u || (op & 0xff00u) == 0x4a00u ||
            (op & 0xff00u) == 0x4400u || (op & 0xff00u) == 0x4600u ||
            (op & 0xff00u) == 0x4000u) {
            size = (op >> 6) & 3u;
            if (size == 3u) { m->fault = 2; return; }
            size = 1u << size;
            Ea dst = ea(m, bits, size);
            uint32_t old = ea_read(m, dst), v = old;
            switch ((op >> 8) & 15u) {
            case 0: v = 0u - old - !!(m->sr & X); ea_write(m, dst, v); flags_sub(m, 0, old + !!(m->sr & X), v, size, 1); break;
            case 2: v = 0; ea_write(m, dst, v); flags_logic(m, v, size); break;
            case 4: v = 0u - old; ea_write(m, dst, v); flags_sub(m, 0, old, v, size, 1); break;
            case 6: v = ~old; ea_write(m, dst, v); flags_logic(m, v, size); break;
            case 10: flags_logic(m, old, size); break;
            }
            return;
        }
    }
    if (top == 8 || top == 9 || top == 11 || top == 12 || top == 13) {
        unsigned mode = (op >> 6) & 7u;
        if ((top == 9 || top == 11 || top == 13) && (mode == 3 || mode == 7)) {
            size = mode == 3 ? 2 : 4;
            Ea src = ea(m, bits, size);
            uint32_t v = ea_read(m, src);
            if (size == 2) v = (uint32_t)(int32_t)(int16_t)v;
            if (top == 9) m->a[reg] -= v;
            else if (top == 13) m->a[reg] += v;
            else flags_sub(m, m->a[reg], v, m->a[reg] - v, 4, 0);
            return;
        }
        if (top == 11 && (mode == 4 || mode == 5 || mode == 6)) {
            size = 1u << (mode - 4u);
            Ea dst = ea(m, bits, size);
            uint32_t v = ea_read(m, dst) ^ (m->d[reg] & mask(size));
            ea_write(m, dst, v); flags_logic(m, v, size); return;
        }
        if (top == 11 && mode <= 2) {
            size = 1u << mode;
            Ea src = ea(m, bits, size);
            uint32_t v = ea_read(m, src), old = m->d[reg] & mask(size);
            flags_sub(m, old, v, old - v, size, 0); return;
        }
        if ((top == 8 || top == 12) && (mode == 3 || mode == 7)) {
            size = 2;
            Ea src = ea(m, bits, size);
            uint32_t v = ea_read(m, src);
            if (top == 12) {
                uint32_t result = (mode == 7) ? (uint32_t)((int32_t)(int16_t)v * (int32_t)(int16_t)m->d[reg]) :
                    (uint32_t)(uint16_t)v * (uint16_t)m->d[reg];
                m->d[reg] = result; flags_logic(m, result, 4);
            } else {
                if (!v) { m->fault = 5; return; }
                uint32_t dividend = m->d[reg];
                int32_t quotient, remainder;
                if (mode == 7) { quotient = (int32_t)dividend / (int16_t)v; remainder = (int32_t)dividend % (int16_t)v; }
                else { quotient = dividend / v; remainder = dividend % v; }
                if (quotient < -32768 || quotient > 65535) m->sr |= V;
                else { m->d[reg] = ((uint32_t)remainder << 16) | (uint16_t)quotient; flags_logic(m, quotient, 2); }
            }
            return;
        }
        if (top == 12 && (op & 0xf130u) == 0xc100u) {
            unsigned r = bits & 7u;
            if ((op & 0x00f8u) == 0x0040u) {
                uint32_t t = m->d[reg]; m->d[reg] = m->d[r]; m->d[r] = t;
            } else if ((op & 0x00f8u) == 0x0048u) {
                uint32_t t = m->a[reg]; m->a[reg] = m->a[r]; m->a[r] = t;
            } else { uint32_t t = m->d[reg]; m->d[reg] = m->a[r]; m->a[r] = t; }
            return;
        }
        if (mode <= 2 || (mode >= 4 && mode <= 6)) {
            size = 1u << (mode & 3u);
            if (mode <= 2) {
                Ea src = ea(m, bits, size);
                uint32_t v = ea_read(m, src), old = m->d[reg] & mask(size), result;
                if (top == 8) result = old | v;
                else if (top == 12) result = old & v;
                else if (top == 9) result = old - v;
                else result = old + v;
                m->d[reg] = (m->d[reg] & ~mask(size)) | (result & mask(size));
                if (top == 9) flags_sub(m, old, v, result, size, 1);
                else if (top == 13) flags_add(m, old, v, result, size, 1);
                else flags_logic(m, result, size);
            } else {
                Ea dst = ea(m, bits, size);
                uint32_t old = ea_read(m, dst), v = m->d[reg] & mask(size), result;
                if (top == 8) result = old | v;
                else if (top == 12) result = old & v;
                else if (top == 9) result = old - v;
                else result = old + v;
                ea_write(m, dst, result);
                if (top == 9) flags_sub(m, old, v, result, size, 1);
                else if (top == 13) flags_add(m, old, v, result, size, 1);
                else flags_logic(m, result, size);
            }
            return;
        }
    }
    if (top == 14) {
        size = (op >> 6) & 3u;
        if (size == 3u) {
            Ea dst = ea(m, bits, 2);
            uint16_t old = (uint16_t)ea_read(m, dst);
            int left = !!(op & 0x0100u);
            unsigned type = (op >> 9) & 3u;
            uint16_t result = left ? (uint16_t)(old << 1) : old >> 1;
            unsigned carry = left ? !!(old & 0x8000u) : !!(old & 1u);
            if (type == 0 && !left) result = (uint16_t)((int16_t)old >> 1);
            if (type == 3) result |= (uint16_t)(carry << (left ? 0 : 15));
            ea_write(m, dst, result); flags_logic(m, result, 2);
            m->sr = (m->sr & ~(C | X)) | (carry ? C | X : 0);
            return;
        }
        size = 1u << size;
        unsigned count = (op & 0x0020u) ? (m->d[(op >> 9) & 7u] & 63u) : ((op >> 9) & 7u);
        if (!(op & 0x0020u) && !count) count = 8;
        unsigned type = (op >> 3) & 3u, left = !!(op & 0x0100u);
        unsigned r = bits & 7u;
        uint32_t v = m->d[r] & mask(size), last = 0;
        for (unsigned i = 0; i < count; ++i) {
            last = left ? !!(v & sign(size)) : !!(v & 1u);
            uint32_t next = left ? v << 1 : v >> 1;
            if (type == 0 && !left) next |= v & sign(size);
            if (type == 2) next |= (left ? !!(m->sr & X) : !!(m->sr & X) << (size * 8 - 1));
            if (type == 3) next |= left ? last : last << (size * 8 - 1);
            v = next & mask(size);
            if (type == 2) m->sr = (m->sr & ~X) | (last ? X : 0);
        }
        m->d[r] = (m->d[r] & ~mask(size)) | v;
        flags_logic(m, v, size);
        if (count) {
            m->sr = (m->sr & ~C) | (last ? C : 0);
            if (type != 3) m->sr = (m->sr & ~X) | (last ? X : 0);
        }
        return;
    }
    m->fault = 2;
}

void md_full_step_interp(MdFull *m) {
    if (m->fault) return;
    ++m->fallback_hits;
    m->fault_pc = m->pc;
    if (m->coverage && m->pc < m->coverage_bytes * 8u)
        m->coverage[m->pc >> 3] |= (uint8_t)(1u << (m->pc & 7u));
    uint16_t op = fetch16(m);
    if (!m->fault) md_full_execute(m, op);
}

static void interrupt(MdFull *m, unsigned level) {
    if ((m->sr >> 8 & 7u) >= level) return;
    push32(m, m->pc);
    push16(m, m->sr);
    m->sr = (m->sr & 0xf8ffu) | (level << 8);
    m->pc = md_full_read32(m, (24u + level) * 4u);
    m->stopped = 0;
    m->irq_pending = 0;
}

void md_full_frame(MdFull *m, int native) {
    const uint32_t end = 127856u;
    int vblank_sent = 0;
    m->frame_cycles = 0;
    m->irq_pending = 0;
    while (m->frame_cycles < end && !m->fault) {
        if (m->frame_cycles >= 109312u && !vblank_sent) {
            m->irq_pending = 1;
            vblank_sent = 1;
        }
        if (m->irq_pending && (m->vdp_reg[1] & 0x20u)) interrupt(m, 6);
        if (m->stopped) { m->frame_cycles += 8; continue; }
        if (native) md_full_step_native(m); else md_full_step_interp(m);
        if (m->instructions > 1000000000u) { m->fault = 6; break; }
    }
    ++m->frames;
    md_full_sound_frame(m);
}

uint32_t md_full_hash(const MdFull *m) {
    uint32_t h = 2166136261u;
    const uint8_t *data[] = {(const uint8_t *)m->d, (const uint8_t *)m->a,
        (const uint8_t *)m->ram, (const uint8_t *)m->vram, (const uint8_t *)m->cram};
    const size_t sizes[] = {sizeof m->d, sizeof m->a, sizeof m->ram, sizeof m->vram, sizeof m->cram};
    for (unsigned j = 0; j < 5; ++j)
        for (size_t i = 0; i < sizes[j]; ++i) h = (h ^ data[j][i]) * 16777619u;
    for (unsigned i = 0; i < 4; ++i) h = (h ^ ((m->pc >> (i * 8)) & 255u)) * 16777619u;
    return h;
}
