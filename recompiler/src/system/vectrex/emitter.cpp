/* 6809 -> C for the Vectrex runtime (targets/vectrex/runtime/vx_gen.h).
 *
 * The cartridge's code (and the system ROM's, when the generator has it)
 * reached from the vectors, the cartridge's entry after its header, the
 * targets that follow and the host's coverage profile (played ones first
 * when the flash budget cuts) is grouped by address into regions, one C
 * function each, with gotos inside and a switch on the entry at the top.
 * Every exit returns to the dispatcher (Thumb-1 GCC never tail-calls): a
 * known target in another region leaves that region as a hint; RTS, PULS
 * PC, JMP/JSR through a register or the direct page leave pc to be looked
 * up; anything without an entry (code in RAM, SWI, RTI, CWAI, SYNC, illegal
 * opcodes, what the budget left out) is the run loop's.
 *
 * The semantics are the reference's (m6809.c), cycle for cycle: same
 * counts, same bus accesses in the same order, and an access off the page
 * maps (the VIA) sees the clock the interpreter would have shown it, the
 * registers stored and pc past the operand.  ROM bytes are constants.
 * Precise mode checks the slice after every instruction, so it ends on the
 * same instruction as the interpreter; the fast mode checks at entries
 * only, at most `check_cycles` apart, a slice overrunning by that much. */
#include "m0boy/vectrex_emitter.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <sstream>

namespace m0boy::vectrex {
namespace {

/* m6809.c's m6809_cycles0: the unprefixed opcodes, indexed extra excluded */
const uint8_t cycles0[256] = {
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6, 0, 0, 2, 4, 2, 2, 5, 9, 2, 2, 3, 2, 3, 2, 8, 6,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 2, 5, 3, 6, 20, 11, 2, 19,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 4, 7,
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 4, 7, 3, 2, 4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5,
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 7, 8, 6, 6,
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 3, 2, 4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6,
};

constexpr uint8_t fC = 1, fV = 2, fZ = 4, fN = 8, fH = 32, ALL = 0xff;

enum class K {
    none, alu8, st8, alu16, st16, ureg, umem, jmp, jsr, br, lbr, nop, daa, orcc, andcc,
    sex, exg, tfr, lea, psh, pul, rts, abx, mul,
};
/* alu16's operations and registers */
enum { SUB16, ADD16, CMP16, LD16 };
enum { RD_, RX, RY, RU, RS };

struct Ins {
    uint16_t addr = 0, next = 0;
    unsigned len = 0;
    K k = K::none;
    unsigned op = 0;   /* the opcode, 0x10xx / 0x11xx on the prefixed pages */
    unsigned lo = 0;   /* the operation (the low nibble, or SUB16..LD16) */
    unsigned reg = 0;  /* 0 A, 1 B; RD_..RS for 16 bits; LEA's X Y S U */
    unsigned mode = 0; /* 0 immediate/inherent/relative, 1 direct, 2 indexed, 3 extended */
    uint32_t imm = 0;  /* immediate, direct offset, address, mask, register pair, offset */
    uint8_t pb = 0;    /* indexed postbyte */
    uint32_t off = 0;  /* indexed offset */
    unsigned cycles = 0;
    uint16_t target = 0;
    bool has_target = false, falls = true, call = false, dynamic = false;
    bool io_read = false; /* a read may leave the page maps */
    unsigned writes = 0;
    uint8_t rd = 0, wr = 0;
    uint8_t live_in = 0, live_out = ALL;
    bool label = false;
    int region = -1;
    uint32_t hits = 0;
};

std::string hex(unsigned v, int digits = 4) {
    char b[16];
    snprintf(b, sizeof b, "0x%0*X", digits, v);
    return b;
}
std::string hexu(unsigned v) { return hex(v, 4) + "u"; }

/* an operand's address: a constant, or C over the registers (ad_) */
struct Addr {
    bool constant = false;
    uint16_t value = 0;
    std::string expr;
    bool direct = false; /* dp:value, through the region's direct page pointers */
};

class Emitter {
public:
    Emitter(const std::vector<uint8_t>& cart, const std::vector<uint8_t>& bios, const EmitOptions& o)
        : cart_(cart), bios_(bios), opt_(o) {}
    std::string run(const std::vector<CoverageEntry>& coverage, EmitStats& stats);

private:
    const std::vector<uint8_t>& cart_;
    const std::vector<uint8_t>& bios_;
    EmitOptions opt_;
    std::map<uint16_t, Ins> ins_;
    std::vector<std::vector<uint16_t>> regions_;

    /* what the reference reads at a ROM address (code, constants) */
    bool rom(uint16_t at) const {
        return at < cart_.size() || (!bios_.empty() && at >= 0xE000u);
    }
    uint8_t byte(uint16_t at) const {
        if (at < 0x8000u) return at < cart_.size() ? cart_[at] : 0xFF;
        if (at >= 0xE000u && !bios_.empty()) return bios_[at - 0xE000u];
        return 0xFF;
    }
    bool decode(uint16_t at, Ins& in) const;
    bool indexed(Ins& in, uint16_t& p) const;
    void select(const std::vector<CoverageEntry>& coverage, EmitStats& stats);
    void liveness();
    void plan();
    void emit_region(std::ostream& o, int r);
    void emit_ins(std::ostream& o, const Ins& in);

    std::string exit_to(const Ins& from, uint16_t to) const;
    std::string go(const Ins& from, uint16_t to) const;
    std::string check(const Ins& from, uint16_t to) const;
    /* vxg_read's: the cycles counted, and pc where the read may be IFR's
     * (only the spin detector looks at it) */
    std::string info(const Ins& in, unsigned counted, bool ifr = true) const {
        return ifr ? hexu(unsigned(in.next) << 8 | counted) : std::to_string(counted) + "u";
    }
    Addr address(const Ins& in, std::string& pre) const;
    std::string rd8(const Ins& in, const Addr& a, unsigned counted) const;
    std::string rd16(const Ins& in, const Addr& a, unsigned counted, std::string& pre) const;
    std::string wr8(const Ins& in, const Addr& a, const std::string& value, unsigned counted) const;
    static Addr plus1(const Addr& a) {
        Addr b = a;
        if (a.direct && (a.value & 0xFFu) != 0xFFu) { b.value = uint16_t(a.value + 1); return b; }
        b.direct = false;
        if (a.constant) b.value = uint16_t(a.value + 1);
        else b.expr = "((" + a.expr + " + 1u) & 0xFFFFu)";
        return b;
    }
};

/* the indexed postbyte and its offset; the extra cycles as the reference
 * counts them.  false: the postbytes the reference counts as illegal */
bool Emitter::indexed(Ins& in, uint16_t& p) const {
    const uint8_t pb = byte(p++);
    in.pb = pb;
    if (!(pb & 0x80)) {
        in.cycles += 1;
        in.off = uint32_t(((pb & 31) ^ 16) - 16) & 0xFFFFu;
        return true;
    }
    static const int8_t extra[16] = {2, 3, 2, 3, 0, 1, 1, -1, 1, 4, -1, 4, 1, 5, -1, 2};
    if (extra[pb & 15] < 0) return false;
    in.cycles += unsigned(extra[pb & 15]);
    switch (pb & 15) {
    case 8: case 12: in.off = uint32_t(int8_t(byte(p++))) & 0xFFFFu; break;
    case 9: case 13: case 15: in.off = unsigned(byte(p)) << 8 | byte(uint16_t(p + 1)); p += 2; break;
    }
    if (pb & 0x10) in.cycles += 3;
    return true;
}

bool Emitter::decode(uint16_t at, Ins& in) const {
    if (!rom(at)) return false;
    in = Ins{};
    in.addr = at;
    uint16_t p = uint16_t(at + 1);
    unsigned op = byte(at);
    unsigned page = 0;
    if (op == 0x10 || op == 0x11) {
        page = op;
        op = byte(p++);
    }
    auto imm8 = [&] { in.imm = byte(p++); };
    auto imm16 = [&] { in.imm = unsigned(byte(p)) << 8 | byte(uint16_t(p + 1)); p += 2; };
    /* the memory operand of the modes 1-3 */
    auto operand = [&](unsigned mode) {
        in.mode = mode;
        if (mode == 1) imm8();
        else if (mode == 3) imm16();
        else if (mode == 2) return indexed(in, p);
        return true;
    };
    auto flags_nzv = [&] { in.wr = fN | fZ | fV; };
    if (page) {
        in.op = page << 8 | op;
        const unsigned mode = (op >> 4) & 3, lo = op & 15;
        static const uint8_t compare[4] = {5, 7, 7, 8}, load[4] = {4, 6, 6, 7};
        if (page == 0x10 && op >= 0x21 && op <= 0x2F) {
            in.k = K::lbr;
            in.lo = op & 15;
            in.cycles = 5;
            imm16();
        } else if (op < 0x80 || op == 0x3F) {
            return false;
        } else if (op < 0xC0 && (lo == 3 || lo == 12)) {
            in.k = K::alu16;
            in.lo = CMP16;
            in.reg = page == 0x10 ? (lo == 3 ? RD_ : RY) : (lo == 3 ? RU : RS);
            in.cycles = compare[mode];
            if (mode == 0) imm16();
            else if (!operand(mode)) return false;
            in.wr = fN | fZ | fV | fC;
        } else if (page == 0x10 && lo == 14) {
            in.k = K::alu16;
            in.lo = LD16;
            in.reg = op < 0xC0 ? RY : RS;
            in.cycles = load[mode];
            if (mode == 0) imm16();
            else if (!operand(mode)) return false;
            flags_nzv();
        } else if (page == 0x10 && lo == 15 && mode) {
            in.k = K::st16;
            in.reg = op < 0xC0 ? RY : RS;
            in.cycles = load[mode];
            if (!operand(mode)) return false;
            in.writes = 2;
            flags_nzv();
        } else {
            return false;
        }
    } else {
        in.op = op;
        in.cycles = cycles0[op];
        const unsigned hi = op >> 4, lo = op & 15;
        const bool hole = lo == 1 || lo == 2 || lo == 5 || lo == 11;
        if (hi == 0 || hi == 6 || hi == 7) {
            if (hole) return false;
            if (!operand(hi == 0 ? 1 : hi == 6 ? 2 : 3)) return false;
            in.lo = lo;
            in.k = lo == 14 ? K::jmp : K::umem;
            if (lo != 14 && lo != 13) in.writes = 1;
        } else if (hi == 4 || hi == 5) {
            if (hole || lo == 14) return false;
            in.k = K::ureg;
            in.lo = lo;
            in.reg = hi == 5;
        } else if (hi == 2) {
            in.k = K::br;
            in.lo = lo;
            imm8();
        } else if (hi == 1) {
            switch (op) {
            case 0x12: in.k = K::nop; break;
            case 0x16: in.k = K::lbr; in.lo = 0; imm16(); break;
            case 0x17: in.k = K::jsr; in.mode = 0; imm16(); in.writes = 2; break;
            case 0x19: in.k = K::daa; break;
            case 0x1A: in.k = K::orcc; imm8(); break;
            case 0x1C: in.k = K::andcc; imm8(); break;
            case 0x1D: in.k = K::sex; break;
            case 0x1E: in.k = K::exg; imm8(); break;
            case 0x1F: in.k = K::tfr; imm8(); break;
            default: return false;
            }
        } else if (hi == 3) {
            switch (op) {
            case 0x30: case 0x31: case 0x32: case 0x33:
                in.k = K::lea;
                in.reg = op & 3;
                if (!operand(2)) return false;
                break;
            case 0x34: case 0x35: case 0x36: case 0x37: {
                in.k = (op & 1) ? K::pul : K::psh;
                in.reg = (op & 2) ? 1 : 0; /* 0: S, 1: U */
                imm8();
                static const uint8_t bytes[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
                const unsigned n = bytes[in.imm & 15] + 2u * bytes[in.imm >> 4];
                in.cycles += n;
                if (in.k == K::psh) in.writes = bytes[in.imm & 15] + 2u * bytes[in.imm >> 4];
                break;
            }
            case 0x39: in.k = K::rts; break;
            case 0x3A: in.k = K::abx; break;
            case 0x3D: in.k = K::mul; break;
            default: return false;
            }
        } else {
            /* 0x80-0xFF */
            const unsigned mode = hi & 3;
            const bool isb = op >= 0xC0;
            in.reg = isb;
            in.lo = lo;
            if ((lo == 7 || lo == 15) && mode == 0) return false;
            if (lo == 13 && isb && mode == 0) return false;
            if (lo == 13 && !isb) {
                in.k = K::jsr;
                in.writes = 2;
                if (mode == 0) imm8(); /* BSR */
                else if (!operand(mode)) return false;
                in.mode = mode;
            } else {
                const bool wide = lo == 3 || lo == 12 || lo == 14;
                if (mode == 0) {
                    if (wide) imm16();
                    else imm8();
                } else if (!operand(mode)) {
                    return false;
                }
                switch (lo) {
                case 3: in.k = K::alu16; in.lo = isb ? ADD16 : SUB16; in.reg = RD_; in.wr = fN | fZ | fV | fC; break;
                case 12:
                    in.k = K::alu16;
                    in.lo = isb ? LD16 : CMP16;
                    in.reg = isb ? RD_ : RX;
                    in.wr = isb ? fN | fZ | fV : fN | fZ | fV | fC;
                    break;
                case 14: in.k = K::alu16; in.lo = LD16; in.reg = isb ? RU : RX; flags_nzv(); break;
                case 15: in.k = K::st16; in.reg = isb ? RU : RX; in.writes = 2; flags_nzv(); break;
                case 13: in.k = K::st16; in.reg = RD_; in.writes = 2; flags_nzv(); break;
                case 7: in.k = K::st8; in.writes = 1; flags_nzv(); break;
                default:
                    in.k = K::alu8;
                    if (lo == 0 || lo == 1) in.wr = fN | fZ | fV | fC;
                    else if (lo == 2) { in.wr = fN | fZ | fV | fC; in.rd = fC; }
                    else if (lo == 9) { in.wr = fN | fZ | fV | fC | fH; in.rd = fC; }
                    else if (lo == 11) in.wr = fN | fZ | fV | fC | fH;
                    else flags_nzv();
                    break;
                }
            }
        }
    }
    in.len = unsigned(uint16_t(p - at));
    in.next = p;
    /* the whole instruction in the same ROM */
    const unsigned end = unsigned(at) + in.len;
    if (at < 0x8000u ? end > cart_.size() : end > 0x10000u) return false;

    /* the flags each kind reads and writes */
    switch (in.k) {
    case K::ureg: case K::umem:
        switch (in.lo) {
        case 0: case 3: case 8: case 15: in.wr = fN | fZ | fV | fC; break;
        case 4: case 7: in.wr = fN | fZ | fC; break;
        case 6: in.wr = fN | fZ | fC; in.rd = fC; break;
        case 9: in.wr = fN | fZ | fV | fC; in.rd = fC; break;
        case 10: case 12: case 13: in.wr = fN | fZ | fV; break;
        }
        break;
    case K::br: case K::lbr: {
        static const uint8_t reads[8] = {0, fC | fZ, fC, fZ, fV, fN, fN | fV, fN | fZ | fV};
        in.rd = reads[in.lo >> 1];
        break;
    }
    case K::daa: in.rd = fH | fC; in.wr = fN | fZ | fV; break;
    case K::orcc: case K::andcc: in.rd = ALL; break;
    case K::sex: in.wr = fN | fZ | fV; break;
    case K::exg: case K::tfr:
        if ((in.imm >> 4) == 10 || (in.imm & 15) == 10) in.rd = ALL;
        break;
    case K::lea: if (in.reg < 2) in.wr = fZ; break;
    case K::psh: if (in.imm & 1) in.rd = ALL; break;
    case K::pul: if (in.imm & 1) in.wr = ALL; break;
    case K::mul: in.wr = fZ | fC; break;
    default: break;
    }

    /* control */
    const uint16_t w = uint16_t(in.imm);
    switch (in.k) {
    case K::br:
        if (in.lo == 1) break; /* BRN */
        in.target = uint16_t(in.next + int8_t(in.imm));
        in.has_target = true;
        in.falls = in.lo != 0;
        break;
    case K::lbr:
        if (in.lo == 1) break; /* LBRN */
        in.target = uint16_t(in.next + w);
        in.has_target = true;
        in.falls = in.op != 0x16; /* LBRA */
        break;
    case K::jmp: case K::jsr: {
        if (in.k == K::jsr) {
            in.call = true;
        }
        in.falls = false;
        if (in.k == K::jsr && in.mode == 0) {
            /* BSR, LBSR */
            in.target = uint16_t(in.next + (in.op == 0x17 ? w : uint16_t(int8_t(in.imm))));
            in.has_target = true;
            break;
        }
        std::string pre;
        const Addr a = address(in, pre);
        if (a.constant) {
            in.target = a.value;
            in.has_target = true;
        } else {
            in.dynamic = true;
        }
        break;
    }
    case K::rts: in.falls = false; in.dynamic = true; break;
    case K::pul: if (in.imm & 0x80) { in.falls = false; in.dynamic = true; } break;
    case K::exg:
        if ((in.imm >> 4) == 5 || (in.imm & 15) == 5) { in.falls = false; in.dynamic = true; }
        break;
    case K::tfr: if ((in.imm & 15) == 5) { in.falls = false; in.dynamic = true; } break;
    default: break;
    }

    /* reads that may be the VIA's IFR: its spin detector compares the
     * registers (the flags included) from one IFR read to the next */
    {
        bool reads = false;
        unsigned width = 1;
        switch (in.k) {
        case K::alu8: reads = in.mode != 0; break;
        case K::alu16: reads = in.mode != 0; width = 2; break;
        case K::umem: reads = true; break;
        case K::pul: case K::rts: in.io_read = true; break;
        default: break;
        }
        auto ifr = [](unsigned at) { return (at & 0xF800u) == 0xD000u && (at & 15u) == 13u; };
        const bool pointer = in.mode == 2 && (in.pb & 0x90) == 0x90;
        if (in.mode == 2) {
            const unsigned base = in.pb & 15;
            const bool constant = (in.pb & 0x80) && (base == 12 || base == 13 || base == 15);
            uint16_t at = uint16_t(base == 15 ? in.off : in.next + in.off);
            if (pointer) {
                if (!constant || ifr(at) || ifr(uint16_t(at + 1))) in.io_read = true;
                if (constant && rom(at) && rom(uint16_t(at + 1)))
                    at = uint16_t(byte(at) << 8 | byte(uint16_t(at + 1)));
                else
                    in.io_read = in.io_read || reads; /* the operand behind a pointer in RAM */
            }
            if (reads && (!constant || ifr(at) || (width == 2 && ifr(uint16_t(at + 1))))) in.io_read = true;
        } else if (reads && in.mode == 1) {
            if ((in.imm & 15u) == 13u || (width == 2 && ((in.imm + 1u) & 15u) == 13u)) in.io_read = true;
        } else if (reads && in.mode == 3) {
            if (ifr(in.imm) || (width == 2 && ifr(uint16_t(in.imm + 1)))) in.io_read = true;
        }
        if (in.io_read) in.rd = ALL;
    }
    return true;
}

/* the operand's address; `pre` receives what must run first (the indexed
 * modes' register updates, the pointer's read) */
Addr Emitter::address(const Ins& in, std::string& pre) const {
    Addr a;
    if (in.mode == 1) {
        a.direct = true;
        a.value = uint16_t(in.imm);
        a.expr = "((uint32_t)c->dp << 8 | " + hex(in.imm, 2) + "u)";
        return a;
    }
    if (in.mode == 3) {
        a.constant = true;
        a.value = uint16_t(in.imm);
        return a;
    }
    static const char* regs[4] = {"c->x", "c->y", "c->u", "c->s"};
    const std::string r = regs[(in.pb >> 5) & 3];
    const uint8_t pb = in.pb;
    if (!(pb & 0x80)) {
        pre += "const uint32_t ad_ = (" + r + " + " + hexu(in.off) + ") & 0xFFFFu; ";
        a.expr = "ad_";
        return a;
    }
    switch (pb & 15) {
    case 0: pre += "const uint32_t ad_ = " + r + "; " + r + " = (uint16_t)(ad_ + 1u); "; break;
    case 1: pre += "const uint32_t ad_ = " + r + "; " + r + " = (uint16_t)(ad_ + 2u); "; break;
    case 2: pre += r + " = (uint16_t)(" + r + " - 1u); const uint32_t ad_ = " + r + "; "; break;
    case 3: pre += r + " = (uint16_t)(" + r + " - 2u); const uint32_t ad_ = " + r + "; "; break;
    case 4: pre += "const uint32_t ad_ = " + r + "; "; break;
    case 5: pre += "const uint32_t ad_ = (" + r + " + (uint32_t)(int8_t)c->b) & 0xFFFFu; "; break;
    case 6: pre += "const uint32_t ad_ = (" + r + " + (uint32_t)(int8_t)c->a) & 0xFFFFu; "; break;
    case 8: case 9: pre += "const uint32_t ad_ = (" + r + " + " + hexu(in.off) + ") & 0xFFFFu; "; break;
    case 11: pre += "const uint32_t ad_ = (" + r + " + VXG_D()) & 0xFFFFu; "; break;
    case 12: case 13: a.constant = true; a.value = uint16_t(in.next + in.off); break;
    case 15: a.constant = true; a.value = uint16_t(in.off); break;
    }
    if (!a.constant) a.expr = "ad_";
    if (pb & 0x10) {
        /* indirect: the pointer, read before the last 3 cycles count */
        if (a.constant && rom(a.value) && rom(uint16_t(a.value + 1))) {
            a.value = uint16_t(byte(a.value) << 8 | byte(uint16_t(a.value + 1)));
            return a;
        }
        std::string p2;
        const std::string ptr = rd16(in, a, in.cycles - 3, p2);
        pre += p2 + "const uint32_t ind_ = " + ptr + "; ";
        a.constant = false;
        a.expr = "ind_";
    }
    return a;
}

std::string Emitter::rd8(const Ins& in, const Addr& a, unsigned counted) const {
    if (a.direct)
        return "VXG_DRD(" + hex(a.value & 0xFFu, 2) + "u, " + info(in, counted, (a.value & 15u) == 13u) + ")";
    if (!a.constant) return "VXG_RD(" + a.expr + ", " + info(in, counted) + ")";
    const uint16_t at = a.value;
    if (at < 0x8000u) return hexu(byte(at));
    if ((at & 0xF800u) == 0xC800u) return "ram_[" + hex(at & 1023u, 3) + "]";
    if ((at & 0xF800u) == 0xD000u) return "VXG_IO_RD(" + hexu(at) + ", " + info(in, counted, (at & 15u) == 13u) + ")";
    if (at >= 0xE000u) return bios_.empty() ? "v->bios[" + hex(at - 0xE000u) + "]" : hexu(byte(at));
    return "0xFFu";
}
/* hi, then lo: the order of the reference's accesses */
std::string Emitter::rd16(const Ins& in, const Addr& a, unsigned counted, std::string& pre) const {
    static int serial = 0;
    const std::string h = "h" + std::to_string(serial++) + "_";
    pre += "const uint32_t " + h + " = " + rd8(in, a, counted) + "; ";
    return "(" + h + " << 8 | " + rd8(in, plus1(a), counted) + ")";
}
std::string Emitter::wr8(const Ins& in, const Addr& a, const std::string& value, unsigned counted) const {
    (void)in;
    if (a.direct) return "VXG_DWR(" + hex(a.value & 0xFFu, 2) + "u, " + value + ", " + std::to_string(counted) + "u); ";
    if (!a.constant) return "VXG_WR(" + a.expr + ", " + value + ", " + std::to_string(counted) + "u); ";
    const uint16_t at = a.value;
    if ((at & 0xF800u) == 0xC800u) return "ram_[" + hex(at & 1023u, 3) + "] = (uint8_t)(" + value + "); ";
    if ((at & 0xF800u) == 0xD000u) return "VXG_IO_WR(" + hexu(at) + ", " + value + ", " + std::to_string(counted) + "u); ";
    return ""; /* ROM, nothing: only counted */
}

void Emitter::select(const std::vector<CoverageEntry>& coverage, EmitStats& stats) {
    std::vector<CoverageEntry> hot(coverage);
    std::stable_sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.hits > b.hits; });
    std::map<uint16_t, Ins> found;
    std::vector<uint16_t> order;
    std::deque<uint16_t> work;
    auto visit = [&](uint16_t at) {
        if (found.count(at)) return;
        Ins in;
        if (!decode(at, in)) return;
        found[at] = in;
        order.push_back(at);
        work.push_back(at);
    };
    auto drain = [&] {
        while (!work.empty()) {
            const Ins in = found[work.front()];
            work.pop_front();
            if (in.falls || in.call) visit(in.next);
            if (in.has_target) visit(in.target);
        }
    };
    std::map<uint16_t, uint32_t> played;
    for (const auto& e : hot) played[e.pc] += e.hits;
    for (const auto& e : hot) visit(e.pc);
    const size_t covered = order.size();
    if (!bios_.empty())
        for (unsigned v = 0xFFF2; v < 0x10000; v += 2) visit(uint16_t(byte(uint16_t(v)) << 8 | byte(uint16_t(v + 1))));
    /* the cartridge's code follows its header: "g GCE", text up to 0x80,
     * the music's pointer, then title lines (4 bytes, text, 0x80) up to 0 */
    if (cart_.size() > 16 && cart_[0] == 'g') {
        size_t i = 0;
        while (i < cart_.size() && cart_[i] != 0x80) i++;
        i += 3;
        while (i < cart_.size() && cart_[i] != 0) {
            i += 4;
            while (i < cart_.size() && cart_[i] != 0x80) i++;
            i++;
        }
        if (i + 1 < cart_.size()) visit(uint16_t(i + 1));
    }
    drain();
    const size_t keep = opt_.max_instructions ? std::min(order.size(), opt_.max_instructions) : order.size();
    for (size_t i = 0; i < keep; i++) {
        Ins in = found[order[i]];
        if (auto p = played.find(in.addr); p != played.end()) in.hits = p->second;
        ins_[in.addr] = in;
    }
    stats.covered = std::min(covered, keep);
    stats.compiled = ins_.size();
}

/* the flags each instruction must produce: those a successor reads before
 * writing them; an exit, a call out or an unknown successor reads all */
void Emitter::liveness() {
    for (auto& [a, in] : ins_) in.live_in = 0;
    auto in_of = [&](bool known, uint16_t at) -> uint8_t {
        if (!known) return ALL;
        auto p = ins_.find(at);
        return p == ins_.end() ? ALL : p->second.live_in;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = ins_.rbegin(); it != ins_.rend(); ++it) {
            Ins& in = it->second;
            uint8_t out = 0;
            if (in.dynamic) out = ALL;
            if (in.falls) out |= in_of(true, in.next);
            if (in.has_target) out |= in_of(true, in.target);
            if (!in.falls && !in.has_target && !in.dynamic) out = ALL;
            const uint8_t live = uint8_t(in.rd | (out & ~in.wr));
            if (out != in.live_out || live != in.live_in) {
                in.live_out = out;
                in.live_in = live;
                changed = true;
            }
        }
    }
}

void Emitter::plan() {
    auto mark = [&](uint16_t at) { if (auto p = ins_.find(at); p != ins_.end()) p->second.label = true; };
    std::set<uint16_t> fallen; /* reached by a compiled instruction's fall */
    for (auto& [a, in] : ins_) {
        if (opt_.precise) in.label = true;
        if (in.has_target) mark(in.target);
        if (in.call) mark(in.next);
        if (in.falls) fallen.insert(in.next);
    }
    /* what the run loop hands over: entries the static walk cannot see */
    for (auto& [a, in] : ins_)
        if (in.hits && !fallen.count(a)) in.label = true;
    if (!bios_.empty())
        for (unsigned v = 0xFFF2; v < 0x10000; v += 2) mark(uint16_t(byte(uint16_t(v)) << 8 | byte(uint16_t(v + 1))));
    const size_t cap = 128;
    for (auto& [a, in] : ins_) {
        const bool joins = !regions_.empty() && regions_.back().size() < cap &&
                           ins_.at(regions_.back().back()).next == a;
        if (!joins) {
            regions_.emplace_back();
            in.label = true;
        }
        in.region = int(regions_.size() - 1);
        regions_.back().push_back(a);
    }
    if (!opt_.precise)
        for (const auto& r : regions_) {
            unsigned run = 0;
            for (uint16_t a : r) {
                Ins& in = ins_.at(a);
                if (in.label) run = 0;
                if (run > opt_.check_cycles) { in.label = true; run = 0; }
                run += in.cycles;
            }
        }
}

std::string Emitter::exit_to(const Ins& from, uint16_t to) const {
    (void)from;
    auto p = ins_.find(to);
    const int hint = p != ins_.end() && p->second.label ? p->second.region : -1;
    return "{ pc_ = " + hexu(to) + "; hint_ = " + std::to_string(hint) + "; goto out; }";
}
/* where control goes to reach `to` */
std::string Emitter::go(const Ins& from, uint16_t to) const {
    auto p = ins_.find(to);
    if (p != ins_.end() && p->second.label && p->second.region == from.region)
        return "goto L_" + hex(to).substr(2) + ";";
    return exit_to(from, to);
}
/* precise mode: the slice ends after this instruction, at `to` */
std::string Emitter::check(const Ins& from, uint16_t to) const {
    if (!opt_.precise) return "";
    return " if ((int32_t)(cycles - stop) >= 0) " + exit_to(from, to);
}

void Emitter::emit_ins(std::ostream& o, const Ins& in) {
    std::string pre, body;
    const std::string R8 = in.reg ? "c->b" : "c->a";
    const unsigned live = in.wr & in.live_out;
    const std::string L = std::to_string(live) + "u";
    /* the flags are known dead: emit no flag call at all (the live-masked
     * helpers would fold to nothing anyway) */
    const bool nf = live == 0;
    const std::string NZ8 = nf ? "" : " VXG_NZ8(z_, " + L + "); ";
    static const char* r16[5] = {"VXG_D()", "c->x", "c->y", "c->u", "c->s"};
    auto set16 = [](unsigned reg, const std::string& v) {
        static const char* f[5] = {"", "c->x", "c->y", "c->u", "c->s"};
        if (reg == RD_) return "VXG_SETD(" + v + "); ";
        return std::string(f[reg]) + " = (uint16_t)(" + v + "); ";
    };
    const unsigned T = in.cycles;
    const std::string next = hexu(in.next);
    /* DP changed: the direct page's pointers follow */
    const bool dp_moves = ((in.k == K::tfr || in.k == K::exg) && ((in.imm & 15) == 11 || (in.k == K::exg && (in.imm >> 4) == 11))) ||
                          (in.k == K::pul && (in.imm & 8));
    auto finish = [&](const std::string& tail) {
        if (dp_moves) body += "VXG_DP_LOAD(); ";
        o << "    { " << pre << body;
        if (in.writes) o << "c->writes += " << in.writes << "u; ";
        o << "cycles += " << T << "u; }" << tail << "\n";
    };
    /* pushes and pulls: within the RAM, plain stores and loads at the
     * block's place; anywhere else the run loop's order (vxg_push/pull) */
    static const unsigned bytes[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    auto push = [&](bool u, unsigned m) {
        const std::string sp = u ? "c->u" : "c->s", other = u ? "c->s" : "c->u";
        const unsigned n = bytes[m & 15] + 2 * bytes[m >> 4];
        /* the block from its lowest byte: cc a b dp x y other pc */
        std::vector<std::string> v;
        if (m & 1) v.push_back("c->cc");
        if (m & 2) v.push_back("c->a");
        if (m & 4) v.push_back("c->b");
        if (m & 8) v.push_back("c->dp");
        auto word = [&](const std::string& w) {
            v.push_back("(" + w + ") >> 8");
            v.push_back(w);
        };
        if (m & 0x10) word("c->x");
        if (m & 0x20) word("c->y");
        if (m & 0x40) word(other);
        if (m & 0x80) word(next);
        body += "{ const uint32_t s_ = (" + sp + " - " + std::to_string(n) + "u) & 0xFFFFu; if (VXG_STACK_RAM(s_, " +
                std::to_string(n) + "u)) { uint8_t *const k_ = ram_ + (s_ & 1023u); ";
        for (size_t i = 0; i < v.size(); i++) body += "k_[" + std::to_string(i) + "] = (uint8_t)(" + v[i] + "); ";
        body += sp + " = (uint16_t)s_; } else if (vxg_push(v, " + hexu(m | (u ? 0x100u : 0)) + ", " + next + ", cycles, " +
                std::to_string(T) + "u)) stop = cycles; } ";
    };
    /* returns the pulled pc's expression when the mask has it */
    auto pull = [&](bool u, unsigned m) {
        const std::string sp = u ? "c->u" : "c->s", other = u ? "c->s" : "c->u";
        const unsigned n = bytes[m & 15] + 2 * bytes[m >> 4];
        if (m & 0x80) body += "uint32_t q_ = 0; ";
        body += "{ const uint32_t s_ = " + sp + "; if (VXG_STACK_RAM(s_, " + std::to_string(n) +
                "u)) { const uint8_t *const k_ = ram_ + (s_ & 1023u); " + sp + " = (uint16_t)(s_ + " + std::to_string(n) + "u); ";
        unsigned i = 0;
        auto byte_at = [&] { return "k_[" + std::to_string(i++) + "]"; };
        auto word_at = [&] { const std::string h = byte_at(); return "(uint16_t)(" + h + " << 8 | " + byte_at() + ")"; };
        if (m & 1) body += "c->cc = " + byte_at() + "; ";
        if (m & 2) body += "c->a = " + byte_at() + "; ";
        if (m & 4) body += "c->b = " + byte_at() + "; ";
        if (m & 8) body += "c->dp = " + byte_at() + "; ";
        if (m & 0x10) body += "c->x = " + word_at() + "; ";
        if (m & 0x20) body += "c->y = " + word_at() + "; ";
        if (m & 0x40) body += other + " = " + word_at() + "; ";
        if (m & 0x80) body += "q_ = " + word_at() + "; ";
        body += "} else { " + std::string(m & 0x80 ? "q_ = " : "") + "vxg_pull(v, " + hexu(m | (u ? 0x100u : 0)) +
                ", cycles, " + info(in, T) + "); cycles = c->start; } } ";
        return std::string("q_");
    };

    switch (in.k) {
    case K::alu8: {
        const std::string t = in.mode ? rd8(in, address(in, pre), T) : hexu(in.imm);
        body += "const uint32_t t_ = " + t + "; ";
        switch (in.lo) {
        case 0: body += R8 + " = (uint8_t)vxg_sub8(c, " + R8 + ", t_, 0, " + L + "); "; break;
        case 1: if (!nf) body += "vxg_sub8(c, " + R8 + ", t_, 0, " + L + "); "; break;
        case 2: body += R8 + " = (uint8_t)vxg_sub8(c, " + R8 + ", t_, c->cc & 1u, " + L + "); "; break;
        case 4: body += "const uint32_t z_ = " + R8 + " & t_; " + R8 + " = (uint8_t)z_;" + NZ8; break;
        case 5: if (!nf) body += "const uint32_t z_ = " + R8 + " & t_; VXG_NZ8(z_, " + L + "); "; break;
        case 6: body += R8 + " = (uint8_t)t_;" + (nf ? " " : " VXG_NZ8(t_, " + L + "); "); break;
        case 8: body += "const uint32_t z_ = " + R8 + " ^ t_; " + R8 + " = (uint8_t)z_;" + NZ8; break;
        case 9: body += R8 + " = (uint8_t)vxg_add8(c, " + R8 + ", t_, c->cc & 1u, " + L + "); "; break;
        case 10: body += "const uint32_t z_ = " + R8 + " | t_; " + R8 + " = (uint8_t)z_;" + NZ8; break;
        case 11: body += R8 + " = (uint8_t)vxg_add8(c, " + R8 + ", t_, 0, " + L + "); "; break;
        }
        finish(check(in, in.next));
        return;
    }
    case K::st8: {
        const Addr a = address(in, pre);
        body += "const uint32_t z_ = " + R8 + "; " + wr8(in, a, "z_", T) + NZ8;
        finish(check(in, in.next));
        return;
    }
    case K::alu16: {
        std::string t;
        if (in.mode) {
            const Addr a = address(in, pre);
            t = rd16(in, a, T, pre);
        } else {
            t = hexu(in.imm);
        }
        body += "const uint32_t t_ = " + t + "; ";
        switch (in.lo) {
        case SUB16: body += "VXG_SETD(vxg_sub16(c, VXG_D(), t_, " + L + ")); "; break;
        case ADD16: body += "VXG_SETD(vxg_add16(c, VXG_D(), t_, " + L + ")); "; break;
        case CMP16: if (!nf) body += "vxg_sub16(c, " + std::string(r16[in.reg]) + ", t_, " + L + "); "; break;
        case LD16: body += set16(in.reg, "t_") + (nf ? "" : " VXG_NZ16(t_, " + L + "); "); break;
        }
        finish(check(in, in.next));
        return;
    }
    case K::st16: {
        const Addr a = address(in, pre);
        body += "const uint32_t z_ = " + std::string(r16[in.reg]) + "; " + wr8(in, a, "z_ >> 8", T) +
                wr8(in, plus1(a), "z_", T) + (nf ? " " : " VXG_NZ16(z_, " + L + "); ");
        finish(check(in, in.next));
        return;
    }
    case K::ureg:
        body += R8 + " = (uint8_t)vxg_unary(c, " + std::to_string(in.lo) + "u, " + R8 + ", " + L + "); ";
        finish(check(in, in.next));
        return;
    case K::umem: {
        const Addr a = address(in, pre);
        body += "const uint32_t t_ = " + rd8(in, a, T) + "; ";
        if (in.lo == 13) {
            /* TST: the read is the only effect when the flags are dead */
            if (!nf) body += "const uint32_t z_ = vxg_unary(c, 13u, t_, " + L + "); (void)z_; ";
        } else {
            body += "const uint32_t z_ = vxg_unary(c, " + std::to_string(in.lo) + "u, t_, " + L + "); " +
                    wr8(in, a, "z_", T);
        }
        finish(check(in, in.next));
        return;
    }
    case K::jmp: case K::jsr: {
        std::string target;
        if (in.k == K::jsr && in.mode == 0) {
            target = hexu(in.target);
        } else {
            const Addr a = address(in, pre);
            target = a.constant ? hexu(a.value) : a.expr;
        }
        if (in.dynamic) body += "pc_ = " + target + "; ";
        if (in.k == K::jsr) push(false, 0x80);
        if (in.dynamic) {
            finish(" hint_ = -1; goto out;");
            return;
        }
        finish(check(in, in.target) + " " + go(in, in.target));
        return;
    }
    case K::br:
        if (in.lo == 1) {
            finish(check(in, in.next));
            return;
        }
        if (in.lo == 0) {
            finish(check(in, in.target) + " " + go(in, in.target));
            return;
        }
        o << "    cycles += 3u; if (vxg_cond(c->cc, " << in.lo << "u)) {" << check(in, in.target) << " "
          << go(in, in.target) << " }" << check(in, in.next) << "\n";
        return;
    case K::lbr:
        if (in.lo == 1) {
            finish(check(in, in.next));
            return;
        }
        if (in.op == 0x16) {
            finish(check(in, in.target) + " " + go(in, in.target));
            return;
        }
        o << "    if (vxg_cond(c->cc, " << in.lo << "u)) { cycles += 6u;" << check(in, in.target) << " "
          << go(in, in.target) << " } cycles += 5u;" << check(in, in.next) << "\n";
        return;
    case K::nop: finish(check(in, in.next)); return;
    case K::daa:
        body += "const uint32_t a_ = c->a; uint32_t fix_ = 0; if ((a_ & 15u) > 9u || (c->cc & 32u)) fix_ |= 6u; "
                "if (a_ > 0x99u || (c->cc & 1u)) fix_ |= 0x60u; const uint32_t r_ = (a_ + fix_) & 0xFFu; "
                "c->a = (uint8_t)r_;" + (nf ? " " : " VXG_NZ8(r_, " + L + "); ") + "if (fix_ & 0x60u) c->cc |= 1u; ";
        finish(check(in, in.next));
        return;
    case K::orcc: body += "c->cc |= " + hex(in.imm, 2) + "u; "; finish(check(in, in.next)); return;
    case K::andcc: body += "c->cc &= " + hex(in.imm, 2) + "u; "; finish(check(in, in.next)); return;
    case K::sex:
        body += "c->a = (c->b & 0x80u) ? 0xFFu : 0;" + (nf ? " " : " VXG_NZ16(VXG_D(), " + L + "); ");
        finish(check(in, in.next));
        return;
    case K::exg: case K::tfr: {
        auto get = [&](unsigned r) -> std::string {
            switch (r) {
            case 0: return "VXG_D()";
            case 1: return "c->x";
            case 2: return "c->y";
            case 3: return "c->u";
            case 4: return "c->s";
            case 5: return next;
            case 8: return "(0xFF00u | c->a)";
            case 9: return "(0xFF00u | c->b)";
            case 10: return "(0xFF00u | c->cc)";
            case 11: return "(0xFF00u | c->dp)";
            default: return "0xFFFFu";
            }
        };
        auto set = [&](unsigned r, const std::string& v) -> std::string {
            switch (r) {
            case 0: return "VXG_SETD(" + v + "); ";
            case 1: return "c->x = (uint16_t)" + v + "; ";
            case 2: return "c->y = (uint16_t)" + v + "; ";
            case 3: return "c->u = (uint16_t)" + v + "; ";
            case 4: return "c->s = (uint16_t)" + v + "; ";
            case 5: return "pc_ = " + v + " & 0xFFFFu; ";
            case 8: return "c->a = (uint8_t)" + v + "; ";
            case 9: return "c->b = (uint8_t)" + v + "; ";
            case 10: return "c->cc = (uint8_t)" + v + "; ";
            case 11: return "c->dp = (uint8_t)" + v + "; ";
            default: return "";
            }
        };
        const unsigned hi = in.imm >> 4, lo = in.imm & 15;
        /* only EXG needs the second side; the unused-variable pragmas of the
         * generated file cover an immediate source */
        body += "const uint32_t e1_ = " + get(hi) + "; ";
        if (in.k == K::exg) body += "const uint32_t e2_ = " + get(lo) + "; ";
        if (in.k == K::exg) body += set(hi, "e2_");
        body += set(lo, "e1_");
        if (in.dynamic) {
            finish(" hint_ = -1; goto out;");
            return;
        }
        finish(check(in, in.next));
        return;
    }
    case K::lea: {
        const Addr a = address(in, pre);
        static const char* f[4] = {"c->x", "c->y", "c->s", "c->u"};
        const std::string v = a.constant ? hexu(a.value) : a.expr;
        body += std::string(f[in.reg]) + " = (uint16_t)(" + v + "); ";
        if (in.reg < 2 && !nf) body += "vxg_flags(c, " + L + ", (" + v + ") ? 0u : 4u); ";
        finish(check(in, in.next));
        return;
    }
    case K::psh:
        if (in.imm) push(in.reg, in.imm);
        finish(check(in, in.next));
        return;
    case K::pul:
        if (in.imm) pull(in.reg, in.imm);
        if (in.imm & 0x80) {
            body += "pc_ = q_; ";
            finish(" hint_ = -1; goto out;");
            return;
        }
        finish(check(in, in.next));
        return;
    case K::rts:
        pull(false, 0x80);
        body += "pc_ = q_; ";
        finish(" hint_ = -1; goto out;");
        return;
    case K::abx: body += "c->x = (uint16_t)(c->x + c->b); "; finish(check(in, in.next)); return;
    case K::mul:
        body += "const uint32_t m_ = (uint32_t)c->a * c->b; VXG_SETD(m_);";
        if (!nf) body += " vxg_flags(c, " + L + ", (m_ ? 0u : 4u) | ((m_ >> 7) & 1u)); ";
        body += " ";
        finish(check(in, in.next));
        return;
    case K::none:
        break;
    }
    o << "    " << exit_to(in, in.addr) << "\n";
}

void Emitter::emit_region(std::ostream& o, int r) {
    const auto& addrs = regions_[size_t(r)];
    const uint16_t first = addrs.front();
    o << "\nstatic int r_" << r << "(Vectrex *v, uint32_t stop, uint32_t off) {\n"
      << "    VXG_PROLOGUE();\n    switch (off) {\n";
    for (uint16_t a : addrs) {
        const Ins& in = ins_.at(a);
        if (in.label) o << "    case " << (a - first) << ": goto L_" << hex(a).substr(2) << ";\n";
    }
    o << "    default: return -2;\n    }\n";
    for (size_t i = 0; i < addrs.size(); i++) {
        const Ins& in = ins_.at(addrs[i]);
        if (in.label) {
            o << "L_" << hex(in.addr).substr(2) << ":";
            if (!opt_.precise)
                o << " if ((int32_t)(cycles - stop) >= 0) { pc_ = " << hexu(in.addr) << "; hint_ = " << r
                  << "; goto out; }";
            o << "\n";
        }
        emit_ins(o, in);
        if (i + 1 == addrs.size() && in.falls) o << "    " << go(in, in.next) << "\n";
    }
    o << "out:\n    c->pc = (uint16_t)pc_;\n    c->cycles = cycles;\n    return hint_;\n}\n";
}

std::string Emitter::run(const std::vector<CoverageEntry>& coverage, EmitStats& stats) {
    select(coverage, stats);
    liveness();
    plan();
    std::ostringstream o;
    o << "/* Generated by m0recomp --system vectrex" << (opt_.precise ? " --meta-precise" : "") << ". Do not edit. */\n"
      << "#include \"vx_gen.h\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-value\"\n"
      << "#if defined(__arm__) && !defined(VX_GEN_O2)\n"
      << "#pragma GCC optimize(\"Os\")\n"
      << "#endif\n\n"
      << "const uint32_t vx_gen_bios_sum = " << (bios_.empty() ? 0u : bios_sum(bios_)) << "u;\n";
    for (size_t r = 0; r < regions_.size(); r++)
        o << "static int r_" << r << "(Vectrex *v, uint32_t stop, uint32_t off) __attribute__((noinline));\n";
    for (size_t r = 0; r < regions_.size(); r++) emit_region(o, int(r));

    size_t entries = 0;
    for (const auto& [a, in] : ins_) entries += in.label;
    stats.regions = regions_.size();
    stats.entries = entries;
    const size_t n = std::max<size_t>(regions_.size(), 1);
    o << "\nconst unsigned vx_gen_regions = " << regions_.size() << ";\n"
      << "const vx_region_fn vx_gen_fn[" << n << "] = {";
    for (size_t r = 0; r < regions_.size(); r++) o << (r % 8 ? " " : "\n    ") << "r_" << r << ",";
    if (regions_.empty()) o << "0";
    o << "\n};\nconst uint16_t vx_gen_first[" << n << "] = {";
    for (size_t r = 0; r < regions_.size(); r++) o << (r % 12 ? " " : "\n    ") << hex(regions_[r].front()) << ",";
    if (regions_.empty()) o << "0xFFFF";
    o << "\n};\nconst uint16_t vx_gen_last[" << n << "] = {";
    for (size_t r = 0; r < regions_.size(); r++) o << (r % 12 ? " " : "\n    ") << hex(regions_[r].back()) << ",";
    if (regions_.empty()) o << "0";
    o << "\n};\n\n"
      << "/* the region whose span holds pc, or -1 */\n"
      << "int vx_gen_find(uint32_t pc) {\n"
      << "    int lo = 0, hi = (int)vx_gen_regions - 1;\n"
      << "    while (lo <= hi) {\n"
      << "        const int mid = (lo + hi) >> 1;\n"
      << "        if (pc < vx_gen_first[mid]) hi = mid - 1;\n"
      << "        else if (pc > vx_gen_last[mid]) lo = mid + 1;\n"
      << "        else return mid;\n"
      << "    }\n"
      << "    return -1;\n}\n";
    return o.str();
}

} // namespace

uint32_t bios_sum(const std::vector<uint8_t>& bios) {
    uint32_t h = 2166136261u;
    for (uint8_t b : bios) h = (h ^ b) * 16777619u;
    return h;
}

std::string emit_code(const std::vector<uint8_t>& cart, const std::vector<uint8_t>& bios,
                      const std::vector<CoverageEntry>& coverage, const EmitOptions& options, EmitStats& stats) {
    Emitter e(cart, bios, options);
    return e.run(coverage, stats);
}

} // namespace m0boy::vectrex
