/* Z80 -> C for the SG-1000 runtime.
 *
 * Same structure as the Game Boy META backend: instructions (played ones
 * first, then those reachable from them) are grouped by address into
 * regions, one C function each, with gotos inside and a switch on pc at
 * entry.  CALL becomes a C call followed by a check that the guest returned
 * where expected (otherwise the C stack unwinds to a dispatch loop); every
 * other exit sets pc and returns or tail-calls the target region.  Flags are
 * computed only when a later instruction can read them; cycles and refresh
 * are added in batches.  Instructions without a hand-written lowering use
 * the interpreter's semantics (z80_ops.h) on constant operands. */
#include "m0boy/sg1000_emitter.h"
#include "z80.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <iomanip>
#include <map>
#include <sstream>

namespace m0boy::sg1000 {
namespace {

constexpr uint8_t FC = 1, FN = 2, FPV = 4, FX = 8, FH = 16, FY = 32, FZ = 64, FS = 128, FALL = 0xff;

enum class Flow { Seq, Jump, JumpCond, Djnz, Call, CallCond, Ret, RetCond, Reti, JumpDyn, Halt, Repeat, Ei, Bad };

struct Ins {
    uint16_t addr = 0;
    z80_decoded_t d{};
    Flow flow = Flow::Seq;
    uint16_t target = 0;
    unsigned cycles = 0, taken = 0; ///< not taken (or only) / taken
    uint8_t use = 0, def = 0;
    bool timed = false;              ///< observes machine.cyc or R: flush before
    bool covered = false;
    int region = -1;
    size_t order = 0;
    bool label = false, sw_case = false, loop_head = false;
    uint8_t live_in = 0, live_out = 0;
    uint16_t next() const { return static_cast<uint16_t>(addr + d.length); }
    bool falls() const {
        return flow == Flow::Seq || flow == Flow::JumpCond || flow == Flow::Djnz || flow == Flow::Call ||
               flow == Flow::CallCond || flow == Flow::RetCond || flow == Flow::Halt || flow == Flow::Repeat ||
               flow == Flow::Ei;
    }
    bool calls() const { return flow == Flow::Call || flow == Flow::CallCond; }
    bool jumps() const { return flow == Flow::Jump || flow == Flow::JumpCond || flow == Flow::Djnz; }
};

std::string hex(unsigned v, int w = 4) {
    std::ostringstream o;
    o << "0x" << std::hex << std::setw(w) << std::setfill('0') << v;
    return o.str();
}
std::string lab(uint16_t a) {
    std::ostringstream o;
    o << "L_" << std::hex << std::setw(4) << std::setfill('0') << a;
    return o.str();
}

/* ---- sandbox: the interpreter measures cycles and flow ------------------ */
struct Sandbox {
    uint8_t mem[65536];
};
uint8_t sb_read(void* u, uint16_t a) { return static_cast<Sandbox*>(u)->mem[a]; }
void sb_write(void*, uint16_t, uint8_t) {}
uint8_t sb_in(void*, uint16_t) { return 0xff; }
void sb_out(void*, uint16_t, uint8_t) {}

struct Run { unsigned cycles; uint16_t pc; };
Run measure(Sandbox& sb, const z80_decoded_t& d, uint16_t addr, uint8_t f, uint16_t bc) {
    z80_t c{};
    c.user = &sb; c.read = sb_read; c.write = sb_write; c.in = sb_in; c.out = sb_out;
    c.pc = addr; c.f = f; c.bc = bc; c.a = 0x5a; c.hl = 0xc100; c.sp = 0xd000;
    unsigned cycles = z80_execute_decoded(&c, &d);
    return { cycles, c.pc };
}

uint8_t cond_flag(unsigned cc) {
    static const uint8_t f[4] = { FZ, FC, FPV, FS };
    return f[cc >> 1];
}
std::string cond_expr(unsigned cc) {
    static const char* names[4] = { "F_Z", "F_C", "F_PV", "F_S" };
    std::string t = std::string("(Z.f & ") + names[cc >> 1] + ")";
    return (cc & 1) ? t : "!" + t;
}

/* Flags read and written; unknown cases read everything and write nothing. */
void flag_info(const z80_decoded_t& d, uint8_t& use, uint8_t& def, bool& timed) {
    const unsigned op = d.opcode, x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    use = def = 0;
    timed = false;
    const uint8_t all_but_c = FALL & ~FC;
    if (d.group == 1) {
        if (x == 0) { def = FALL; if (y == 2 || y == 3) use = FC; }
        else if (x == 1) def = all_but_c;
        return;
    }
    if (d.group == 2) {
        if (x == 1) switch (z) {
        case 0: def = all_but_c; timed = true; return;          /* IN r,(C) */
        case 1: timed = true; return;                            /* OUT (C),r */
        case 2: use = FC; def = FALL; return;                    /* SBC/ADC HL,rr */
        case 4: def = FALL; return;                              /* NEG */
        case 7:
            if (y == 1 || y == 3) timed = true;                  /* LD R,A / LD A,R */
            if (y == 2 || y == 3 || y == 4 || y == 5) def = all_but_c;
            return;
        default: return;
        }
        if (x == 2 && y >= 4 && z <= 3) {
            if (z == 0) def = FH | FPV | FN | FX | FY;
            else if (z == 1) def = all_but_c;
            else { def = FALL; timed = true; }
        }
        return;
    }
    if (x == 1) return;
    if (x == 2) { def = FALL; if (y == 1 || y == 3) use = FC; return; }
    if (x == 0) switch (z) {
    case 0:
        if (y == 1) { use = FALL; def = FALL; }                  /* EX AF,AF' */
        else if (y >= 4) use = cond_flag(y - 4);
        return;
    case 1: if (q) def = FH | FN | FC | FX | FY; return;        /* ADD HL,rr */
    case 4: case 5: def = all_but_c; return;
    case 7:
        switch (y) {
        case 0: case 1: def = FC | FN | FH | FX | FY; return;
        case 2: case 3: use = FC; def = FC | FN | FH | FX | FY; return;
        case 4: use = FC | FH | FN; def = FALL & ~FN; return;   /* DAA */
        case 5: def = FH | FN | FX | FY; return;                 /* CPL */
        case 6: def = FC | FH | FN | FX | FY; return;            /* SCF */
        default: use = FC; def = FC | FH | FN | FX | FY; return; /* CCF */
        }
    default: return;
    }
    switch (z) { /* x == 3 */
    case 0: case 2: case 4: use = cond_flag(y); return;
    case 1: if (!q && p == 3) def = FALL; return;                /* POP AF */
    case 3: if (y == 2 || y == 3) timed = true; return;          /* OUT/IN (n) */
    case 5: if (!q && p == 3) use = FALL; return;                /* PUSH AF */
    case 6: def = FALL; if (y == 1 || y == 3) use = FC; return;
    default: return;
    }
}

class Emitter {
public:
    Emitter(const Cartridge& cart, const EmitOptions& opt) : cart_(cart), opt_(opt) {
        /* a sync per instruction makes large functions very costly to compile */
        if (opt_.precise) opt_.max_region_instructions = std::min<size_t>(opt_.max_region_instructions, 32);
        std::memset(sb_.mem, 0, sizeof sb_.mem);
        for (uint32_t a = 0; a < 0xc000; a++) sb_.mem[a] = rom(static_cast<uint16_t>(a));
    }
    std::string run(const std::vector<uint16_t>& hot, EmitStats& stats);

private:
    const Cartridge& cart_;
    EmitOptions opt_;
    Sandbox sb_;
    std::map<uint16_t, Ins> ins_;
    std::vector<std::vector<uint16_t>> regions_;
    uint32_t pend_cy_ = 0, pend_rf_ = 0;
    EmitStats* stats_ = nullptr;

    uint32_t rom_size() const { return static_cast<uint32_t>(cart_.bytes().size()); }
    uint8_t rom(uint16_t a) const { /* bus semantics of sg1000_read for 0000-BFFF */
        const auto b = cart_.bytes();
        const size_t n = b.size();
        if (n && !(n & (n - 1))) return b[a & (n - 1)];
        return a < n ? b[a] : 0xff;
    }
    bool rom_const(uint16_t a) const { return a < (opt_.cart_ram ? 0x8000u : 0xc000u); }
    bool decode(uint16_t addr, Ins& in);
    Ins* find(uint16_t a) { auto it = ins_.find(a); return it == ins_.end() ? nullptr : &it->second; }
    void plan();
    void flush(std::ostringstream& o) {
        if (pend_cy_ || pend_rf_) {
            if (pend_rf_) o << "    ADV(" << pend_cy_ << ", " << pend_rf_ << ");\n";
            else o << "    CY(" << pend_cy_ << ");\n";
        }
        pend_cy_ = pend_rf_ = 0;
    }
    std::string region_name(int r) const { return "r_" + std::to_string(r); }
    void transfer(std::ostringstream& o, const Ins& from, uint16_t target);
    void emit_region(std::ostringstream& o, int r);
    void emit_ins(std::ostringstream& o, Ins& in, const Ins* following);
    bool emit_native(std::ostringstream& o, const Ins& in, bool flags);
    void emit_generic(std::ostringstream& o, const Ins& in);
    std::string reg8(unsigned r, const z80_decoded_t& d) const;
    std::string addr_hl(const z80_decoded_t& d) const;
    std::string pair(unsigned p, const z80_decoded_t& d) const;
    std::string mem_read(uint16_t a) const;
    std::string mem_write(uint16_t a, const std::string& v) const;
    static std::string decoded(const z80_decoded_t& d);
};

bool Emitter::decode(uint16_t addr, Ins& in) {
    z80_t c{};
    c.user = &sb_; c.read = sb_read;
    c.pc = addr;
    in.addr = addr;
    if (!z80_decode(&c, &in.d) || addr + in.d.length > rom_size()) return false;
    const z80_decoded_t& d = in.d;
    const unsigned op = d.opcode, x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    flag_info(d, in.use, in.def, in.timed);
    const uint16_t next = in.next();
    if (d.group == 0) {
        if (x == 0 && z == 0 && y >= 2) {
            in.flow = y == 2 ? Flow::Djnz : y == 3 ? Flow::Jump : Flow::JumpCond;
            in.target = static_cast<uint16_t>(next + static_cast<int8_t>(d.immediate));
        } else if (op == 0x76) in.flow = Flow::Halt;
        else if (x == 3) switch (z) {
        case 0: in.flow = Flow::RetCond; break;
        case 1: if (q && p == 0) in.flow = Flow::Ret; else if (q && p == 2) in.flow = Flow::JumpDyn; break;
        case 2: in.flow = Flow::JumpCond; in.target = d.immediate; break;
        case 3: if (y == 0) { in.flow = Flow::Jump; in.target = d.immediate; } else if (y == 7) in.flow = Flow::Ei; break;
        case 4: in.flow = Flow::CallCond; in.target = d.immediate; break;
        case 5: if (q && p == 0) { in.flow = Flow::Call; in.target = d.immediate; } break;
        case 7: in.flow = Flow::Call; in.target = static_cast<uint16_t>(y * 8); break;
        default: break;
        }
    } else if (d.group == 2) {
        if (x == 1 && z == 5) in.flow = Flow::Reti;
        else if (x == 2 && y >= 6 && z <= 3) in.flow = Flow::Repeat;
    }
    /* Cycles from the interpreter: both outcomes of a condition. */
    Run a = measure(sb_, d, addr, 0x00, 0x0101), b = measure(sb_, d, addr, 0xff, 0x0201);
    switch (in.flow) {
    case Flow::JumpCond: case Flow::CallCond: case Flow::RetCond: case Flow::Djnz: {
        const bool a_taken = a.pc != next, b_taken = b.pc != next;
        if (a_taken == b_taken) return false; /* not expected: leave to the interpreter */
        in.taken = a_taken ? a.cycles : b.cycles;
        in.cycles = a_taken ? b.cycles : a.cycles;
        break;
    }
    case Flow::Repeat: in.cycles = 16; in.taken = 21; break;
    default: in.cycles = in.taken = a.cycles; break;
    }
    return true;
}

void Emitter::plan() {
    /* Regions: runs of nearby instructions. */
    const Ins* prev = nullptr;
    for (auto& [a, in] : ins_) {
        bool split = !prev || in.addr > prev->next() + 32;
        if (!split && regions_.back().size() >= opt_.max_region_instructions &&
            !(prev->falls() && prev->next() == in.addr))
            split = true;
        if (split) regions_.emplace_back();
        in.region = static_cast<int>(regions_.size() - 1);
        in.order = regions_.back().size();
        regions_.back().push_back(a);
        prev = &in;
    }
    for (auto& reg : regions_) { Ins& f = ins_[reg.front()]; f.label = f.sw_case = true; }
    for (uint16_t v : { 0x0000, 0x0038, 0x0066 })
        if (Ins* t = find(v)) t->label = t->sw_case = true;
    for (auto& [a, in] : ins_) {
        if (in.falls()) {
            if (Ins* n = find(in.next())) {
                const bool adjacent = n->region == in.region && n->order == in.order + 1;
                if (!adjacent) { n->label = true; if (n->region != in.region) n->sw_case = true; }
                /* resumption points: after a call returns, after HALT and
                 * after the instruction that follows EI */
                if (in.calls() || in.flow == Flow::Halt) n->label = n->sw_case = true;
                if (in.flow == Flow::Ei) if (Ins* m = find(n->next())) m->label = m->sw_case = true;
            }
        }
        if (in.flow == Flow::Repeat) in.label = in.sw_case = in.loop_head = true;
        if (in.jumps() || in.calls()) {
            if (Ins* t = find(in.target)) {
                t->label = true;
                if (in.calls() || t->region != in.region) t->sw_case = true;
                if (!in.calls() && t->region == in.region && t->order <= in.order) t->loop_head = t->sw_case = true;
            }
        }
    }
    /* Flag liveness per region, least fixed point; exits keep all live. */
    for (auto& [a, in] : ins_) in.live_in = in.live_out = 0;
    for (auto& reg : regions_) {
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t k = reg.size(); k-- > 0;) {
                Ins& in = ins_[reg[k]];
                uint8_t out = 0;
                if (opt_.precise || in.calls() || in.flow == Flow::Ei || in.flow == Flow::Bad) out = FALL;
                if (in.falls()) {
                    Ins* n = find(in.next());
                    out |= (n && n->region == in.region) ? n->live_in : FALL;
                }
                if (in.jumps()) {
                    Ins* t = find(in.target);
                    out |= (t && t->region == in.region) ? t->live_in : FALL;
                }
                if (in.flow == Flow::Ret || in.flow == Flow::RetCond || in.flow == Flow::Reti || in.flow == Flow::JumpDyn)
                    out = FALL;
                const uint8_t live_in = static_cast<uint8_t>(in.use | (out & ~in.def));
                if (out != in.live_out || live_in != in.live_in) {
                    in.live_out = out;
                    in.live_in = live_in;
                    changed = true;
                }
            }
        }
    }
}

std::string Emitter::reg8(unsigned r, const z80_decoded_t& d) const {
    static const char* n[8] = { "Z.b", "Z.c", "Z.d", "Z.e", "Z.h", "Z.l", nullptr, "Z.a" };
    if (r == 6) return "sg_rd(" + addr_hl(d) + ")";
    return n[r];
}
std::string Emitter::addr_hl(const z80_decoded_t& d) const {
    if (!d.index) return "Z.hl";
    std::ostringstream o;
    o << "(uint16_t)(" << (d.index == 1 ? "Z.ix" : "Z.iy") << " + " << int(static_cast<int8_t>(d.displacement)) << ")";
    return o.str();
}
std::string Emitter::pair(unsigned p, const z80_decoded_t& d) const {
    if (p == 0) return "Z.bc";
    if (p == 1) return "Z.de";
    if (p == 2) return d.index == 1 ? "Z.ix" : d.index == 2 ? "Z.iy" : "Z.hl";
    return "Z.sp";
}

/* Constant addresses: ROM folded, RAM indexed directly. */
std::string Emitter::mem_read(uint16_t a) const {
    if (rom_const(a)) return hex(rom(a), 2);
    if (a >= 0xc000) return "machine.ram[" + hex(a & 1023u, 3) + "]";
    return "machine.cart_ram[" + hex(a & 8191u) + "]";
}
std::string Emitter::mem_write(uint16_t a, const std::string& v) const {
    if (rom_const(a)) return "/* ROM */";
    if (a >= 0xc000) return "machine.ram[" + hex(a & 1023u, 3) + "] = " + v + ";";
    return "machine.cart_ram[" + hex(a & 8191u) + "] = " + v + ";";
}
std::string Emitter::decoded(const z80_decoded_t& d) {
    std::ostringstream o;
    o << "{" << d.immediate << ", " << d.length << ", " << unsigned(d.opcode) << ", " << unsigned(d.index) << ", "
      << unsigned(d.group) << ", " << unsigned(d.displacement) << ", " << unsigned(d.refresh) << ", " << d.extra_cycles << "}";
    return o.str();
}

void Emitter::transfer(std::ostringstream& o, const Ins& from, uint16_t target) {
    const Ins* t = find(target);
    if (t && t->region == from.region) { o << "    goto " << lab(target) << ";\n"; return; }
    o << "    Z.pc = " << hex(target) << ";";
    if (t) o << " " << region_name(t->region) << "();";
    o << " return;\n";
}

void Emitter::emit_generic(std::ostringstream& o, const Ins& in) {
    const z80_decoded_t& d = in.d;
    o << "    { static const z80_decoded_t d_ = " << decoded(d) << "; (void)sg_op(&d_); }\n";
    stats_->generic++;
}

/* Hand-written lowering of sequential instructions; false: use generic. */
bool Emitter::emit_native(std::ostringstream& o, const Ins& in, bool flags) {
    const z80_decoded_t& d = in.d;
    const unsigned op = d.opcode, x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    const bool ixhl = d.index != 0; /* H/L name IXH/IXL unless memory is involved */
    auto ok = [&](unsigned r, bool plain) { return !(ixhl && !plain && (r == 4 || r == 5)); };
    std::string s;
    if (d.group == 1) {
        if (d.index && z != 6) return false; /* undocumented register copy */
        const unsigned r = d.index ? 6 : z;
        const std::string v = reg8(r, d);
        if (x == 1) { /* BIT: X/Y from the address high byte when indexed */
            if (!flags) { o << "    /* BIT: flags unused */\n"; return true; }
            const std::string xy = d.index ? "(" + addr_hl(d) + " >> 8)" : "v_";
            o << "    { uint8_t v_ = " << v << "; Z.f = (uint8_t)((Z.f & 1) | 0x10 | (" << xy << " & 0x28) | ((v_ & "
              << hex(1u << y, 2) << ") ? " << (y == 7 ? "0x80" : "0") << " : 0x44)); }\n";
            return true;
        }
        if (x == 0) {
            std::string expr;
            if (flags) expr = "sg_rot" + std::to_string(y) + "(V_)";
            else {
                static const char* plain[8] = {
                    "(uint8_t)((V_ << 1) | (V_ >> 7))", "(uint8_t)((V_ >> 1) | (V_ << 7))",
                    "(uint8_t)((V_ << 1) | (Z.f & 1))", "(uint8_t)((V_ >> 1) | ((Z.f & 1) << 7))",
                    "(uint8_t)(V_ << 1)", "(uint8_t)((V_ >> 1) | (V_ & 0x80))",
                    "(uint8_t)((V_ << 1) | 1)", "(uint8_t)(V_ >> 1)" };
                expr = plain[y];
            }
            auto sub = [&](const std::string& val) { std::string e = expr; size_t pos;
                while ((pos = e.find("V_")) != std::string::npos) e.replace(pos, 2, val); return e; };
            if (r == 6) o << "    { uint16_t a_ = " << addr_hl(d) << "; uint8_t m_ = sg_rd(a_); sg_wr(a_, " << sub("m_") << "); }\n";
            else o << "    { uint8_t m_ = " << v << "; " << v << " = " << sub("m_") << "; }\n";
            return true;
        }
        const std::string mask = hex(1u << y, 2);
        if (r == 6) {
            o << "    { uint16_t a_ = " << addr_hl(d) << "; sg_wr(a_, (uint8_t)(sg_rd(a_) " << (x == 2 ? "& ~" : "| ") << mask << ")); }\n";
        } else {
            o << "    " << reg8(r, d) << (x == 2 ? " &= (uint8_t)~" : " |= ") << mask << ";\n";
        }
        return true;
    }
    if (d.group == 2) {
        if (x == 1 && z == 4) { o << (flags ? "    sg_neg();\n" : "    Z.a = (uint8_t)-Z.a;\n"); return true; }
        if (x == 1 && z == 2) { o << "    sg_hl16(" << q << ", " << pair(p, d) << ");\n"; return true; }
        if (x == 2 && (y == 4 || y == 5) && z == 0 && !flags) { /* LDI / LDD */
            const char* step = y == 4 ? "++" : "--";
            o << "    { uint8_t v_ = sg_rd(Z.hl); sg_wr(Z.de, v_); Z.hl" << step << "; Z.de" << step << "; Z.bc--; }\n";
            return true;
        }
        return false;
    }
    if (x == 1) {
        const bool plain = y == 6 || z == 6;
        if (!ok(y, plain) || !ok(z, plain)) return false;
        if (y == z) return true;
        if (y == 6) o << "    sg_wr(" << addr_hl(d) << ", " << reg8(z, d) << ");\n";
        else o << "    " << reg8(y, d) << " = " << reg8(z, d) << ";\n";
        return true;
    }
    if (x == 2 || (x == 3 && z == 6)) {
        std::string v;
        if (x == 2) { if (!ok(z, z == 6)) return false; v = reg8(z, d); }
        else v = hex(d.immediate & 255, 2);
        static const char* alu[8] = { "sg_add", "sg_adc", "sg_sub", "sg_sbc", "sg_and", "sg_xor", "sg_or", "sg_cp" };
        if (flags) { o << "    " << alu[y] << "(" << v << ");\n"; return true; }
        switch (y) {
        case 0: o << "    Z.a = (uint8_t)(Z.a + " << v << ");\n"; break;
        case 1: o << "    Z.a = (uint8_t)(Z.a + " << v << " + (Z.f & 1));\n"; break;
        case 2: o << "    Z.a = (uint8_t)(Z.a - " << v << ");\n"; break;
        case 3: o << "    Z.a = (uint8_t)(Z.a - " << v << " - (Z.f & 1));\n"; break;
        case 4: o << "    Z.a &= " << v << ";\n"; break;
        case 5: o << "    Z.a ^= " << v << ";\n"; break;
        case 6: o << "    Z.a |= " << v << ";\n"; break;
        default: o << "    /* CP: flags unused */\n"; break;
        }
        return true;
    }
    if (x == 0) switch (z) {
    case 0:
        if (y == 0) return true;
        if (y == 1) {
            o << "    { uint16_t t_ = (uint16_t)((Z.a << 8) | Z.f); Z.a = (uint8_t)(Z.af2 >> 8); Z.f = (uint8_t)Z.af2; Z.af2 = t_; }\n";
            return true;
        }
        return false;
    case 1:
        if (!q) { o << "    " << pair(p, d) << " = " << hex(d.immediate) << ";\n"; return true; }
        {
            const std::string hl = pair(2, d), rr = pair(p, d);
            const uint8_t need = in.def & in.live_out;
            if (!need) o << "    " << hl << " = (uint16_t)(" << hl << " + " << rr << ");\n";
            else if (!(need & ~FC)) /* only the carry is read */
                o << "    { uint32_t v_ = (uint32_t)" << hl << " + " << rr << "; Z.f = (uint8_t)((Z.f & ~1) | (v_ >> 16)); " << hl << " = (uint16_t)v_; }\n";
            else
                o << "    { uint32_t a_ = " << hl << ", b_ = " << rr << ", v_ = a_ + b_; Z.f = (uint8_t)((Z.f & 0xc4) | ((v_ >> 8) & 0x28)"
                     " | (((a_ ^ b_ ^ v_) >> 8) & 0x10) | (v_ >> 16)); " << hl << " = (uint16_t)v_; }\n";
        }
        return true;
    case 2: {
        const uint16_t nn = d.immediate;
        if (p < 2) {
            const std::string a = p ? "Z.de" : "Z.bc";
            if (q) o << "    Z.a = sg_rd(" << a << ");\n"; else o << "    sg_wr(" << a << ", Z.a);\n";
        } else if (p == 2) {
            const uint16_t n1 = static_cast<uint16_t>(nn + 1);
            if (!q) o << "    " << mem_write(nn, "(uint8_t)" + pair(2, d)) << " " << mem_write(n1, "(uint8_t)(" + pair(2, d) + " >> 8)") << "\n";
            else o << "    " << pair(2, d) << " = (uint16_t)(" << mem_read(nn) << " | (" << mem_read(n1) << " << 8));\n";
        } else {
            if (!q) o << "    " << mem_write(nn, "Z.a") << "\n";
            else o << "    Z.a = " << mem_read(nn) << ";\n";
        }
        return true;
    }
    case 3: o << "    " << pair(p, d) << (q ? "--" : "++") << ";\n"; return true;
    case 4: case 5: {
        if (!ok(y, false)) return false;
        const bool dec = z == 5;
        if (y == 6) {
            o << "    { uint16_t a_ = " << addr_hl(d) << "; ";
            if (flags) o << "sg_wr(a_, " << (dec ? "sg_dec" : "sg_inc") << "(sg_rd(a_))); }\n";
            else o << "sg_wr(a_, (uint8_t)(sg_rd(a_) " << (dec ? "- 1" : "+ 1") << ")); }\n";
        } else {
            const std::string r = reg8(y, d);
            if (flags) o << "    " << r << " = " << (dec ? "sg_dec(" : "sg_inc(") << r << ");\n";
            else o << "    " << r << (dec ? "--" : "++") << ";\n";
        }
        return true;
    }
    case 7:
        switch (y) {
        case 0: o << "    { uint8_t r_ = (uint8_t)((Z.a << 1) | (Z.a >> 7)); Z.a = r_;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc4) | (r_ & 0x29));" : "") << " }\n"; break;
        case 1: o << "    { uint8_t c_ = Z.a & 1, r_ = (uint8_t)((Z.a >> 1) | (c_ << 7)); Z.a = r_;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc4) | (r_ & 0x28) | c_);" : "") << " }\n"; break;
        case 2: o << "    { uint8_t c_ = Z.a >> 7, r_ = (uint8_t)((Z.a << 1) | (Z.f & 1)); Z.a = r_;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc4) | (r_ & 0x28) | c_);" : "") << " }\n"; break;
        case 3: o << "    { uint8_t c_ = Z.a & 1, r_ = (uint8_t)((Z.a >> 1) | ((Z.f & 1) << 7)); Z.a = r_;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc4) | (r_ & 0x28) | c_);" : "") << " }\n"; break;
        case 4: o << "    sg_daa();\n"; break;
        case 5: o << "    Z.a = (uint8_t)~Z.a;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc5) | 0x12 | (Z.a & 0x28));" : "") << "\n"; break;
        case 6: if (flags) o << "    Z.f = (uint8_t)((Z.f & 0xc4) | 0x01 | (Z.a & 0x28));\n"; break;
        default: if (flags) o << "    Z.f = (uint8_t)((Z.f & 0xc4) | ((Z.f & 1) ? 0x10 : 0x01) | (Z.a & 0x28));\n"; break;
        }
        return true;
    case 6:
        if (!ok(y, false)) return false;
        if (y == 6) o << "    sg_wr(" << addr_hl(d) << ", " << hex(d.immediate & 255, 2) << ");\n";
        else o << "    " << reg8(y, d) << " = " << hex(d.immediate & 255, 2) << ";\n";
        return true;
    default: return false;
    }
    /* x == 3, sequential forms */
    switch (z) {
    case 1:
        if (!q) {
            if (p == 3) o << "    { uint16_t v_ = sg_pop(); Z.a = (uint8_t)(v_ >> 8); Z.f = (uint8_t)v_; }\n";
            else o << "    " << pair(p, d) << " = sg_pop();\n";
            return true;
        }
        if (p == 1) {
            o << "    { uint16_t t_ = Z.bc; Z.bc = Z.bc2; Z.bc2 = t_; t_ = Z.de; Z.de = Z.de2; Z.de2 = t_;"
                 " t_ = Z.hl; Z.hl = Z.hl2; Z.hl2 = t_; }\n";
            return true;
        }
        if (p == 3) { o << "    Z.sp = " << pair(2, d) << ";\n"; return true; }
        return false;
    case 3:
        if (y == 2) { o << "    sg1000_out(" << hex(d.immediate & 255, 2) << ", Z.a);\n"; return true; }
        if (y == 3) { o << "    Z.a = sg1000_in(" << hex(d.immediate & 255, 2) << ");\n"; return true; }
        if (y == 5) { o << "    { uint16_t t_ = Z.de; Z.de = Z.hl; Z.hl = t_; }\n"; return true; }
        if (y == 4) {
            const std::string hl = pair(2, d);
            o << "    { uint16_t t_ = sg_rd16(Z.sp); sg_wr16(Z.sp, " << hl << "); " << hl << " = t_; }\n";
            return true;
        }
        if (y == 6) { o << "    Z.iff1 = Z.iff2 = 0; Z.ei_delay = 0;\n"; return true; }
        return false;
    case 5:
        if (q) return false;
        if (p == 3) o << "    sg_push((uint16_t)((Z.a << 8) | Z.f));\n";
        else o << "    sg_push(" << pair(p, d) << ");\n";
        return true;
    default: return false;
    }
}

void Emitter::emit_ins(std::ostringstream& o, Ins& in, const Ins* following) {
    const z80_decoded_t& d = in.d;
    const uint16_t next = in.next();
    if (in.label) {
        flush(o);
        o << lab(in.addr) << ":\n";
        if (in.loop_head && !opt_.precise) o << "    SYNC(" << hex(in.addr) << ");\n";
    }
    if (opt_.precise) { flush(o); o << "    SYNC(" << hex(in.addr) << ");\n"; }
    const bool flags = (in.def & in.live_out) != 0;
    if (in.timed) flush(o);
    auto native_or_generic = [&]() {
        if (emit_native(o, in, flags)) { pend_rf_ += d.refresh; stats_->native++; }
        else emit_generic(o, in); /* adds its own refresh */
        pend_cy_ += in.cycles;
    };
    switch (in.flow) {
    case Flow::Seq:
        native_or_generic();
        break;
    case Flow::Bad:
        flush(o);
        o << "    Z.pc = " << hex(in.addr) << "; return;\n";
        return;
    case Flow::Jump:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        transfer(o, in, in.target);
        return;
    case Flow::JumpCond: case Flow::Djnz: {
        pend_rf_ += d.refresh;
        flush(o);
        if (in.flow == Flow::Djnz) o << "    if (--Z.b) {\n";
        else o << "    if (" << cond_expr(in.d.group == 0 && (d.opcode >> 6) == 0 ? ((d.opcode >> 3) & 7) - 4 : (d.opcode >> 3) & 7) << ") {\n";
        o << "    CY(" << in.taken << ");\n";
        transfer(o, in, in.target);
        o << "    }\n";
        pend_cy_ += in.cycles;
        break;
    }
    case Flow::Call: case Flow::CallCond: {
        pend_rf_ += d.refresh;
        flush(o);
        const Ins* t = find(in.target);
        if (in.flow == Flow::CallCond) o << "    if (" << cond_expr((d.opcode >> 3) & 7) << ") {\n";
        o << "    CY(" << in.taken << "); sg_push(" << hex(next) << "); Z.pc = " << hex(in.target) << ";\n";
        o << "    " << (t ? region_name(t->region) + "()" : std::string("sg_exec()")) << ";\n";
        o << "    if (Z.pc != " << hex(next) << " || SG_STOPPED()) return;\n";
        if (in.flow == Flow::CallCond) o << "    } else CY(" << in.cycles << ");\n";
        break;
    }
    case Flow::Ret:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.pc = sg_pop(); return;\n";
        return;
    case Flow::Reti:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.pc = sg_pop(); Z.iff1 = Z.iff2; return;\n";
        return;
    case Flow::RetCond:
        pend_rf_ += d.refresh;
        flush(o);
        o << "    if (" << cond_expr((d.opcode >> 3) & 7) << ") { CY(" << in.taken << "); Z.pc = sg_pop(); return; }\n";
        pend_cy_ += in.cycles;
        break;
    case Flow::JumpDyn:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.pc = " << pair(2, d) << "; return;\n";
        return;
    case Flow::Halt:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.halted = 1; Z.pc = " << hex(next) << "; sg_halt(); if (sg_event(" << hex(next) << ")) return;\n";
        break;
    case Flow::Repeat: {
        /* label + SYNC above.  Fast builds run LDIR/LDDR/OTIR/OTDR as C loops
         * (their flags unread), still interruptible between iterations. */
        flush(o);
        const unsigned op = d.opcode, y = (op >> 3) & 7, z = op & 7;
        if (!opt_.precise && !(in.def & in.live_out) && (z == 0 || z == 3)) {
            const char* step = (y & 1) ? "--" : "++";
            o << "    for (;;) {\n";
            if (z == 0) {
                o << "        uint8_t v_ = sg_rd(Z.hl); sg_wr(Z.de, v_); Z.hl" << step << "; Z.de" << step << "; Z.bc--;\n"
                  << "        if (!Z.bc) { ADV(16, " << unsigned(d.refresh) << "); break; }\n";
            } else {
                o << "        uint8_t v_ = sg_rd(Z.hl); Z.b--; Z.hl" << step << "; sg1000_out(Z.c, v_);\n"
                  << "        if (!Z.b) { ADV(16, " << unsigned(d.refresh) << "); break; }\n";
            }
            o << "        ADV(21, " << unsigned(d.refresh) << ");\n"
              << "        if (EVT() && sg_event(" << hex(in.addr) << ")) return;\n    }\n";
            stats_->native++;
            break;
        }
        o << "    Z.pc = " << hex(in.addr) << ";\n";
        o << "    { static const z80_decoded_t d_ = " << decoded(d) << "; CY(sg_op(&d_)); }\n";
        o << "    if (Z.pc == " << hex(in.addr) << ") goto " << lab(in.addr) << ";\n";
        stats_->generic++;
        break;
    }
    case Flow::Ei: {
        /* The next instruction runs in the interpreter, which ends the EI
         * delay and forces a sync; the generated code resumes after it. */
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.iff1 = Z.iff2 = 1; Z.ei_delay = 1;\n";
        const Ins* n = find(next);
        if (!n) { o << "    Z.pc = " << hex(next) << "; return;\n"; return; }
        const uint16_t after = n->next();
        if (n->flow == Flow::Jump || n->flow == Flow::Ret || n->flow == Flow::Reti) {
            /* EI; JP/JR/RET/RETI: the delay ends with the transfer (the idle
             * loop EI / JP $-1 and the handler end EI / RETI) */
            if (opt_.precise) o << "    SYNC(" << hex(next) << ");\n";
            pend_cy_ += n->cycles; pend_rf_ += n->d.refresh;
            flush(o);
            o << "    EI_DONE();\n";
            if (n->flow == Flow::Jump) {
                if (!opt_.precise) o << "    SYNC(" << hex(n->target) << ");\n";
                transfer(o, in, n->target);
            } else {
                o << "    Z.pc = sg_pop();" << (n->flow == Flow::Reti ? " Z.iff1 = Z.iff2;" : "") << " return;\n";
            }
            stats_->native++;
            return;
        }
        if (n->flow == Flow::Seq) {
            /* EI; N; ...: N inline, then the delay ends and a sync takes a
             * pending interrupt before the instruction after N (the usual
             * EI / NOP / DI window costs no interpreter step). */
            if (opt_.precise) o << "    SYNC(" << hex(next) << ");\n";
            if (n->timed) flush(o);
            if (emit_native(o, *n, (n->def & n->live_out) != 0)) { pend_rf_ += n->d.refresh; stats_->native++; }
            else emit_generic(o, *n);
            pend_cy_ += n->cycles;
            flush(o);
            o << "    EI_DONE();\n";
            if (!opt_.precise) o << "    SYNC(" << hex(after) << ");\n";
            transfer(o, in, after);
            return;
        }
        o << "    Z.pc = " << hex(next) << "; if (!sg_step(" << hex(after) << ")) return;\n";
        o << "    SYNC(" << hex(after) << ");\n";
        transfer(o, in, after);
        return;
    }
    }
    /* Fall through to the next instruction, inline or by transfer. */
    if (!following || following->addr != next) {
        flush(o);
        transfer(o, in, next);
    }
}

void Emitter::emit_region(std::ostringstream& o, int r) {
    const auto& reg = regions_[static_cast<size_t>(r)];
    pend_cy_ = pend_rf_ = 0;
    o << "\nvoid " << region_name(r) << "(void) {\n    ENTER();\n";
    if (!opt_.precise) o << "    SYNC(Z.pc);\n";
    o << "    switch (Z.pc) {\n";
    for (uint16_t a : reg) if (ins_[a].sw_case) o << "    case " << hex(a) << ": goto " << lab(a) << ";\n";
    o << "    default: return;\n    }\n";
    for (size_t k = 0; k < reg.size(); k++) {
        Ins& in = ins_[reg[k]];
        const Ins* following = k + 1 < reg.size() ? &ins_[reg[k + 1]] : nullptr;
        /* an Ei/Ret/Jump... ends the straight line: the next one needs a label */
        emit_ins(o, in, following);
    }
    flush(o);
    o << "}\n";
}

std::string Emitter::run(const std::vector<uint16_t>& hot, EmitStats& stats) {
    stats_ = &stats;
    /* Played instructions first (hottest first), then what they reach. */
    std::deque<uint16_t> work;
    auto add = [&](uint16_t a, bool covered) {
        if (ins_.size() >= opt_.max_instructions || a >= rom_size() || ins_.count(a)) return;
        Ins in;
        if (!decode(a, in)) return;
        in.covered = covered;
        ins_[a] = in;
        work.push_back(a);
    };
    for (uint16_t a : hot) add(a, true);
    stats.covered = ins_.size();
    /* Static reach stops at padding: four equal bytes 00 (NOP) or FF (RST 38). */
    auto padding = [&](uint16_t a) {
        const uint8_t v = rom(a);
        if (v != 0x00 && v != 0xff) return false;
        for (unsigned k = 1; k < 4; k++) if (rom(static_cast<uint16_t>(a + k)) != v) return false;
        return true;
    };
    auto reach = [&](uint16_t a) { if (!ins_.count(a) && !padding(a)) add(a, false); };
    for (uint16_t v : { 0x0000, 0x0038, 0x0066 }) reach(v);
    while (!work.empty()) {
        Ins in = ins_[work.front()];
        work.pop_front();
        if (in.falls()) reach(in.next());
        if (in.jumps() || in.calls()) reach(in.target);
    }
    stats.compiled = ins_.size();
    stats.speculative = stats.compiled - stats.covered;
    plan();
    std::ostringstream o;
    o << "/* Generated by m0recomp --system sg1000. Do not edit. */\n#include \"sg_gen.h\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-label\"\n";
    /* noinline: regions call each other; inlining chains of them explodes
     * compile time and flash. */
    for (size_t r = 0; r < regions_.size(); r++)
        o << "void " << region_name(static_cast<int>(r)) << "(void) __attribute__((noinline));\n";
    for (size_t r = 0; r < regions_.size(); r++) emit_region(o, static_cast<int>(r));
    /* Entry table: sorted pc -> region, direct-mapped cache in front. */
    std::vector<const Ins*> entries;
    for (auto& [a, in] : ins_) if (in.sw_case) entries.push_back(&in);
    stats.regions = regions_.size();
    stats.entries = entries.size();
    o << "\nstatic const uint16_t entry_pc[" << std::max<size_t>(entries.size(), 1) << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 12 ? " " : "\n    ") << hex(entries[i]->addr) << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const sg_fn entry_fn[" << std::max<size_t>(entries.size(), 1) << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 8 ? " " : "\n    ") << region_name(entries[i]->region) << ",";
    if (entries.empty()) o << "0";
    o << "\n};\n"
      << "sg_fn sg_gen_lookup(uint16_t pc) {\n"
      << "    static uint16_t cache_pc[64];\n    static sg_fn cache_fn[64];\n"
      << "    unsigned h = (pc ^ (pc >> 6)) & 63;\n"
      << "    if (cache_fn[h] && cache_pc[h] == pc) return cache_fn[h];\n"
      << "    unsigned lo = 0, hi = " << entries.size() << ";\n"
      << "    while (lo < hi) { unsigned mid = (lo + hi) / 2; if (entry_pc[mid] < pc) lo = mid + 1; else hi = mid; }\n"
      << "    if (lo == " << entries.size() << " || entry_pc[lo] != pc) return 0;\n"
      << "    cache_pc[h] = pc; cache_fn[h] = entry_fn[lo];\n"
      << "    return entry_fn[lo];\n}\n";
    return o.str();
}

} // namespace

std::string emit_code(const Cartridge& cartridge, const std::vector<uint16_t>& hot_first,
                      const EmitOptions& options, EmitStats& stats) {
    Emitter e(cartridge, options);
    return e.run(hot_first, stats);
}

} // namespace m0boy::sg1000
