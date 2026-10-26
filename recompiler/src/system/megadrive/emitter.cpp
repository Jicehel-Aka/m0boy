/* 68000 -> C for the Mega Drive PC runtime.
 *
 * Instructions reached from the vectors and the coverage profile are grouped
 * by address into regions, one C function each, with gotos inside and a
 * switch on the entry at the top.  Every exit returns to the dispatcher
 * (md_full_step_native): a known target in another region leaves its entry
 * as a hint for the lookup; RTS, RTE, JMP/JSR through a register and
 * exceptions leave m->pc to be looked up; what has no entry (code in RAM,
 * the rare instructions listed in decode(), what the walk did not reach)
 * is the interpreter's.
 *
 * The semantics are the interpreter's (md_full_cpu.c): the same helpers
 * (md_m68k_ops.h), the same cycle counts (md_m68k_time.h, included here so
 * the counts are folded from one table), the same order of bus accesses and
 * register side effects.  The slice may end after any instruction, so every
 * instruction is an entry and the budget is checked after each one, as the
 * interpreter's loop does; idle loops are spent at once like the
 * interpreter spends them (md_m68k_idle_loop, md_full_idle). */
#include "emitter.h"
#include "../../../../targets/megadrive/runtime/md_m68k_time.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <sstream>

namespace m0boy::megadrive {
namespace {

std::string hx(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%xu", v); return b; }
std::string lbl(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "L_%06x", v); return b; }
const char *mask_c(unsigned size) { return size == 1 ? "0xffu" : size == 2 ? "0xffffu" : "0xffffffffu"; }
const char *rd_c(unsigned size) { return size == 1 ? "md_rd8" : size == 2 ? "md_rd16" : "md_rd32"; }
const char *wr_c(unsigned size) { return size == 1 ? "md_wr8" : size == 2 ? "md_wr16" : "md_wr32"; }
const char *wr_cast(unsigned size) { return size == 1 ? "(uint8_t)" : size == 2 ? "(uint16_t)" : ""; }

/* An operand: mode/reg as encoded, with what its extension words hold. */
struct Ea {
    unsigned mode = 0, reg = 0;
    uint32_t value = 0;     /* address (abs, PC-relative), displacement, or immediate */
    uint16_t ext = 0;       /* brief extension word of the indexed modes */
    bool constant_addr() const { return mode == 7 && reg <= 2; }
    bool memory() const { return mode >= 2 && !(mode == 7 && reg == 4); }
};

struct Ins {
    uint32_t addr = 0, next = 0, target = 0;
    uint16_t op = 0;
    bool has_target = false, falls = true;
    int region = -1;
    unsigned local = 0, global = 0;
};

class Emitter {
public:
    Emitter(const std::vector<uint8_t> &rom) : rom_(rom) {}
    std::string run(const std::set<uint32_t> &seeds, EmitStats &stats);

private:
    const std::vector<uint8_t> &rom_;
    std::map<uint32_t, Ins> ins_;
    std::vector<std::vector<uint32_t>> regions_;
    unsigned tmp_ = 0;

    bool in_rom(uint32_t at, uint32_t bytes) const { return at + bytes <= rom_.size(); }
    uint16_t w16(uint32_t at) const { return uint16_t(rom_[at] << 8 | rom_[at + 1]); }
    uint32_t w32(uint32_t at) const { return uint32_t(w16(at)) << 16 | w16(at + 2); }
    bool parse(uint32_t &at, unsigned mode, unsigned reg, unsigned size, Ea &e) const;
    bool decode(uint32_t pc, Ins &in, std::string *code);
    void select(const std::set<uint32_t> &seeds);
    void plan();
    std::string go(const Ins &from, uint32_t to) const;
    std::string fresh(const char *p) { return std::string(p) + std::to_string(tmp_++); }
    std::string address(const Ea &e, unsigned size, std::string &pre);
    std::string load(const Ea &e, unsigned size, std::string &pre, std::string *addr = nullptr);
    std::string store(const Ea &e, unsigned size, const std::string &value, const std::string &addr);
};

bool Emitter::parse(uint32_t &at, unsigned mode, unsigned reg, unsigned size, Ea &e) const {
    e = Ea{};
    e.mode = mode; e.reg = reg;
    switch (mode) {
    case 0: case 1: case 2: case 3: case 4: return true;
    case 5:
        if (!in_rom(at, 2)) return false;
        e.value = uint32_t(int32_t(int16_t(w16(at)))); at += 2; return true;
    case 6:
        if (!in_rom(at, 2)) return false;
        e.ext = w16(at); at += 2;
        return !(e.ext & 0x0700u);              /* 68000 brief format only */
    default:
        switch (reg) {
        case 0: if (!in_rom(at, 2)) return false; e.value = uint32_t(int32_t(int16_t(w16(at)))); at += 2; return true;
        case 1: if (!in_rom(at, 4)) return false; e.value = w32(at); at += 4; return true;
        case 2: if (!in_rom(at, 2)) return false; e.value = at + uint32_t(int32_t(int16_t(w16(at)))); at += 2; return true;
        case 3: if (!in_rom(at, 2)) return false; e.value = at; e.ext = w16(at); at += 2; return !(e.ext & 0x0700u);
        case 4:
            if (size == 4) { if (!in_rom(at, 4)) return false; e.value = w32(at); at += 4; }
            else { if (!in_rom(at, 2)) return false; e.value = w16(at) & (size == 1 ? 0xffu : 0xffffu); at += 2; }
            return true;
        default: return false;
        }
    }
}

/* The address of a memory operand, as a constant or a local computed in
 * `pre` at this point (side effects of (An)+ and -(An) included). */
std::string Emitter::address(const Ea &e, unsigned size, std::string &pre) {
    const unsigned step = size == 1 && e.reg == 7 ? 2 : size;
    const std::string a = fresh("a");
    auto index = [&](void) {
        const unsigned x = (e.ext >> 12) & 7u;
        std::string r = std::string(e.ext & 0x8000u ? "m->a[" : "m->d[") + std::to_string(x) + "]";
        if (!(e.ext & 0x0800u)) r = "(uint32_t)(int32_t)(int16_t)" + r;
        return r + " + " + hx(uint32_t(int32_t(int8_t(e.ext))));
    };
    switch (e.mode) {
    case 2: pre += "const uint32_t " + a + " = m->a[" + std::to_string(e.reg) + "]; "; break;
    case 3: pre += "const uint32_t " + a + " = m->a[" + std::to_string(e.reg) + "]; m->a[" + std::to_string(e.reg) +
                   "] += " + std::to_string(step) + "u; "; break;
    case 4: pre += "m->a[" + std::to_string(e.reg) + "] -= " + std::to_string(step) + "u; const uint32_t " + a +
                   " = m->a[" + std::to_string(e.reg) + "]; "; break;
    case 5: pre += "const uint32_t " + a + " = m->a[" + std::to_string(e.reg) + "] + " + hx(e.value) + "; "; break;
    case 6: pre += "const uint32_t " + a + " = m->a[" + std::to_string(e.reg) + "] + " + index() + "; "; break;
    default:
        if (e.reg == 3) pre += "const uint32_t " + a + " = " + hx(e.value) + " + " + index() + "; ";
        else return hx(e.value);
    }
    return a;
}

/* The operand's value (masked to `size`), read into a local in `pre`. */
std::string Emitter::load(const Ea &e, unsigned size, std::string &pre, std::string *addr) {
    switch (e.mode) {
    case 0: return "(m->d[" + std::to_string(e.reg) + "] & " + mask_c(size) + ")";
    case 1: return "(m->a[" + std::to_string(e.reg) + "] & " + mask_c(size) + ")";
    default: break;
    }
    if (e.mode == 7 && e.reg == 4) return hx(e.value);
    const std::string a = address(e, size, pre);
    if (addr) *addr = a;
    const std::string v = fresh("v");
    /* constant ROM words fold (md_rd* would read the same bytes) */
    if (e.constant_addr() && (size == 1 || !(e.value & 1u)) && (e.value & 0xffffffu) + size <= rom_.size()) {
        const uint32_t at = e.value & 0xffffffu;
        const uint32_t k = size == 1 ? rom_[at] : size == 2 ? w16(at) : w32(at);
        pre += "const uint32_t " + v + " = " + hx(k) + "; ";
        return v;
    }
    pre += "const uint32_t " + v + " = " + rd_c(size) + "(m, " + a + "); ";
    return v;
}

std::string Emitter::store(const Ea &e, unsigned size, const std::string &value, const std::string &addr) {
    if (e.mode == 0) return "md_setd(m, " + std::to_string(e.reg) + ", " + value + ", " + std::to_string(size) + "); ";
    if (e.mode == 1) return "m->a[" + std::to_string(e.reg) + "] = md_sext(" + value + ", " + std::to_string(size) + "); ";
    return std::string(wr_c(size)) + "(m, " + addr + ", " + wr_cast(size) + "(" + value + ")); ";
}

/* Decodes the instruction at pc; with `code`, also writes its C (without
 * the time and the budget check, which run() adds).  False: left to the
 * interpreter. */
bool Emitter::decode(uint32_t pc, Ins &in, std::string *code) {
    if ((pc & 1u) || !in_rom(pc, 2)) return false;
    in = Ins{};
    in.addr = pc;
    const uint16_t op = in.op = w16(pc);
    const unsigned top = op >> 12, reg = (op >> 9) & 7u, mode = (op >> 3) & 7u, ry = op & 7u;
    const std::string R = std::to_string(reg), RY = std::to_string(ry);
    uint32_t at = pc + 2;
    std::string c;
    Ea s, d;
    auto finish = [&](void) { in.next = at; if (code) *code = c; return true; };
    auto alterable = [&](const Ea &e) { return !(e.mode == 7 && e.reg >= 2); };
    auto data_alterable = [&](const Ea &e) { return e.mode != 1 && alterable(e); };
    auto memory_alterable = [&](const Ea &e) { return e.mode >= 2 && alterable(e); };
    auto control = [&](const Ea &e) { return md_m68k_control(e.mode, e.reg) >= 0; };
    switch (top) {
    case 1: case 2: case 3: {
        const unsigned size = top == 1 ? 1 : top == 2 ? 4 : 2, dmode = (op >> 6) & 7u;
        if (!parse(at, mode, ry, size, s) || (size == 1 && mode == 1)) return false;
        if (!parse(at, dmode, reg, size, d) || !alterable(d) || (size == 1 && dmode == 1)) return false;
        std::string pre;
        const std::string v = load(s, size, pre);
        if (dmode == 1) c = pre + "m->a[" + R + "] = md_sext(" + v + ", " + std::to_string(size) + ");";
        else {
            std::string a;
            if (d.memory()) a = address(d, size, pre);
            c = pre + "{ const uint32_t r_ = " + v + "; " + store(d, size, "r_", a) + "md_logic(m, r_, " +
                std::to_string(size) + "); }";
        }
        return finish();
    }
    case 7:
        if (op & 0x0100u) return false;
        c = "m->d[" + R + "] = " + hx(uint32_t(int32_t(int8_t(op)))) + "; md_logic(m, m->d[" + R + "], 4);";
        return finish();
    case 6: {
        const unsigned cc = (op >> 8) & 15u;
        int32_t disp = int8_t(op);
        if (!(op & 255u)) { if (!in_rom(at, 2)) return false; disp = int16_t(w16(at)); at += 2; }
        if ((op & 255u) == 0xffu) return false;               /* .l is 68020 */
        in.target = pc + 2 + uint32_t(disp);
        in.has_target = true;
        if (cc == 0) in.falls = false;
        if (code) *code = "";
        in.next = at;
        return !(in.target & 1u);
    }
    case 5:
        if ((op & 0x00c0u) == 0x00c0u) {
            const unsigned cc = (op >> 8) & 15u;
            if (mode == 1) {
                if (!in_rom(at, 2)) return false;
                in.target = at + uint32_t(int32_t(int16_t(w16(at))));
                at += 2;
                in.has_target = true;
                if (code) *code = "";
                in.next = at;
                return !(in.target & 1u);
            }
            if (!parse(at, mode, ry, 1, d) || !data_alterable(d)) return false;
            std::string pre, a;
            if (d.memory()) a = address(d, 1, pre);
            c = "{ const int t_ = md_cc(m, " + std::to_string(cc) + "); " +
                (mode == 0 ? "if (t_) fc_ += MD_SCC_TRUE_DN; " : "") + pre +
                store(d, 1, "t_ ? 0xffu : 0u", a) + "}";
            return finish();
        } else {
            const unsigned size = 1u << ((op >> 6) & 3u), q = reg ? reg : 8;
            const bool sub = op & 0x0100u;
            if (mode == 1) {
                if (size == 1) return false;
                c = "m->a[" + RY + "] " + (sub ? "-= " : "+= ") + std::to_string(q) + "u;";
                return finish();
            }
            if (!parse(at, mode, ry, size, d) || !data_alterable(d)) return false;
            std::string pre, a;
            const std::string v = load(d, size, pre, &a);
            c = pre + "{ const uint32_t r_ = " + (sub ? "md_sub" : "md_add") + "(m, " + v + ", " + std::to_string(q) +
                "u, " + std::to_string(size) + "); " + store(d, size, "r_", a) + "}";
            return finish();
        }
    case 0: {
        if ((op & 0x0138u) == 0x0108u) {                     /* MOVEP */
            if (!in_rom(at, 2)) return false;
            const unsigned n = (op & 0x0040u) ? 4 : 2;
            c = "{ const uint32_t p_ = m->a[" + RY + "] + " + hx(uint32_t(int32_t(int16_t(w16(at))))) + "; ";
            at += 2;
            if (op & 0x0080u)
                for (unsigned i = 0; i < n; ++i)
                    c += "md_wr8(m, p_ + " + std::to_string(2 * i) + "u, (uint8_t)(m->d[" + R + "] >> " + std::to_string(8 * (n - 1 - i)) + ")); ";
            else {
                c += "uint32_t v_ = 0; ";
                for (unsigned i = 0; i < n; ++i) c += "v_ = v_ << 8 | md_rd8(m, p_ + " + std::to_string(2 * i) + "u); ";
                c += "md_setd(m, " + R + ", v_, " + std::to_string(n) + "); ";
            }
            c += "}";
            return finish();
        }
        if ((op & 0x0100u) || (op & 0x0f00u) == 0x0800u) {
            const unsigned kind = (op >> 6) & 3u, size = mode == 0 ? 4 : 1;
            std::string bit;
            if (op & 0x0100u) bit = "m->d[" + R + "]";
            else { if (!in_rom(at, 2)) return false; bit = hx(w16(at) & 0xffu); at += 2; }
            if (!parse(at, mode, ry, size, d) || d.mode == 1) return false;
            if (kind && !data_alterable(d)) return false;
            if (!kind && d.mode == 7 && d.reg == 4 && !(op & 0x0100u)) return false;
            std::string pre, a;
            const std::string v = load(d, size, pre, &a);
            c = "{ const uint32_t b_ = 1u << (" + bit + " & " + (size == 4 ? "31u" : "7u") + "); " + pre +
                "m->sr = (uint16_t)((m->sr & ~MD_Z) | (" + v + " & b_ ? 0 : MD_Z)); ";
            if (kind) c += store(d, size, kind == 1 ? v + " ^ b_" : kind == 2 ? v + " & ~b_" : v + " | b_", a);
            c += "}";
            return finish();
        }
        if ((op & 0xffbfu) == 0x003cu || (op & 0xffbfu) == 0x023cu || (op & 0xffbfu) == 0x0a3cu) {
            if (!in_rom(at, 2)) return false;
            const uint16_t v = w16(at); at += 2;
            const unsigned k = (op >> 9) & 7u;
            const char *o = k == 0 ? " | " : k == 1 ? " & " : " ^ ";
            if (op & 0x0040u) c = std::string("md_set_sr(m, (uint16_t)(m->sr") + o + hx(v) + "));";
            else c = std::string("m->sr = (uint16_t)(m->sr") + o + hx(k == 1 ? 0xffe0u | (v & 0x1fu) : v & 0x1fu) + ");";
            return finish();
        }
        const unsigned k = (op >> 9) & 7u;
        unsigned size = (op >> 6) & 3u;
        if (size == 3 || k == 4 || k == 7) return false;
        size = 1u << size;
        uint32_t imm;
        if (size == 4) { if (!in_rom(at, 4)) return false; imm = w32(at); at += 4; }
        else { if (!in_rom(at, 2)) return false; imm = w16(at) & (size == 1 ? 0xffu : 0xffffu); at += 2; }
        if (!parse(at, mode, ry, size, d) || !data_alterable(d)) return false;
        std::string pre, a;
        const std::string v = load(d, size, pre, &a), S = std::to_string(size), I = hx(imm);
        switch (k) {
        case 0: c = pre + "{ const uint32_t r_ = " + v + " | " + I + "; " + store(d, size, "r_", a) + "md_logic(m, r_, " + S + "); }"; break;
        case 1: c = pre + "{ const uint32_t r_ = " + v + " & " + I + "; " + store(d, size, "r_", a) + "md_logic(m, r_, " + S + "); }"; break;
        case 2: c = pre + "{ const uint32_t r_ = md_sub(m, " + v + ", " + I + ", " + S + "); " + store(d, size, "r_", a) + "}"; break;
        case 3: c = pre + "{ const uint32_t r_ = md_add(m, " + v + ", " + I + ", " + S + "); " + store(d, size, "r_", a) + "}"; break;
        case 5: c = pre + "{ const uint32_t r_ = " + v + " ^ " + I + "; " + store(d, size, "r_", a) + "md_logic(m, r_, " + S + "); }"; break;
        default: c = pre + "md_cmp(m, " + v + ", " + I + ", " + S + ");"; break;
        }
        return finish();
    }
    case 4: {
        switch (op) {
        case 0x4e70: case 0x4e71: return finish();
        case 0x4e72:
            if (!in_rom(at, 2)) return false;
            c = "md_set_sr(m, " + hx(w16(at)) + "); m->stopped = 1;"; at += 2;
            return finish();
        case 0x4e73:
            c = "{ const uint16_t s_ = md_pop16(m); m->pc = md_pop32(m); md_set_sr(m, s_); } goto dyn;";
            in.falls = false; return finish();
        case 0x4e75:
            c = "m->pc = md_pop32(m); goto dyn;";
            in.falls = false; return finish();
        default: break;
        }
        if ((op & 0xfff0u) == 0x4e40u) {
            /* its handler usually returns to the next instruction: walk on */
            c = "md_exception(m, " + std::to_string(32 + (op & 15u)) + ", " + hx(at) + "); goto dyn;";
            return finish();
        }
        if (op == 0x4e76u) {
            c = "if (m->sr & MD_V) { fc_ += 30u; MDFLUSH(); md_exception(m, 7, " + hx(at) + "); goto dyn; }";
            return finish();
        }
        if (op == 0x4e77u) {
            c = "{ const uint16_t c_ = md_pop16(m); m->sr = (uint16_t)((m->sr & 0xff00u) | (c_ & 0x1fu)); m->pc = md_pop32(m); } goto dyn;";
            in.falls = false; return finish();
        }
        if ((op & 0xfff8u) == 0x4e50u) {
            if (!in_rom(at, 2)) return false;
            c = "md_push32(m, m->a[" + RY + "]); m->a[" + RY + "] = m->a[7]; m->a[7] += " +
                hx(uint32_t(int32_t(int16_t(w16(at))))) + ";";
            at += 2; return finish();
        }
        if ((op & 0xfff8u) == 0x4e58u) { c = "m->a[7] = m->a[" + RY + "]; m->a[" + RY + "] = md_pop32(m);"; return finish(); }
        if ((op & 0xfff0u) == 0x4e60u) {
            c = (op & 8u) ? "m->a[" + RY + "] = m->usp;" : "m->usp = m->a[" + RY + "];";
            return finish();
        }
        if ((op & 0xffc0u) == 0x4e80u || (op & 0xffc0u) == 0x4ec0u) {
            if (!parse(at, mode, ry, 4, d) || !control(d)) return false;
            const bool call = !(op & 0x0040u);
            if (d.constant_addr()) {
                in.target = d.value;
                in.has_target = true;
                in.falls = call;
                in.next = at;
                if (code) *code = call ? "md_push32(m, " + hx(at) + ");" : "";
                return !(in.target & 1u);
            }
            std::string pre;
            const std::string a = address(d, 4, pre);
            c = pre + (call ? "md_push32(m, " + hx(at) + "); " : "") + "m->pc = " + a + "; goto dyn;";
            in.falls = false;
            return finish();
        }
        if ((op & 0xf1c0u) == 0x41c0u) {
            if (!parse(at, mode, ry, 4, d) || !control(d)) return false;
            std::string pre;
            const std::string a = address(d, 4, pre);
            c = pre + "m->a[" + R + "] = " + a + ";";
            return finish();
        }
        if ((op & 0xf1c0u) == 0x4180u) {                     /* CHK.W */
            if (!parse(at, mode, ry, 2, s) || s.mode == 1) return false;
            std::string pre;
            const std::string v = load(s, 2, pre);
            c = pre + "{ const int16_t b_ = (int16_t)" + v + ", x_ = (int16_t)m->d[" + R + "]; if (x_ < 0 || x_ > b_) { "
                "m->sr = (uint16_t)((m->sr & ~MD_N) | (x_ < 0 ? MD_N : 0)); fc_ += 30u; MDFLUSH(); md_exception(m, 6, " + hx(at) + "); goto dyn; } }";
            return finish();
        }
        if ((op & 0xfff8u) == 0x4840u) {
            c = "{ const uint32_t v_ = m->d[" + RY + "]; m->d[" + RY + "] = v_ << 16 | v_ >> 16; md_logic(m, m->d[" + RY + "], 4); }";
            return finish();
        }
        if ((op & 0xffc0u) == 0x4840u) {
            if (!parse(at, mode, ry, 4, d) || !control(d)) return false;
            std::string pre;
            const std::string a = address(d, 4, pre);
            c = pre + "md_push32(m, " + a + ");";
            return finish();
        }
        if ((op & 0xffb8u) == 0x4880u) {
            c = (op & 0x0040u) ? "m->d[" + RY + "] = (uint32_t)(int32_t)(int16_t)m->d[" + RY + "]; md_logic(m, m->d[" + RY + "], 4);"
                               : "md_setd(m, " + RY + ", (uint32_t)(int32_t)(int8_t)m->d[" + RY + "], 2); md_logic(m, m->d[" + RY + "], 2);";
            return finish();
        }
        if ((op & 0xfb80u) == 0x4880u) {                     /* MOVEM, unrolled */
            const unsigned size = (op & 0x0040u) ? 4 : 2;
            const bool to_regs = op & 0x0400u;
            if (!in_rom(at, 2)) return false;
            const uint16_t list = w16(at); at += 2;
            unsigned count = 0;
            for (uint16_t l = list; l; l &= uint16_t(l - 1)) ++count;
            const std::string S = std::to_string(size), extra = "fc_ += " + std::to_string(count * (size == 4 ? 8 : 4)) + "u; ";
            if (mode == 4) {
                if (to_regs) return false;
                c = extra + "{ uint32_t p_ = m->a[" + RY + "]; ";
                for (unsigned r = 0; r < 16; ++r) if (list & (1u << r))
                    c += "p_ -= " + S + "u; " + wr_c(size) + "(m, p_, " + wr_cast(size) + "(" +
                         (r < 8 ? "m->a[" + std::to_string(7 - r) + "]" : "m->d[" + std::to_string(15 - r) + "]") + ")); ";
                c += "m->a[" + RY + "] = p_; }";
                return finish();
            }
            if (mode == 3 && !to_regs) return false;
            if (mode != 3) {
                if (!parse(at, mode, ry, 4, d) || !control(d)) return false;
                if (!to_regs && (d.mode == 7 && d.reg >= 2)) return false;
            }
            std::string pre;
            const std::string start = mode == 3 ? "m->a[" + RY + "]" : address(d, 4, pre);
            c = extra + "{ " + pre + "uint32_t p_ = " + start + "; ";
            for (unsigned r = 0; r < 16; ++r) if (list & (1u << r)) {
                const std::string regname = r < 8 ? "m->d[" + std::to_string(r) + "]" : "m->a[" + std::to_string(r - 8) + "]";
                if (to_regs) c += regname + " = md_sext(" + rd_c(size) + "(m, p_), " + S + "); p_ += " + S + "u; ";
                else c += std::string(wr_c(size)) + "(m, p_, " + wr_cast(size) + "(" + regname + ")); p_ += " + S + "u; ";
            }
            if (mode == 3) c += "m->a[" + RY + "] = p_; ";
            c += "}";
            return finish();
        }
        if ((op & 0xffc0u) == 0x40c0u) {
            if (!parse(at, mode, ry, 2, d) || !data_alterable(d)) return false;
            std::string pre, a;
            if (d.memory()) a = address(d, 2, pre);
            c = pre + store(d, 2, "m->sr", a);
            return finish();
        }
        if ((op & 0xffc0u) == 0x44c0u || (op & 0xffc0u) == 0x46c0u) {
            if (!parse(at, mode, ry, 2, s) || s.mode == 1) return false;
            std::string pre;
            const std::string v = load(s, 2, pre);
            c = pre + ((op & 0x0200u) ? "md_set_sr(m, (uint16_t)" + v + ");"
                                      : "m->sr = (uint16_t)((m->sr & 0xff00u) | (" + v + " & 0x1fu));");
            return finish();
        }
        if ((op & 0xffc0u) == 0x4800u) {                     /* NBCD */
            if (!parse(at, mode, ry, 1, d) || !data_alterable(d)) return false;
            std::string pre, a;
            const std::string v = load(d, 1, pre, &a);
            c = pre + store(d, 1, "md_sbcd(m, 0, (uint8_t)" + v + ")", a);
            return finish();
        }
        if ((op & 0xffc0u) == 0x4ac0u) {
            if (!parse(at, mode, ry, 1, d) || !data_alterable(d)) return false;
            std::string pre, a;
            const std::string v = load(d, 1, pre, &a);
            c = pre + "md_logic(m, " + v + ", 1); " + store(d, 1, v + " | 0x80u", a);
            return finish();
        }
        const unsigned k = (op >> 8) & 15u;
        unsigned size = (op >> 6) & 3u;
        if (size == 3 || (k != 0 && k != 2 && k != 4 && k != 6 && k != 10)) return false;
        size = 1u << size;
        if (!parse(at, mode, ry, size, d)) return false;
        if (k == 10 ? (d.mode == 1 && size == 1) : !data_alterable(d)) return false;
        std::string pre, a;
        const std::string v = load(d, size, pre, &a), S = std::to_string(size);
        switch (k) {
        case 0: c = pre + "{ const uint32_t r_ = md_subx(m, 0, " + v + ", " + S + "); " + store(d, size, "r_", a) + "}"; break;
        case 2: c = pre + store(d, size, "0u", a) + "md_logic(m, 0, " + S + ");"; break;
        case 4: c = pre + "{ const uint32_t r_ = md_sub(m, 0, " + v + ", " + S + "); " + store(d, size, "r_", a) + "}"; break;
        case 6: c = pre + "{ const uint32_t r_ = ~" + v + "; " + store(d, size, "r_", a) + "md_logic(m, r_, " + S + "); }"; break;
        default: c = pre + "md_logic(m, " + v + ", " + S + ");"; break;
        }
        return finish();
    }
    case 8: case 9: case 11: case 12: case 13: {
        const unsigned opmode = (op >> 6) & 7u;
        if ((top == 9 || top == 11 || top == 13) && (opmode == 3 || opmode == 7)) {
            const unsigned size = opmode == 3 ? 2 : 4;
            if (!parse(at, mode, ry, size, s)) return false;
            std::string pre;
            const std::string v = "md_sext(" + load(s, size, pre) + ", " + std::to_string(size) + ")";
            c = pre + (top == 9 ? "m->a[" + R + "] -= " + v + ";" : top == 13 ? "m->a[" + R + "] += " + v + ";"
                                                                    : "md_cmp(m, m->a[" + R + "], " + v + ", 4);");
            return finish();
        }
        if ((top == 8 || top == 12) && (opmode == 3 || opmode == 7)) {
            if (!parse(at, mode, ry, 2, s) || s.mode == 1) return false;
            std::string pre;
            const std::string v = "(uint16_t)" + load(s, 2, pre);
            if (top == 12) c = pre + (opmode == 3 ? "md_mulu" : "md_muls") + std::string("(m, ") + R + ", " + v +
                  "); fc_ = st_ = m->frame_cycles;";
            else c = pre + "if (!" + (opmode == 3 ? "md_divu" : "md_divs") + "(m, " + R + ", " + v + ", " + hx(at) +
                  ")) { fc_ = st_ = m->frame_cycles; goto dyn; } fc_ = st_ = m->frame_cycles;";
            return finish();
        }
        if ((top == 8 || top == 12) && opmode == 4 && mode <= 1) {   /* SBCD/ABCD */
            const std::string f = top == 8 ? "md_sbcd" : "md_abcd";
            if (!mode) c = "md_setd(m, " + R + ", " + f + "(m, (uint8_t)m->d[" + R + "], (uint8_t)m->d[" + RY + "]), 1);";
            else c = "{ m->a[" + RY + "] -= " + (ry == 7 ? "2u" : "1u") + "; const uint8_t s_ = md_rd8(m, m->a[" + RY + "]); m->a[" + R +
                     "] -= " + (reg == 7 ? "2u" : "1u") + "; const uint8_t d_ = md_rd8(m, m->a[" + R + "]); md_wr8(m, m->a[" + R + "], " + f +
                     "(m, d_, s_)); }";
            return finish();
        }
        if (top == 12 && (op & 0x0130u) == 0x0100u && (opmode == 5 || opmode == 6)) {
            const std::string x = opmode == 5 && mode == 1 ? "m->a[" + R + "]" : "m->d[" + R + "]";
            const std::string y = mode == 1 ? "m->a[" + RY + "]" : "m->d[" + RY + "]";
            c = "{ const uint32_t t_ = " + x + "; " + x + " = " + y + "; " + y + " = t_; }";
            return finish();
        }
        if ((top == 9 || top == 13) && opmode >= 4 && opmode <= 6 && mode <= 1) {
            const unsigned size = 1u << (opmode - 4);
            const std::string S = std::to_string(size), f = top == 9 ? "md_subx" : "md_addx";
            if (!mode) c = "md_setd(m, " + R + ", " + f + "(m, m->d[" + R + "], m->d[" + RY + "], " + S + "), " + S + ");";
            else {
                const std::string sy = std::to_string(size == 1 && ry == 7 ? 2 : size), sx = std::to_string(size == 1 && reg == 7 ? 2 : size);
                c = "{ m->a[" + RY + "] -= " + sy + "u; const uint32_t s_ = " + rd_c(size) + "(m, m->a[" + RY + "]); m->a[" + R +
                    "] -= " + sx + "u; const uint32_t d_ = " + rd_c(size) + "(m, m->a[" + R + "]); " + wr_c(size) + "(m, m->a[" + R +
                    "], " + wr_cast(size) + "(" + f + "(m, d_, s_, " + S + "))); }";
            }
            return finish();
        }
        if (top == 11 && opmode >= 4 && opmode <= 6 && mode == 1) {
            const unsigned size = 1u << (opmode - 4);
            const std::string sy = std::to_string(size == 1 && ry == 7 ? 2 : size), sx = std::to_string(size == 1 && reg == 7 ? 2 : size);
            c = "{ const uint32_t s_ = " + std::string(rd_c(size)) + "(m, m->a[" + RY + "]); m->a[" + RY + "] += " + sy +
                "u; const uint32_t d_ = " + rd_c(size) + "(m, m->a[" + R + "]); m->a[" + R + "] += " + sx +
                "u; md_cmp(m, d_, s_, " + std::to_string(size) + "); }";
            return finish();
        }
        if (top == 11 && opmode >= 4 && opmode <= 6) {
            const unsigned size = 1u << (opmode - 4);
            if (!parse(at, mode, ry, size, d) || !data_alterable(d)) return false;
            std::string pre, a;
            const std::string v = load(d, size, pre, &a);
            c = pre + "{ const uint32_t r_ = " + v + " ^ m->d[" + R + "]; " + store(d, size, "r_", a) + "md_logic(m, r_, " +
                std::to_string(size) + "); }";
            return finish();
        }
        if (opmode == 3 || opmode == 7) return false;
        const unsigned size = 1u << (opmode & 3u);
        const std::string S = std::to_string(size);
        if (top == 11) {
            if (!parse(at, mode, ry, size, s) || (size == 1 && mode == 1)) return false;
            std::string pre;
            const std::string v = load(s, size, pre);
            c = pre + "md_cmp(m, m->d[" + R + "], " + v + ", " + S + ");";
            return finish();
        }
        auto op_c = [&](const std::string &x, const std::string &y) {
            if (top == 8) return "{ const uint32_t r_ = " + x + " | " + y + "; md_logic(m, r_, " + S + "); ";
            if (top == 12) return "{ const uint32_t r_ = " + x + " & " + y + "; md_logic(m, r_, " + S + "); ";
            return "{ const uint32_t r_ = " + std::string(top == 9 ? "md_sub" : "md_add") + "(m, " + x + ", " + y + ", " + S + "); ";
        };
        if (opmode <= 2) {
            if (!parse(at, mode, ry, size, s) || (mode == 1 && (size == 1 || top == 8 || top == 12))) return false;
            std::string pre;
            const std::string v = load(s, size, pre);
            c = pre + op_c("m->d[" + R + "]", v) + "md_setd(m, " + R + ", r_, " + S + "); }";
        } else {
            if (!parse(at, mode, ry, size, d) || !memory_alterable(d)) return false;
            std::string pre, a;
            const std::string v = load(d, size, pre, &a);
            c = pre + op_c(v, "m->d[" + R + "]") + store(d, size, "r_", a) + "}";
        }
        return finish();
    }
    case 14: {
        if (((op >> 6) & 3u) == 3u) {
            if (op & 0x0800u) return false;
            if (!parse(at, mode, ry, 2, d) || !memory_alterable(d)) return false;
            std::string pre, a;
            const std::string v = load(d, 2, pre, &a);
            c = pre + store(d, 2, "md_shift(m, " + std::to_string((op >> 9) & 3u) + ", " + ((op & 0x0100u) ? "1" : "0") +
                                  ", " + v + ", 1, 2)", a);
            return finish();
        }
        const unsigned size = 1u << ((op >> 6) & 3u);
        const std::string S = std::to_string(size), type = std::to_string((op >> 3) & 3u), left = (op & 0x0100u) ? "1" : "0";
        if (op & 0x0020u)
            c = "{ const unsigned n_ = m->d[" + R + "] & 63u; fc_ += 2u * n_; md_setd(m, " + RY + ", md_shift(m, " + type +
                ", " + left + ", m->d[" + RY + "], n_, " + S + "), " + S + "); }";
        else
            c = "md_setd(m, " + RY + ", md_shift(m, " + type + ", " + left + ", m->d[" + RY + "], " + std::to_string(reg ? reg : 8) +
                ", " + S + "), " + S + ");";
        return finish();
    }
    default:
        return false;
    }
}

void Emitter::select(const std::set<uint32_t> &seeds) {
    std::deque<uint32_t> work(seeds.begin(), seeds.end());
    std::set<uint32_t> seen;
    while (!work.empty()) {
        const uint32_t at = work.front();
        work.pop_front();
        if (!seen.insert(at).second) continue;
        Ins in;
        if (!decode(at, in, nullptr)) continue;
        ins_[at] = in;
        if (in.falls) work.push_back(in.next);
        if (in.has_target) work.push_back(in.target);
    }
}

void Emitter::plan() {
    /* regions: runs of neighbouring instructions; every instruction is an
     * entry, since the slice may end after any of them */
    const size_t cap = 256;
    for (auto &[a, in] : ins_) {
        const bool joins = !regions_.empty() && regions_.back().size() < cap && ins_.at(regions_.back().back()).next == a;
        if (!joins) regions_.emplace_back();
        in.region = int(regions_.size() - 1);
        in.local = unsigned(regions_.back().size());
        regions_.back().push_back(a);
    }
    unsigned global = 0;
    for (auto &[a, in] : ins_) in.global = global++;
}

std::string Emitter::go(const Ins &from, uint32_t to) const {
    auto p = ins_.find(to);
    if (p == ins_.end()) return "{ pc_ = " + hx(to) + "; goto out; }";
    if (p->second.region == from.region) return "goto " + lbl(to) + ";";
    return "{ pc_ = " + hx(to) + "; md_gen_next = " + std::to_string(p->second.global) + "; goto out; }";
}

std::string Emitter::run(const std::set<uint32_t> &seeds, EmitStats &stats) {
    select(seeds);
    plan();
    std::ostringstream o;
    o << "/* Generated by m0recomp --system megadrive --target pc. Do not edit. */\n"
         "#include \"md_m68k_ops.h\"\n"
         "#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
         "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
         "/* the region's own cycle shadow: fc_ adds what the interpreter would\n"
         " * spend, and the clock reaches memory only at exits and before any\n"
         " * helper that reads it (VDP status, HV counter, Z80 sync) or spends\n"
         " * by itself (mul, div, idle, exception) */\n"
         "#define OVER (fc_ >= m->slice_limit)\n"
         "#define MDFLUSH() (m->cycles += fc_ - st_, m->frame_cycles = fc_, st_ = fc_)\n\n"
         "static int md_gen_next = -1;   /* the entry a region left for the dispatcher */\n";
    for (size_t r = 0; r < regions_.size(); ++r)
        o << "static void r_" << r << "(MdFull *m, unsigned entry) __attribute__((noinline));\n";
    for (size_t r = 0; r < regions_.size(); ++r) {
        const auto &addrs = regions_[r];
        o << "\nstatic void r_" << r << "(MdFull *m, unsigned entry) {\n"
          << "    uint32_t pc_;\n    uint32_t fc_ = m->frame_cycles, st_ = fc_;\n    switch (entry) {\n";
        for (uint32_t a : addrs) o << "    case " << ins_.at(a).local << ": goto " << lbl(a) << ";\n";
        o << "    default: __builtin_unreachable();\n    }\n";
        for (size_t i = 0; i < addrs.size(); ++i) {
            Ins in = ins_.at(addrs[i]);
            std::string body;
            decode(in.addr, in, &body);
            in.region = int(r);
            const unsigned top = in.op >> 12;
            const unsigned base = md_m68k_cycles(in.op);
            unsigned extra = 0;
            /* a helper that reads the clock (VDP status, HV counter, the
             * Z80 sync behind a bus read) or spends by itself (mul, div,
             * idle, exception) sees the flushed one */
            static const char *const impure[] = {
                "md_rd8(", "md_rd16(", "md_rd32(", "md_wr8(", "md_wr16(", "md_wr32(",
                "md_push16(", "md_push32(", "md_pop16(", "md_pop32(",
                "md_exception(", "md_full_idle(", "md_sbcd(", "md_abcd(",
                "md_mulu(", "md_muls(", "md_divu(", "md_divs("};
            bool flushes = false;
            for (const char *h : impure)
                if (body.find(h) != std::string::npos) { flushes = true; break; }
            o << lbl(in.addr) << ": /* " << hx(in.op) << " */\n";
            /* MOVEM's per-register time is folded into its code */
            o << "    fc_ += " << base + extra << "u;";
            if (flushes) o << " MDFLUSH();";
            if (top == 6) {
                const unsigned cc = (in.op >> 8) & 15u;
                if (cc == 1) o << " MDFLUSH(); md_push32(m, " << hx(in.next) << ");";
                if (cc <= 1) {
                    o << " if (OVER) { pc_ = " << hx(in.target) << "; goto out; } " << go(in, in.target) << "\n";
                    continue;
                }
                o << " if (md_cc(m, " << cc << ")) { fc_ += (unsigned)" << md_m68k_branch_taken(in.op)
                  << "; if (OVER) { pc_ = " << hx(in.target) << "; goto out; } " << go(in, in.target) << " }\n";
            } else if (top == 5 && (in.op & 0x00f8u) == 0x00c8u) {
                const unsigned cc = (in.op >> 8) & 15u, dn = in.op & 7u;
                o << " if (!md_cc(m, " << cc << ")) { const uint16_t v_ = (uint16_t)(m->d[" << dn << "] - 1u); md_setd(m, "
                  << dn << ", v_, 2); if (v_ != 0xffffu) { fc_ += MD_DBCC_LOOP; if (OVER) { pc_ = "
                  << hx(in.target) << "; goto out; } " << go(in, in.target) << " } fc_ += MD_DBCC_EXPIRED; }\n";
            } else if (in.has_target) {                  /* JMP/JSR to a constant */
                o << " " << body << " if (OVER) { pc_ = " << hx(in.target) << "; goto out; } " << go(in, in.target) << "\n";
                continue;
            } else {
                o << " " << body << "\n";
                if (!in.falls) continue;
            }
            o << "    if (OVER) { pc_ = " << hx(in.next) << "; goto out; }\n";
            if (const unsigned len = md_m68k_idle_loop(rom_.data(), uint32_t(rom_.size()), in.addr)) {
                const uint16_t bop = w16(in.addr + len);
                o << "    if (md_cc(m, " << ((bop >> 8) & 15u) << ")) { MDFLUSH(); md_full_idle(m, " << base << "u, 10u, "
                  << hx(in.addr) << ", " << hx(in.addr + len) << "); return; }\n";
                ++stats.idle_loops;
            }
            if (i + 1 == addrs.size()) o << "    " << go(in, in.next) << "\n";
        }
        o << "out:\n    MDFLUSH(); m->pc = pc_; return;\ndyn:\n    MDFLUSH(); return;\n}\n";
    }
    std::vector<const Ins *> entries;
    for (const auto &[a, in] : ins_) entries.push_back(&in);
    stats.compiled = ins_.size();
    stats.regions = regions_.size();
    stats.entries = entries.size();
    const size_t count = std::max<size_t>(entries.size(), 1);
    o << "\ntypedef void (*md_region_fn)(MdFull *, unsigned);\nstatic const md_region_fn region_fn["
      << std::max<size_t>(regions_.size(), 1) << "] = {";
    for (size_t r = 0; r < regions_.size(); ++r) o << (r % 8 ? " " : "\n    ") << "r_" << r << ",";
    if (regions_.empty()) o << "0";
    o << "\n};\nstatic const uint32_t entry_pc[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); ++i) o << (i % 8 ? " " : "\n    ") << hx(entries[i]->addr) << ",";
    if (entries.empty()) o << "0xffffffffu";
    o << "\n};\nstatic const uint16_t entry_region[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); ++i) o << (i % 16 ? " " : "\n    ") << entries[i]->region << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint16_t entry_local[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); ++i) o << (i % 16 ? " " : "\n    ") << entries[i]->local << ",";
    if (entries.empty()) o << "0";
    unsigned bits = 4;
    while ((size_t(1) << bits) < entries.size() * 2) ++bits;
    const size_t slots = size_t(1) << bits;
    std::vector<uint32_t> slot(slots, 0xffffffffu);
    for (size_t i = 0; i < entries.size(); ++i) {
        size_t h = uint32_t(entries[i]->addr * 0x9E3779B1u) >> (32 - bits);
        while (slot[h] != 0xffffffffu) h = (h + 1) & (slots - 1);
        slot[h] = uint32_t(i);
    }
    o << "\n};\nstatic const uint32_t entry_slot[" << slots << "] = {";
    for (size_t i = 0; i < slots; ++i) o << (i % 8 ? " " : "\n    ") << hx(slot[i]) << ",";
    o << "\n};\n\n"
         "static int lookup(uint32_t pc) {\n"
         "    const int hint = md_gen_next;\n"
         "    if (hint >= 0 && entry_pc[hint] == pc) return hint;\n"
         "    unsigned h = (pc * 0x9E3779B1u) >> " << (32 - bits) << ";\n"
         "    for (;;) {\n"
         "        const uint32_t i = entry_slot[h];\n"
         "        if (i == 0xffffffffu) return -1;\n"
         "        if (entry_pc[i] == pc) return (int)i;\n"
         "        h = (h + 1u) & " << (slots - 1) << "u;\n"
         "    }\n}\n\n"
         "/* One region from m->pc, or one interpreted instruction when there is none. */\n"
         "void md_full_step_native(MdFull *m) {\n"
         "    const int e = lookup(m->pc);\n"
         "    md_gen_next = -1;\n"
         "    if (e < 0) { md_full_step_interp(m); return; }\n"
         "    ++m->native_hits;\n"
         "    region_fn[entry_region[e]](m, entry_local[e]);\n"
         "}\n";
    return o.str();
}

} // namespace

std::string emit_m68k(const std::vector<uint8_t> &rom, const std::set<uint32_t> &seeds, EmitStats &stats) {
    Emitter e(rom);
    return e.run(seeds, stats);
}

} // namespace m0boy::megadrive
