#include "m0boy/m0plus_generator.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

namespace m0boy::m0plus {
namespace fs = std::filesystem;
namespace {
uint16_t half(const std::vector<uint8_t>& bytes, size_t at) {
    return uint16_t(bytes[at]) | uint16_t(bytes[at + 1]) << 8;
}
uint32_t word(const std::vector<uint8_t>& bytes, size_t at) {
    return uint32_t(half(bytes, at)) | uint32_t(half(bytes, at + 2)) << 16;
}
std::set<uint32_t> walk(const std::vector<uint8_t>& bytes, uint32_t base,
                        const std::vector<fs::path>& coverage, std::string& error) {
    std::set<uint32_t> found;
    std::vector<uint32_t> todo;
    auto enqueue = [&](uint32_t at) {
        if (!(at & 1u) && at >= base && uint64_t(at) + 2 <= uint64_t(base) + bytes.size())
            todo.push_back(at);
    };
    const size_t vectors = std::min<size_t>(48, bytes.size() / 4);
    for (size_t i = 1; i < vectors; ++i) {
        const uint32_t handler = word(bytes, i * 4);
        if (handler & 1u) enqueue(handler & ~1u);
    }
    for (const auto& path : coverage) {
        std::ifstream file(path);
        if (!file) { error = "Cannot read M0+ coverage: " + path.string(); return {}; }
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream row(line);
            std::string address;
            row >> address;
            try {
                size_t used = 0;
                const auto parsed = std::stoull(address, &used, 16);
                if (used != address.size() || parsed > 0xffffffffu) throw std::invalid_argument("address");
                enqueue(static_cast<uint32_t>(parsed));
            } catch (const std::exception&) {
                error = "Invalid M0+ coverage address in " + path.string(); return {};
            }
        }
    }
    while (!todo.empty()) {
        const uint32_t pc = todo.back(); todo.pop_back();
        if (!found.insert(pc).second) continue;
        const size_t at = pc - base;
        const uint16_t op = half(bytes, at);
        const bool wide = (op & 0xf800u) >= 0xe800u;
        if (wide && at + 3 >= bytes.size()) continue;
        const uint32_t next = pc + (wide ? 4u : 2u);
        if ((op & 0xf800u) == 0xe000u) {
            const int32_t off = (int32_t)((op & 0x7ffu) << 21) >> 20;
            enqueue(pc + 4u + uint32_t(off));
            continue;
        }
        if ((op & 0xf000u) == 0xd000u && ((op >> 8) & 15u) < 14u) {
            enqueue(pc + 4u + uint32_t(int32_t(int8_t(op)) * 2));
        } else if ((op & 0xf500u) == 0xb100u) {
            enqueue(pc + 4u + (((op >> 9) & 1u) << 6) + (((op >> 3) & 31u) << 1));
        } else if ((op & 0xf800u) == 0xf000u && (half(bytes, at + 2) & 0xd000u) == 0xd000u) {
            const uint16_t second = half(bytes, at + 2);
            const uint32_t s = (op >> 10) & 1u;
            const uint32_t i1 = !(((second >> 13) & 1u) ^ s);
            const uint32_t i2 = !(((second >> 11) & 1u) ^ s);
            const uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) |
                                 ((op & 0x3ffu) << 12) | ((second & 0x7ffu) << 1);
            enqueue(pc + 4u + uint32_t(int32_t(imm << 7) >> 7));
        }
        if ((op & 0xff00u) == 0x4700u || /* BX / BLX register */
            ((op & 0xfe00u) == 0xbc00u && (op & 0x100u)) || /* POP PC */
            ((op & 0xfc00u) == 0x4400u && ((op >> 8) & 3u) != 1u &&
             ((op & 7u) | ((op >> 4) & 8u)) == 15u)) /* ADD/MOV PC */
            continue;
        enqueue(next);
    }
    return found;
}
}

/* Straight-line blocks of the translated instructions.  A block starts at a
 * leader (branch target, return site, entry without a sequential
 * predecessor) and runs up to a branch or the next leader.  Each instruction
 * keeps the interpreter's timing: its own fetch through the NVM cache model,
 * the body of its entry, then the time is applied and the machine's events
 * are looked at, exactly where step() looks at them; only the dispatch
 * (page switch, op check, per-step bookkeeping) is paid once per block. */
bool write_blocks(const fs::path& path,
                  const std::map<uint32_t, std::pair<uint16_t, std::string>>& bodies) {
    auto next_of = [](uint32_t pc, uint16_t op) { return pc + ((op & 0xf800u) >= 0xe800u ? 4u : 2u); };
    auto terminator = [](const std::string& body) { return body.find("BRANCH(") != std::string::npos; };
    std::set<uint32_t> leaders, sequential;
    for (const auto& [pc, entry] : bodies) {
        const uint32_t next = next_of(pc, entry.first);
        if (!terminator(entry.second)) { sequential.insert(next); continue; }
        leaders.insert(next);
        for (size_t at = entry.second.find("BRANCH(0x"); at != std::string::npos;
             at = entry.second.find("BRANCH(0x", at + 1))
            leaders.insert(uint32_t(std::stoul(entry.second.substr(at + 9), nullptr, 16)));
    }
    for (const auto& [pc, entry] : bodies)
        if (!sequential.count(pc)) leaders.insert(pc);
    std::ofstream out(path);
    std::ostringstream dispatch;
    out << "/* Generated ARMv6-M blocks (META). */\n";
    size_t blocks = 0;
    for (uint32_t leader : leaders) {
        if (!bodies.count(leader)) continue;
        std::ostringstream code;
        code << std::hex;
        unsigned count = 0, bound = 0;
        uint32_t pc = leader;
        for (; count < 24; ++count) {
            const auto found = bodies.find(pc);
            if (found == bodies.end() || (count && leaders.count(pc))) break;
            const uint16_t op = found->second.first;
            const std::string& body = found->second.second;
            const bool branch = terminator(body);
            /* what can reach the machine: memory, the interrupt mask, a spin */
            const bool hardware = branch || body.find("LD") != std::string::npos ||
                                  body.find("ST") != std::string::npos || body.find("ldm_t") != std::string::npos ||
                                  body.find("stm_t") != std::string::npos || body.find("irq_work") != std::string::npos ||
                                  body.find("primask") != std::string::npos || body.find("stepTicks") != std::string::npos ||
                                  body.find("flash[") != std::string::npos;
            code << "{ const uint32_t pc = 0x" << pc << "u, op = 0x" << op << "u; (void)op;\n";
            if (hardware) {
                bound += 24;
                code << "w = nvm_miss(pc); fw0 = flashWaits; lastExecPc = pc; regs[15] = pc + 4u; cyc = 1;\n"
                     << "switch (0) { default: " << body << " goto ok" << count
                     << "; }\nregs[15] = pc + 2u; return 1;\nok" << count << ":\n"
                     << "w += flashWaits - fw0; step_flush(T, cyc + w); }\n";
            } else {
                /* nothing here can raise an event and the entry check left room
                 * for the whole block: the time is added without looking */
                bound += 2;
                code << "w = nvm_miss(pc); regs[15] = pc + 4u; cyc = 1;\n"
                     << "switch (0) { default: " << body << " goto ok" << count
                     << "; }\nregs[15] = pc + 2u; return 1;\nok" << count << ":\n"
                     << "tickCount += cyc + w; }\n";
            }
            pc = next_of(pc, op);
            if (branch) { ++count; break; }
            if (hardware && bodies.count(pc) && !leaders.count(pc))
                code << "if (__builtin_expect(((int32_t)(tickCount - evtAt) >= 0) | "
                        "((int32_t)(tickCount - emu_nextFrameTick) >= 0), 0)) return 1;\n";
        }
        if (!count) continue;
        out << std::dec << "static __attribute__((noinline)) int gm0_blk_" << leader << "(void) {\n"
               "const int T = TGT_META; uint32_t w, cyc, fw0 = 0;\n"
               "int32_t room = (int32_t)(timerDeadline - tickCount), r = (int32_t)(evtAt - tickCount);\n"
               "if (r < room) room = r;\n"
               "r = (int32_t)(emu_nextFrameTick - tickCount); if (r < room) room = r;\n"
               "if (room < " << bound << ") return 0;\n" << code.str() << "return 1;\n}\n";
        dispatch << "case 0x" << std::hex << leader << std::dec << "u: return gm0_blk_" << leader << "();\n";
        ++blocks;
    }
    out << "static int gm0_block_run(uint32_t pc) {\nswitch (pc) {\n" << dispatch.str()
        << "default: return 0; }\n}\n";
    return bool(out);
}


/* Standalone translation (META): one C function per 1 KiB of flash.  Inside
 * it every translated instruction has a label, direct branches inside the
 * page are gotos, and cycles are summed per chain.  See m0_native_glue.h for
 * where the machine is consulted. */
struct Entry { uint32_t pc; uint16_t op; unsigned size; std::string body; };

static std::string strip_dialect(const std::string& in) {
    std::string text = in;
    /* the guard on the second halfword (flash is read-only here) */
    const std::regex guard("if \\(\\(uint32_t\\)flash\\[pc \\+ 2u\\] \\+ \\(\\(uint32_t\\)flash\\[pc \\+ 3u\\] << 8\\) != 0x[0-9a-f]+u\\) break; ");
    text = std::regex_replace(text, guard, "");
    /* r15 is only written when a chain ends */
    const std::regex pc_constant("regs\\[15\\] = 0x[0-9a-f]+u;");
    text = std::regex_replace(text, pc_constant, ";");
    /* REV: shifts, not a libgcc call (Xtensa has no byte-swap instruction) */
    for (size_t at = text.find("__builtin_bswap32("); at != std::string::npos; at = text.find("__builtin_bswap32(", at))
        text.replace(at, 18, "m0_bswap32(");
    return text;
}
static unsigned max_cycles(const std::string& body) {
    unsigned most = 1;
    static const std::regex number("cyc = (0x[0-9a-f]+|[0-9]+)u?;");
    for (std::sregex_iterator it(body.begin(), body.end(), number), end; it != end; ++it)
        most = std::max<unsigned>(most, unsigned(std::stoul((*it)[1], nullptr, 0)));
    if (body.find("BRANCH(") != std::string::npos) most = std::max(most, 2u);
    return most;
}

bool write_native_pages(const fs::path& source,
                        const std::map<uint32_t, std::pair<uint16_t, std::string>>& bodies,
                        uint32_t base, const std::vector<uint8_t>& rom, std::string& error) {
    const size_t rom_size = rom.size();
    /* a load from a constant flash address (a literal pool word) is the constant */
    auto fold_literals = [&](std::string text) {
        static const std::regex literal("LD32\\(0x([0-9a-f]+)u\\)");
        std::string out;
        auto last = text.cbegin();
        for (std::sregex_iterator it(text.begin(), text.end(), literal), end; it != end; ++it) {
            const uint64_t at = std::stoull((*it)[1], nullptr, 16);
            out.append(last, text.cbegin() + it->position());
            last = text.cbegin() + it->position() + it->length();
            if (at >= base && at - base + 4 <= rom_size) {
                const uint32_t v = rom[at - base] | rom[at - base + 1] << 8 | rom[at - base + 2] << 16 |
                                   uint32_t(rom[at - base + 3]) << 24;
                char buffer[16];
                std::snprintf(buffer, sizeof buffer, "0x%xu", v);
                out += buffer;
            } else out.append(it->str());
        }
        out.append(last, text.cend());
        return out;
    };
    std::map<uint32_t, Entry> entries;
    for (const auto& [pc, e] : bodies)
        entries[pc] = {pc, e.first, (e.first & 0xf800u) >= 0xe800u ? 4u : 2u, fold_literals(strip_dialect(e.second))};
    auto branch_of = [](const Entry& e, uint32_t* target) {
        static const std::regex direct("BRANCH\\(0x([0-9a-f]+)u\\)");
        std::smatch m;
        size_t count = 0;
        for (size_t at = e.body.find("BRANCH("); at != std::string::npos; at = e.body.find("BRANCH(", at + 1)) ++count;
        if (count == 1 && std::regex_search(e.body, m, direct)) { *target = uint32_t(std::stoul(m[1], nullptr, 16)); return 1; }
        return count ? 2 : 0;                       /* 1: direct, 2: computed */
    };
    auto terminates = [](const Entry& e) { return e.body.find("BRANCH(") != std::string::npos; };
    auto unconditional = [&](const Entry& e) { return terminates(e) && e.body.rfind("if (", 0) != 0; };
    auto reaches_machine = [](const Entry& e) {
        for (const char* word : {"LD", "ST", "ldm_t", "stm_t", "irq_work", "primask", "idle_spin"})
            if (e.body.find(word) != std::string::npos) return true;
        return false;
    };
    /* Flags nobody reads before they are written again are not computed.  Backward
     * liveness over the translated instructions: where a successor is not
     * translated or not known (computed branch, return, exit to the
     * interpreter) every flag counts as live.  N=8 Z=4 C=2 V=1. */
    {
        enum : unsigned { FN = 8, FZ = 4, FC = 2, FV = 1, FALL = 15 };
        struct Use { unsigned reads = 0, kills = 0; bool opaque = false; };
        std::map<uint32_t, Use> use;
        auto flag_bit = [](char c) -> unsigned { return c == 'N' ? FN : c == 'Z' ? FZ : c == 'C' ? FC : FV; };
        for (const auto& [pc, e] : entries) {
            Use u;
            const std::string& b = e.body;
            const bool conditional = b.find("if (") != std::string::npos;
            for (size_t at = b.find('f'); at != std::string::npos; at = b.find('f', at + 1)) {
                if (at + 2 > b.size() || !(b[at + 1] == 'N' || b[at + 1] == 'Z' || b[at + 1] == 'C' || b[at + 1] == 'V')) continue;
                if (at && (std::isalnum((unsigned char)b[at - 1]) || b[at - 1] == '_')) continue;
                if (at + 2 < b.size() && (std::isalnum((unsigned char)b[at + 2]) || b[at + 2] == '_')) continue;
                const bool write = b.compare(at + 2, 3, " = ") == 0 && b.compare(at + 2, 4, " == ") != 0;
                if (write) { if (!conditional) u.kills |= flag_bit(b[at + 1]); else if (b[at + 1] == 'C') u.reads |= FC; }
                else u.reads |= flag_bit(b[at + 1]);
            }
            if (b.find("setNZ(") != std::string::npos) u.kills |= FN | FZ;
            if (b.find("addSetCond(") != std::string::npos) u.kills |= FALL;
            if (b.find("loop_spin") != std::string::npos || b.find("idle_spin") != std::string::npos ||
                b.find("return ") != std::string::npos || b.find("stepTicks") != std::string::npos ||
                b.find("m0_cpu_") != std::string::npos || b.find("exc_active") != std::string::npos)
                u.opaque = true;
            use[pc] = u;
        }
        std::map<uint32_t, unsigned> live_in, live_out;
        for (const auto& [pc, e] : entries) { live_in[pc] = FALL; live_out[pc] = FALL; }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                const Entry& e = it->second;
                const Use& u = use[e.pc];
                unsigned out = 0;
                uint32_t target = 0;
                const int kind = branch_of(e, &target);
                if (u.opaque || kind == 2) out = FALL;
                else {
                    if (kind == 1) out |= entries.count(target) ? live_in[target] : FALL;
                    if (!unconditional(e)) {
                        const uint32_t next = e.pc + e.size;
                        out |= entries.count(next) ? live_in[next] : FALL;
                    }
                }
                const unsigned in = u.opaque ? FALL : (u.reads | (out & ~u.kills));
                if (out != live_out[e.pc] || in != live_in[e.pc]) { live_out[e.pc] = out; live_in[e.pc] = in; changed = true; }
            }
        }
        auto close_of = [](const std::string& t, size_t open) {
            int depth = 0;
            for (size_t i = open; i < t.size(); ++i) {
                if (t[i] == '(') ++depth;
                else if (t[i] == ')' && --depth == 0) return i;
            }
            return std::string::npos;
        };
        auto rewrite = [&](std::string text, const char* name, const char* replacement, unsigned mask) {
            const std::string key = std::string(name) + "(";
            for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1)) {
                if (at && (std::isalnum((unsigned char)text[at - 1]) || text[at - 1] == '_')) continue;
                const size_t close = close_of(text, at + key.size() - 1);
                if (close == std::string::npos) continue;
                text.insert(close, ", " + std::to_string(mask));
                text.replace(at, key.size() - 1, replacement);
                at += std::strlen(replacement);
            }
            return text;
        };
        for (auto& [pc, e] : entries) {
            if (use[pc].opaque) continue;
            const unsigned live = live_out[pc];
            e.body = rewrite(e.body, "setNZ", "setNZm", live & (FN | FZ));
            e.body = rewrite(e.body, "addSetCond", "addSetCondM", live & FALL);
            for (const char f : {'N', 'Z', 'C', 'V'}) {
                if (live & flag_bit(f)) continue;
                const std::string key = std::string("f") + f + " = ";
                for (size_t at = e.body.find(key); at != std::string::npos; at = e.body.find(key, at)) {
                    if (e.body.compare(at, key.size() + 1, key + "=") == 0 || (at && (std::isalnum((unsigned char)e.body[at - 1]) || e.body[at - 1] == '_'))) { ++at; continue; }
                    const size_t semi = e.body.find(';', at);
                    if (semi == std::string::npos) break;
                    e.body.erase(at, semi - at + 1);
                }
            }
        }
    }
    /* leaders: branch targets, what follows a branch, what nothing falls into;
     * and every 24 straight-line instructions, so no chain outgrows its room */
    std::set<uint32_t> leaders;
    for (const auto& [pc, e] : entries) {
        uint32_t target = 0;
        const int kind = branch_of(e, &target);
        if (kind == 1 && entries.count(target)) leaders.insert(target);
        if (terminates(e)) leaders.insert(pc + e.size);
        const auto previous = entries.find(pc);
        bool fed = false;
        if (previous != entries.begin()) {
            const Entry& before = std::prev(previous)->second;
            fed = before.pc + before.size == pc && !unconditional(before);
        }
        if (!fed) leaders.insert(pc);
    }
    {
        unsigned run = 0;
        for (const auto& [pc, e] : entries) {
            if (leaders.count(pc)) run = 0;
            if (++run > 24) { leaders.insert(pc); run = 1; }
        }
    }
    /* bound: the longest a chain can run from here to its next check */
    std::map<uint32_t, unsigned> bound;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const Entry& e = it->second;
        const unsigned own = max_cycles(e.body);
        const auto next = entries.find(e.pc + e.size);
        const bool ends = terminates(e) || next == entries.end() || leaders.count(e.pc + e.size);
        bound[e.pc] = own + (ends ? 0u : bound[e.pc + e.size]);
    }
    std::error_code ec;
    for (const auto& file : fs::directory_iterator(source)) {
        const auto name = file.path().filename().string();
        if (name.starts_with("m0_native_page_") && file.path().extension() == ".c") fs::remove(file.path(), ec);
    }
    struct Kernel { uint32_t pc; const char* name; std::vector<uint16_t> code; };
    static const std::vector<Kernel> kernels = {
        {0xae08, "ae08", {0x6a79, 0x428b, 0xda0d, 0x6af8, 0x009a, 0x18c1, 0x6ab8, 0x0049, 0x6900, 0x3301,
                          0x5a09, 0x18a8, 0xba49, 0x8041, 0xb29b, 0x52a9, 0xe7ee}},
    };
    auto matches = [&](const Kernel& k) {
        if (k.pc < base || k.pc - base + k.code.size() * 2 > rom.size()) return false;
        for (size_t i = 0; i < k.code.size(); ++i)
            if (half(rom, k.pc - base + i * 2) != k.code[i]) return false;
        return true;
    };
    std::map<uint32_t, std::vector<const Entry*>> pages;       /* 1 KiB pages */
    for (const auto& [pc, e] : entries) pages[pc >> 10].push_back(&e);
    std::map<uint32_t, std::ostringstream> files;              /* 4 KiB per file */
    std::ostringstream dispatch, header;
    dispatch << "#include \"m0_native_glue.h\"\n#include \"m0_native_pages.h\"\n#include \"samd21.h\"\n"
                "/* Flash is read-only here: translated code never re-reads its opcodes.\n"
                " * A page returns 0 (nothing run), 1 (run, the clock has been ticked: a\n"
                " * machine access or an exception return) or 2 (run, cycles on the\n"
                " * clock, nothing else can have happened inside `room`): the next page\n"
                " * is entered at once, and the machine ticks once when the chain stops. */\n"
                "int m0_native_step(M0Cpu *m) {\n"
                "  uint32_t room = samd21_room((const Samd21 *)m->board);\n"
                "  if (!room) return 0;\n"
                "  const uint64_t start = m->cycles;\n  int ran = 0;\n"
                "  for (;;) {\n"
                "    uint32_t pc = m->r[15], used = (uint32_t)(m->cycles - start);\n"
                "    if (pc - M0_FLASH_BASE >= " << rom_size << "u || used >= room) break;\n"
                "    const uint32_t left = room - used;\n    int r;\n"
                "    switch (pc >> 10) {\n";
    for (const auto& [page, list] : pages) {
        std::ostringstream& out = files[(page << 10) >> 12];
        if (out.tellp() == 0) out << "#include \"m0_native_glue.h\"\n#include \"m0_native_pages.h\"\n";
        std::ostringstream name;
        name << "m0_page_" << std::hex << page;
        header << "int " << name.str() << "(M0Cpu *m, uint32_t pc, uint32_t room);\n";
        dispatch << "    case 0x" << std::hex << page << "u: r = " << name.str() << "(m, pc, left); break;\n";
        out << "int " << name.str() << "(M0Cpu *m, uint32_t pc, uint32_t room) {\n"
               "uint32_t c = 0; int hw = 0; (void)hw;\nswitch (pc) {\n";
        /* Only addresses something can resume at get a case: leaders, what follows
         * a machine access, what follows a gap, the page start, and every 4th
         * instruction (interrupt returns land anywhere; the interpreter steps
         * the few instructions up to the next entry). */
        for (size_t i = 0; i < list.size(); ++i) {
            const Entry* e = list[i];
            const bool prev_adjacent = i && list[i - 1]->pc + list[i - 1]->size == e->pc;
            const bool entry = !i || !prev_adjacent || leaders.count(e->pc) || reaches_machine(*list[i - 1]) || i % 4 == 0;
            if (!entry) continue;
            out << "case 0x" << std::hex << e->pc << "u: if (room < " << std::dec << bound[e->pc]
                << "u) return 0; goto i_" << std::hex << e->pc << ";\n";
        }
        out << "default: return 0;\n}\n";
        std::set<uint32_t> here;
        for (const Entry* e : list) here.insert(e->pc);
        for (size_t i = 0; i < list.size(); ++i) {
            const Entry& e = *list[i];
            const uint32_t next = e.pc + e.size;
            std::ostringstream line;
            line << std::hex;
            if (leaders.count(e.pc))
                line << "L_" << e.pc << ": if ((int32_t)(room - c) < " << std::dec << bound[e.pc] << std::hex
                     << ") { m->r[15] = 0x" << e.pc << "u; goto out; }\n";
            /* hand-written loop kernels (m0_native_kernels.h), only for the exact code they were written for */
            for (const Kernel& k : kernels)
                if (k.pc == e.pc && leaders.count(e.pc) && matches(k)) {
                    line << "if (m0k_" << k.name << "(m, &c, room, " << std::dec << bound[e.pc] << "u)) { }\n" << std::hex;
                }
            line << "i_" << e.pc << ": { uint32_t cyc = 1; int br = 0; uint32_t bt = 0; (void)bt; (void)br;\n"
                 << e.body << "\nc += cyc;\n";
            uint32_t target = 0;
            const int kind = branch_of(e, &target);
            if (kind) {
                line << "if (br) { ";
                if (kind == 1 && here.count(target))
                    line << "if (hw) { m->r[15] = 0x" << target << "u; goto out; } goto L_" << target << ";";
                else if (kind == 1) line << "m->r[15] = 0x" << target << "u; goto out;";
                else line << "m0_native_branch(m, bt); hw = bt >= 0xfffffff0u; goto out;";
                line << " }\n";
            }
            if (reaches_machine(e)) line << "if (hw) { m->r[15] = 0x" << next << "u; goto out; }\n";
            line << "}\n";
            const bool flows = !unconditional(e);
            const bool adjacent = i + 1 < list.size() && list[i + 1]->pc == next;
            if (flows && !adjacent) line << "m->r[15] = 0x" << next << "u; goto out;\n";
            out << line.str();
        }
        out << "out: if (hw) { m0_native_finish(m, c); return 1; }\nm->cycles += c; return 2;\n}\n";
    }
    for (auto& [file, text] : files) {
        std::ofstream unit(source / ("m0_native_page_" + [&] { std::ostringstream n; n << std::hex << (file << 12); return n.str(); }() + ".c"));
        unit << text.str();
        if (!unit) { error = "Cannot write M0+ native page"; return false; }
    }
    dispatch << "    default: r = 0; break;\n    }\n"
                "    if (!r) break;\n    ran = 1;\n    if (r == 1) return 1;\n  }\n"
                "  if (ran && m->event) m->event(m->board, m, 0);\n  return ran;\n}\n";
    std::ofstream(source / "m0_native.c") << dispatch.str();
    std::ofstream(source / "m0_native_pages.h") << header.str();
    (void)base;
    return true;
}

bool generate(const fs::path& firmware, SystemId system, const fs::path& output,
              std::string& error, const fs::path& runtime_dir,
              const std::vector<fs::path>& coverage, bool aka) {
    if (system != SystemId::meta && system != SystemId::pokitto) {
        error = "M0+ project requires --system meta or pokitto";
        return false;
    }
    if (aka && system != SystemId::meta) {
        error = "Only the META firmware has an AKA project";
        return false;
    }
    const auto inspector = system_descriptor(system).inspect;
    if (!inspector(firmware, error)) return false;
    std::error_code ec;
    if (fs::exists(output, ec) && !fs::is_directory(output, ec)) {
        error = "Output is not a directory: " + output.string();
        return false;
    }
    const fs::path marker = output / "m0plus-project.txt";
    if (fs::exists(output, ec) && !fs::is_empty(output, ec)) {
        std::ifstream previous(marker);
        std::string signature;
        std::getline(previous, signature);
        if (signature != std::string(system == SystemId::meta ? "meta" : "pokitto") + (aka ? "-aka" : "-pc")) {
            error = "Output directory belongs to another project";
            return false;
        }
    }
    fs::create_directories(output, ec);
    if (ec) { error = "Cannot create output directory: " + ec.message(); return false; }
    const fs::path destination = output / "firmware.bin";
    if (!fs::equivalent(firmware, destination, ec)) {
        ec.clear();
        fs::copy_file(firmware, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "Cannot copy firmware: " + ec.message(); return false; }
    }
    const char* board = system == SystemId::meta ? "meta" : "pokitto";
    if (!runtime_dir.empty()) {
        const fs::path source = output / "src";
        fs::create_directories(source, ec);
        if (ec) { error = "Cannot create native source directory: " + ec.message(); return false; }
        const fs::path host = runtime_dir.parent_path() / "host/m0_native_host.c";
        std::vector<fs::path> shared = {runtime_dir / "m0_cpu.h", runtime_dir / "m0_cpu.c",
                                        runtime_dir / "m0_machine.h", runtime_dir / "m0_machine.c"};
        if (system != SystemId::meta) shared.push_back(host);
        for (const fs::path& file : shared) {
            ec.clear();
            fs::copy_file(file, source / file.filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) { error = "Cannot copy M0+ runtime " + file.string() + ": " + ec.message(); return false; }
        }
        const bool standalone = system == SystemId::meta;
        if (standalone) {
            const fs::path platform = runtime_dir.parent_path() / "platform";
            for (const fs::path& file : {runtime_dir / "samd21.h", runtime_dir / "samd21.c",
                                         runtime_dir / "m0_native_glue.h", runtime_dir / "m0_native_kernels.h", platform / (aka ? "m0_aka_native.cpp" : "m0_pc.c")}) {
                ec.clear();
                fs::copy_file(file, source / file.filename(), fs::copy_options::overwrite_existing, ec);
                if (ec) { error = "Cannot copy M0+ board " + file.string() + ": " + ec.message(); return false; }
            }
        } else {
            const fs::path gm0 = runtime_dir.parent_path() / "gm0";
        for (const char* name : {"gm0.c", "aka.c", "aka.h", "esp8266.h",
                                 "esp8266_xt.h", "esp8266_rom_syms.h"}) {
            ec.clear();
            fs::copy_file(gm0 / name, source / name, fs::copy_options::overwrite_existing, ec);
            if (ec) { error = "Cannot copy integrated gm0 component: " + ec.message(); return false; }
        }
        ec.clear();
        fs::copy_file(gm0 / "LICENSE", output / "GM0-LICENSE", fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "Cannot copy integrated gm0 license: " + ec.message(); return false; }
        }
        std::ifstream binary(destination, std::ios::binary);
        const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{binary}, {});
        if (bytes.empty()) { error = "Cannot read firmware for M0+ C translation"; return false; }
        const uint32_t base = system == SystemId::meta ? 0x4000u : 0u;
        const auto compiled = walk(bytes, base, coverage, error);
        if (!error.empty()) return false;
        size_t board_native = 0;
        {
            std::ofstream code, dispatch;
            if (system != SystemId::meta) {
                code.open(source / "gm0_native.inc");
                dispatch.open(source / "gm0_native_dispatch.inc");
            }
            std::map<uint32_t, std::string> pages;
            std::map<uint32_t, std::pair<uint16_t, std::string>> bodies;
            for (uint32_t pc : compiled) {
                const uint16_t op = half(bytes, pc - base);
                std::ostringstream body;
                body << std::hex;
                const unsigned rd = (op >> 8) & 7u, imm = op & 255u;
                if ((op & 0xe000u) == 0 && (op & 0xf800u) < 0x1800u) {
                    const unsigned dst = op & 7u, src = (op >> 3) & 7u;
                    const unsigned kind = op >> 11, shift = (op >> 6) & 31u;
                    if (kind == 0) {
                        body << "{ uint32_t v = regs[" << src << "]; ";
                        if (shift) body << "fC = (v >> " << std::dec << 32u - shift << std::hex
                                        << ") & 1u; v <<= 0x" << shift << "u; ";
                        body << "regs[" << dst << "] = v; setNZ(v); }";
                    } else if (kind == 1) {
                        const unsigned n = shift ? shift : 32u;
                        body << "{ uint32_t v = regs[" << src << "]; fC = (v >> " << std::dec
                             << n - 1u << ") & 1u; " << std::hex;
                        if (n == 32) body << "v = 0; "; else body << "v >>= 0x" << n << "u; ";
                        body << "regs[" << dst << "] = v; setNZ(v); }";
                    } else {
                        const unsigned n = shift ? shift : 32u;
                        body << "{ int32_t v = (int32_t)regs[" << src << "]; fC = ((uint32_t)(v >> "
                             << std::dec << n - 1u << ")) & 1u; " << std::hex
                             << "regs[" << dst << "] = (uint32_t)(v >> " << std::dec
                             << (n == 32 ? 31 : n) << "); setNZ(regs[" << dst << "]); }" << std::hex;
                    }
                } else if ((op & 0xf800u) == 0x1800u) {
                    const unsigned dst = op & 7u, lhs = (op >> 3) & 7u, rhs = (op >> 6) & 7u;
                    const bool sub = op & 0x200u;
                    body << "regs[" << dst << "] = addSetCond(regs[" << lhs << "], "
                         << (sub ? "~" : "") << (op & 0x400u ? "0x" : "regs[") << rhs
                         << (op & 0x400u ? "u" : "]") << ", " << sub << ");";
                } else if ((op & 0xf800u) == 0x2000u)
                    body << "regs[" << rd << "] = 0x" << imm << "u; fN = 0; fZ = " << (imm == 0) << ";";
                else if ((op & 0xf800u) == 0x2800u)
                    body << "addSetCond(regs[" << rd << "], ~0x" << imm << "u, 1);";
                else if ((op & 0xf800u) == 0x3000u)
                    body << "regs[" << rd << "] = addSetCond(regs[" << rd << "], 0x" << imm << "u, 0);";
                else if ((op & 0xf800u) == 0x3800u)
                    body << "regs[" << rd << "] = addSetCond(regs[" << rd << "], ~0x" << imm << "u, 1);";
                else if ((op & 0xfc00u) == 0x4000u) {
                    const unsigned dst = op & 7u, src = (op >> 3) & 7u, fn = (op >> 6) & 15u;
                    const char* operators[] = {"&", "^", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                               nullptr, nullptr, nullptr, nullptr, "|", "*", "& ~", nullptr};
                    if (operators[fn]) body << "regs[" << dst << "] = regs[" << dst << "] " << operators[fn]
                                             << " regs[" << src << "]; setNZ(regs[" << dst << "]);";
                    else if (fn == 15) body << "regs[" << dst << "] = ~regs[" << src << "]; setNZ(regs[" << dst << "]);";
                    else if (fn == 8) body << "setNZ(regs[" << dst << "] & regs[" << src << "]);";
                    else if (fn == 9) body << "regs[" << dst << "] = addSetCond(0, ~regs[" << src << "], 1);";
                    else if (fn == 10 || fn == 11) body << "addSetCond(regs[" << dst << "], " << (fn == 10 ? "~" : "")
                                                           << "regs[" << src << "], " << (fn == 10) << ");";
                    else if (fn == 5 || fn == 6) body << "regs[" << dst << "] = addSetCond(regs[" << dst << "], "
                                                          << (fn == 6 ? "~" : "") << "regs[" << src << "], fC);";
                    else if (fn >= 2 && fn <= 4) {
                        body << "{ uint32_t a = regs[" << dst << "], sh = regs[" << src << "] & 255u, r = a; ";
                        if (fn == 2) body << "if (sh) { if (sh < 32) { fC = (a >> (32 - sh)) & 1; r = a << sh; } else { fC = sh == 32 ? a & 1 : 0; r = 0; } } ";
                        if (fn == 3) body << "if (sh) { if (sh < 32) { fC = (a >> (sh - 1)) & 1; r = a >> sh; } else { fC = sh == 32 ? a >> 31 : 0; r = 0; } } ";
                        if (fn == 4) body << "if (sh) { if (sh < 32) { fC = ((uint32_t)((int32_t)a >> (sh - 1))) & 1; r = (uint32_t)((int32_t)a >> sh); } else { fC = a >> 31; r = (uint32_t)((int32_t)a >> 31); } } ";
                        body << "regs[" << dst << "] = r; setNZ(r); }";
                    } else if (fn == 7) {
                        body << "{ uint32_t a = regs[" << dst << "], sh = regs[" << src
                             << "] & 255u, r = a; if (sh) { sh &= 31; if (sh) r = (a >> sh) | (a << (32 - sh)); fC = r >> 31; } regs["
                             << dst << "] = r; setNZ(r); }";
                    }
                } else if ((op & 0xff00u) >= 0x4400u && (op & 0xff00u) <= 0x4600u) {
                    const unsigned dst = (op & 7u) | ((op >> 4) & 8u), src = (op >> 3) & 15u;
                    const unsigned kind = (op >> 8) & 3u;
                    body << std::dec;
                    if (kind == 1) body << "addSetCond(regs[" << dst << "], ~regs[" << src << "], 1);";
                    else {
                        body << "{ uint32_t v = " << (kind == 0 ? "regs[" + std::to_string(dst) + "] + " : "")
                             << "regs[" << src << "]; ";
                        if (dst == 15) body << "BRANCH(v);"; else body << "regs[" << dst << "] = v;";
                        body << " }";
                    }
                    body << std::hex;
                } else if ((op & 0xff00u) == 0x4700u) {
                    const unsigned src = (op >> 3) & 15u;
                    body << std::dec << "{ uint32_t to = regs[" << src << "]; " << std::hex;
                    if (op & 0x80u) body << "if (T == TGT_POKITTO) { pk_blx(op); stepTicks = 0; cyc = 2; } else { regs[14] = 0x"
                                             << ((pc + 2u) | 1u) << "u; BRANCH(to); }";
                    else body << "BRANCH(to);";
                    body << " }";
                }
                else if ((op & 0xf800u) == 0x4800u)
                    body << "regs[" << rd << "] = LD32(0x" << (((pc + 4u) & ~3u) + imm * 4u) << "u); cyc = 2;";
                else if ((op & 0xf000u) == 0x5000u) {
                    const unsigned dst = op & 7u, src = (op >> 3) & 7u, index = (op >> 6) & 7u;
                    const unsigned kind = (op >> 9) & 7u;
                    static const unsigned bits[] = {32,16,8,8,32,16,8,16};
                    if (kind < 3) body << "ST" << std::dec << bits[kind] << std::hex << "(regs[" << src << "] + regs[" << index << "], regs[" << dst << "]);";
                    else {
                        body << "regs[" << dst << "] = ";
                        if (kind == 3 || kind == 7) body << "(uint32_t)(int32_t)(int" << std::dec << bits[kind] << "_t)" << std::hex;
                        body << "LD" << std::dec << bits[kind] << std::hex << "(regs[" << src << "] + regs[" << index << "]);";
                    }
                    body << " cyc = 2;";
                }
                else if ((op & 0xe000u) == 0x6000u || (op & 0xf000u) == 0x8000u) {
                    const unsigned dst = op & 7u, src = (op >> 3) & 7u;
                    const unsigned width = (op & 0xf000u) == 0x8000u ? 2u : (op & 0x1000u ? 1u : 4u);
                    const unsigned offset = ((op >> 6) & 31u) * width;
                    if (op & 0x800u) body << "regs[" << dst << "] = LD" << std::dec << width * 8u << std::hex
                                          << "(regs[" << src << "] + 0x" << offset << "u);";
                    else body << "ST" << std::dec << width * 8u << std::hex << "(regs[" << src
                              << "] + 0x" << offset << "u, regs[" << dst << "]);";
                    body << " cyc = 2;";
                } else if ((op & 0xf000u) == 0x9000u) {
                    if (op & 0x800u) body << "regs[" << rd << "] = LD32(regs[13] + 0x" << imm * 4u << "u);";
                    else body << "ST32(regs[13] + 0x" << imm * 4u << "u, regs[" << rd << "]);";
                    body << " cyc = 2;";
                } else if ((op & 0xf800u) == 0xa000u)
                    body << "regs[" << rd << "] = 0x" << (((pc + 4u) & ~3u) + imm * 4u) << "u;";
                else if ((op & 0xf800u) == 0xa800u)
                    body << "regs[" << rd << "] = regs[13] + 0x" << imm * 4u << "u;";
                else if ((op & 0xff00u) == 0xb000u)
                    body << "regs[13] " << (op & 0x80u ? "-= " : "+= ") << "0x" << ((op & 0x7fu) * 4u) << "u;";
                else if ((op & 0xff00u) == 0xb200u) {
                    const unsigned dst = op & 7u, src = (op >> 3) & 7u, kind = (op >> 6) & 3u;
                    static const char* cast[] = {"(uint32_t)(int32_t)(int16_t)", "(uint32_t)(int32_t)(int8_t)", "(uint16_t)", "(uint8_t)"};
                    body << "regs[" << dst << "] = " << cast[kind] << "regs[" << src << "];";
                } else if ((op & 0xff00u) == 0xba00u) {
                    const unsigned dst = op & 7u, src = (op >> 3) & 7u, kind = (op >> 6) & 3u;
                    if (kind == 0) body << "regs[" << dst << "] = __builtin_bswap32(regs[" << src << "]);";
                    else if (kind == 1) body << "{ uint32_t v = regs[" << src << "]; regs[" << dst << "] = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8); }";
                    else if (kind == 3) body << "{ uint32_t v = regs[" << src << "]; regs[" << dst << "] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((v & 255u) << 8) | ((v >> 8) & 255u)); }";
                    else body << "regs[" << dst << "] = regs[" << src << "];";
                } else if ((op & 0xff00u) == 0xb600u) {
                    if ((op & 0xffefu) == 0xb662u) {
                        body << "primask = " << !!(op & 0x10u) << "; ";
                        if (!(op & 0x10u)) body << "irq_work();";
                    } else body << ";";
                }
                else if ((op & 0xfe00u) == 0xb400u) {
                    const unsigned list = (op & 0xffu) | ((op & 0x100u) << 6);
                    const unsigned n = __builtin_popcount(list);
                    body << "{ uint32_t a = regs[13] - 0x" << n * 4u << "u; regs[13] = a; stm_t(T, a, 0x"
                         << list << "u, 0x" << n << "u); cyc = 0x" << n + 1u << "u; }";
                } else if ((op & 0xfe00u) == 0xbc00u) {
                    const unsigned list = (op & 0xffu) | ((op & 0x100u) << 7);
                    const unsigned n = __builtin_popcount(list);
                    body << "{ uint32_t a = regs[13]; ldm_t(T, a, 0x" << list << "u, 0x" << n
                         << "u, &w); regs[13] = a + 0x" << n * 4u << "u; ";
                    if (op & 0x100u) body << "BRANCH(regs[15]); cyc = 0x" << n + 3u << "u;";
                    else body << "cyc = 0x" << n + 1u << "u;";
                    body << " }";
                } else if ((op & 0xf000u) == 0xc000u) {
                    const unsigned list = op & 255u, n = __builtin_popcount(list);
                    body << "{ uint32_t a = regs[" << rd << "]; ";
                    if (op & 0x800u) {
                        body << "ldm_t(T, a, 0x" << list << "u, 0x" << n << "u, &w); ";
                        if (!(list & (1u << rd))) body << "regs[" << rd << "] = a + 0x" << n * 4u << "u; ";
                    } else body << "stm_t(T, a, 0x" << list << "u, 0x" << n << "u); regs[" << rd << "] = a + 0x" << n * 4u << "u; ";
                    body << "cyc = 0x" << n + 1u << "u; }";
                } else if ((op & 0xf000u) == 0xd000u && ((op >> 8) & 15u) < 14u) {
                    static const char* conditions[] = {"fZ", "!fZ", "fC", "!fC", "fN", "!fN", "fV", "!fV",
                        "fC && !fZ", "!fC || fZ", "fN == fV", "fN != fV", "!fZ && fN == fV", "fZ || fN != fV"};
                    const uint32_t to = pc + 4u + uint32_t(int32_t(int8_t(op)) * 2);
                    /* ldr/ldrb/ldrh rt,[rn,#i]; cmp rt,#n; bcond back: a poll on
                     * a RAM word that only an event can change.  The runtime
                     * spends the wait up to the next event (gm0 idle_spin). */
                    std::string spin;
                    if (to + 4u == pc && pc - base >= 4u && to >= base) {
                        const uint16_t ld = half(bytes, to - base), cmp = half(bytes, to + 2u - base);
                        const unsigned kind = ld & 0xf800u;
                        if ((kind == 0x6800u || kind == 0x7800u || kind == 0x8800u) &&
                            (cmp & 0xf800u) == 0x2800u && ((cmp >> 8) & 7u) == (ld & 7u) &&
                            (ld & 7u) != ((ld >> 3) & 7u)) {
                            const unsigned imm5 = (ld >> 6) & 31u, scale = kind == 0x6800u ? 4u : kind == 0x8800u ? 2u : 1u;
                            std::ostringstream address;
                            address << "idle_spin(0x" << std::hex << pc << "u, regs[" << std::dec << ((ld >> 3) & 7u)
                                    << "] + " << imm5 * scale << "u); ";
                            spin = address.str();
                        }
                    }
                    if (spin.empty() && to < pc && pc - to <= 40u) {
                        /* a short loop with no store of its own (calls aside) */
                        bool stores = false;
                        for (uint32_t at = to; at < pc; at += 2) {
                            const uint16_t w = half(bytes, at - base);
                            if ((w & 0xe000u) == 0x6000u && !(w & 0x0800u)) stores = true;           /* STR/STRB */
                            if ((w & 0xf000u) == 0x8000u && !(w & 0x0800u)) stores = true;           /* STRH */
                            if ((w & 0xf000u) == 0x9000u && !(w & 0x0800u)) stores = true;           /* STR sp */
                            if ((w & 0xf000u) == 0x5000u && ((w >> 9) & 7u) < 3u) stores = true;    /* STR reg */
                            if ((w & 0xf800u) == 0xc000u || (w & 0xfe00u) == 0xb400u) stores = true; /* STM, PUSH */
                        }
                        if (!stores) spin = "loop_spin(0x" + [&] { std::ostringstream n; n << std::hex << pc; return n.str(); }() + "u); ";
                    }
                    body << "if (" << conditions[(op >> 8) & 15u] << ") { " << spin << "BRANCH(0x" << to << "u); }";
                }
                else if ((op & 0xf800u) == 0xe000u) {
                    const int32_t off = int32_t((op & 0x7ffu) << 21) >> 20;
                    body << "BRANCH(0x" << pc + 4u + uint32_t(off) << "u);";
                } else if ((op & 0xf800u) == 0xf000u && pc - base + 3u < bytes.size() &&
                           (half(bytes, pc - base + 2u) & 0xd000u) == 0xd000u) {
                    const uint16_t op2 = half(bytes, pc - base + 2u);
                    const uint32_t s = (op >> 10) & 1u;
                    const uint32_t i1 = !(((op2 >> 13) & 1u) ^ s), i2 = !(((op2 >> 11) & 1u) ^ s);
                    const uint32_t bits = (s << 24) | (i1 << 23) | (i2 << 22) |
                                          ((op & 0x3ffu) << 12) | ((op2 & 0x7ffu) << 1);
                    const uint32_t to = pc + 4u + uint32_t(int32_t(bits << 7) >> 7);
                    body << "if ((uint32_t)flash[pc + 2u] + ((uint32_t)flash[pc + 3u] << 8) != 0x"
                         << op2 << "u) break; regs[14] = 0x" << ((pc + 4u) | 1u)
                         << "u; BRANCH(0x" << to << "u); cyc = 3;";
                } else if ((op & 0xf800u) == 0xf000u && pc - base + 3u < bytes.size()) {
                    const uint16_t op2 = half(bytes, pc - base + 2u);
                    const unsigned sysm = op2 & 255u, reg = (op2 >> 8) & 15u;
                    body << "if ((uint32_t)flash[pc + 2u] + ((uint32_t)flash[pc + 3u] << 8) != 0x"
                         << op2 << "u) break; cyc = 4; regs[15] = 0x" << pc + 6u << "u; ";
                    if (op == 0xf3efu && (op2 & 0xf000u) == 0x8000u) {
                        body << "{ uint32_t v = 0, apsr = (uint32_t)fN << 31 | (uint32_t)fZ << 30 | (uint32_t)fC << 29 | (uint32_t)fV << 28; "
                                "uint32_t ipsr = T == TGT_META ? (uint32_t)exc_active() : 0; ";
                        if (sysm <= 3) body << "v = apsr " << (sysm & 1u ? "| ipsr" : "") << "; ";
                        else if (sysm >= 5 && sysm <= 7 && (sysm & 1u)) body << "v = ipsr " << (sysm == 7 ? "| apsr" : "") << "; ";
                        else if (sysm == 8 || sysm == 9) body << "v = regs[13]; ";
                        else if (sysm == 16) body << "v = (uint32_t)primask; ";
                        body << "regs[" << std::dec << reg << std::hex << "] = v; }";
                    } else if ((op & 0xfff0u) == 0xf380u && (op2 & 0xff00u) == 0x8800u) {
                        body << "{ uint32_t v = regs[" << std::dec << (op & 15u) << std::hex << "]; ";
                        if (sysm <= 3) body << "fN = v >> 31; fZ = (v >> 30) & 1u; fC = (v >> 29) & 1u; fV = (v >> 28) & 1u; ";
                        else if (sysm == 8 || sysm == 9) body << "regs[13] = v & ~3u; ";
                        else if (sysm == 16) body << "primask = v & 1u; irq_work(); ";
                        body << "}";
                    }
                } else if ((op & 0xf800u) == 0xe800u || (op & 0xf800u) == 0xf800u)
                    body << "regs[15] = 0x" << pc + 6u << "u;";
                else if ((op & 0xff00u) == 0xbf00u || (op & 0xff00u) == 0xbe00u ||
                         (op & 0xfe00u) == 0xde00u || (op & 0xf000u) == 0xb000u) body << ";";
                else continue;
                bodies[pc] = {op, body.str()};
                std::ostringstream entry;
                entry << "case 0x" << std::hex << pc << "u: if (op == 0x" << op
                      << "u) { " << body.str() << " goto gm0_native_done; } break;\n";
                pages[pc & ~0xfffu] += entry.str();
                ++board_native;
            }
            code << "/* Generated ARMv6-M entries, partitioned by flash page. */\n";
            for (const auto& [page, entries] : pages) {
                code << "static __attribute__((noinline)) int gm0_page_" << std::hex << page
                     << "(int T, uint32_t pc, uint32_t op, uint32_t *waits, uint32_t *cycles) {\n"
                        "uint32_t w = *waits, cyc = *cycles; switch (pc) {\n" << entries
                     << "default: break; } return 0;\ngm0_native_done: *waits = w; *cycles = cyc; return 1;\n}\n";
                dispatch << "case 0x" << std::hex << page << "u: return gm0_page_" << page
                         << "(T, pc, op, waits, cycles);\n";
            }
            if (system != SystemId::meta && (!code || !dispatch)) { error = "Cannot write gm0 native entries"; return false; }
            if (system == SystemId::meta && !write_native_pages(source, bodies, base, bytes, error))
                return false;
        }
        {
            std::ofstream rom(source / "m0_rom.c");
            rom << "#include <stdint.h>\nconst uint32_t m0_rom_size = " << bytes.size()
                << "u;\nconst uint8_t m0_rom[] = {\n";
            for (size_t i = 0; i < bytes.size(); ++i) {
                rom << "0x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]) << ',';
                if ((i & 15u) == 15u) rom << '\n';
            }
            rom << "\n};\n";
            if (!rom) { error = "Cannot write embedded M0+ firmware"; return false; }
        }
        if (system != SystemId::meta) {
            std::ofstream bundle(source / "gm0_bundle.h");
            bundle << "#include <stdint.h>\nextern const uint8_t m0_rom[" << bytes.size()
                   << "];\n#define gm0_bundle_data m0_rom\n#define gm0_bundle_name \"firmware.bin\"\n";
            if (!bundle) { error = "Cannot write embedded firmware declaration"; return false; }
        }
        if (system != SystemId::meta) {
            std::ofstream code(source / "m0_native.c");
            code << "#include \"m0_cpu.h\"\n"
                    "/* Fixed flash addresses dispatch to constant Thumb opcodes. "
                    "RAM code and modified flash fall back to m0_cpu_step. */\n"
                    "int m0_native_step(M0Cpu *m) {\n"
                    "  if (m->fault) return 0;\n"
                    "  switch (m->r[15]) {\n";
            for (uint32_t pc : compiled) {
                const size_t i = pc - base;
                const uint16_t op = uint16_t(bytes[i]) | uint16_t(bytes[i + 1]) << 8;
                const bool wide = (op & 0xf800u) >= 0xe800u && i + 3 < bytes.size();
                const uint16_t op2 = wide ? uint16_t(bytes[i + 2]) | uint16_t(bytes[i + 3]) << 8 : 0;
                code << "  case 0x" << std::hex << pc << "u:\n"
                     << "    if (m->read(m->board, 0x" << pc << "u, 2) != 0x" << op << "u";
                if (wide) code << " || m->read(m->board, 0x" << pc + 2 << "u, 2) != 0x" << op2 << "u";
                code << ") break;\n";
                if ((op & 0xf800u) == 0x2000u) { // MOVS Rd, #imm8
                    const unsigned imm = op & 255u, dst = (op >> 8) & 7u;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; m->r[" << dst
                         << "] = 0x" << imm << "u; m->n = 0; m->z = " << (imm == 0 ? 1 : 0)
                         << "; m0_native_finish(m, 1); return 1;\n";
                } else if ((op & 0xe000u) == 0x2000u) { // CMP/ADD/SUB immediate
                    const unsigned dst = (op >> 8) & 7u, imm = op & 255u;
                    const unsigned kind = (op >> 11) & 3u;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; ";
                    if (kind == 1) code << "(void)m0_native_sub(m, m->r[" << dst << "], 0x" << imm << "u); ";
                    else code << "m->r[" << dst << "] = m0_native_" << (kind == 2 ? "add" : "sub")
                              << "(m, m->r[" << dst << "], " << (kind == 2 ? "0x" : "0x") << imm
                              << "u" << (kind == 2 ? ", 0" : "") << "); ";
                    code << "m0_native_finish(m, 1); return 1;\n";
                } else if ((op & 0xf800u) == 0x1800u) { // ADD/SUB low registers or imm3
                    const unsigned dst = op & 7u, lhs = (op >> 3) & 7u, rhs = (op >> 6) & 7u;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; m->r[" << dst
                         << "] = m0_native_" << (op & 0x200u ? "sub" : "add") << "(m, m->r["
                         << lhs << "], " << (op & 0x400u ? "0x" : "m->r[") << rhs
                         << (op & 0x400u ? "u" : "]") << (op & 0x200u ? "" : ", 0")
                         << "); m0_native_finish(m, 1); return 1;\n";
                } else if ((op & 0xf800u) == 0x4800u) { // literal load
                    const unsigned dst = (op >> 8) & 7u;
                    const uint32_t address = ((pc + 4u) & ~3u) + ((op & 255u) << 2);
                    code << "    m->r[15] = 0x" << pc + 2 << "u; m->r[" << dst
                         << "] = m->read(m->board, 0x" << address
                         << "u, 4); m0_native_finish(m, 2); return 1;\n";
                } else if ((op & 0xe000u) == 0x6000u || (op & 0xf000u) == 0x8000u) {
                    const unsigned dst = op & 7u, src = (op >> 3) & 7u;
                    const unsigned width = (op & 0xf000u) == 0x8000u ? 2u : (op & 0x1000u ? 1u : 4u);
                    const unsigned offset = ((op >> 6) & 31u) * width;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; ";
                    if (op & 0x0800u) code << "m->r[" << dst << "] = m->read(m->board, m->r["
                                           << src << "] + 0x" << offset << "u, " << width << "); ";
                    else code << "m->write(m->board, m->r[" << src << "] + 0x" << offset
                              << "u, m->r[" << dst << "], " << width << "); ";
                    code << "m0_native_finish(m, 2); return 1;\n";
                } else if ((op & 0xf000u) == 0x9000u) { // SP-relative load/store
                    const unsigned dst = (op >> 8) & 7u, offset = (op & 255u) << 2;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; ";
                    if (op & 0x0800u) code << "m->r[" << dst << "] = m->read(m->board, m->r[13] + 0x"
                                           << offset << "u, 4); ";
                    else code << "m->write(m->board, m->r[13] + 0x" << offset << "u, m->r["
                              << dst << "], 4); ";
                    code << "m0_native_finish(m, 2); return 1;\n";
                } else if ((op & 0xf000u) == 0xd000u && ((op >> 8) & 15u) < 14u) {
                    const uint32_t target = pc + 4u + uint32_t(int32_t(int8_t(op)) * 2);
                    code << "    if (m0_native_cond(m, " << std::dec << ((op >> 8) & 15u) << std::hex << ")) { m->r[15] = 0x"
                         << target << "u; m0_native_finish(m, 2); } else { m->r[15] = 0x"
                         << pc + 2 << "u; m0_native_finish(m, 1); } return 1;\n";
                } else if ((op & 0xf800u) == 0xe000u) {
                    const int32_t offset = (int32_t)((op & 0x7ffu) << 21) >> 20;
                    code << "    m->r[15] = 0x" << pc + 4u + uint32_t(offset)
                         << "u; m0_native_finish(m, 2); return 1;\n";
                } else if (op == 0xbf00u) { // NOP
                    code << "    m->r[15] = 0x" << pc + 2
                         << "u; m0_native_finish(m, 1); return 1;\n";
                } else {
                    code << "    m0_cpu_execute(m, 0x" << pc << "u, 0x" << op
                         << "u, 0x" << op2 << "u); return 1;\n";
                }
            }
            code << "  default: break;\n  }\n  return 0;\n}\n";
            if (!code) { error = "Cannot write M0+ C translation"; return false; }
        }
        /* Keep each C translation unit bounded. A large ROM with a long
         * straight-line walk otherwise gives GCC one enormous switch and
         * minutes of compilation even for a 64 KiB synthetic image. */
        if (system != SystemId::meta) {
            std::ifstream generated(source / "m0_native.c");
            const std::string all(std::istreambuf_iterator<char>{generated}, {});
            std::map<uint32_t, std::string> pages;
            const size_t first = all.find("  case 0x");
            const size_t end = all.find("  default: break;", first);
            if (first == std::string::npos || end == std::string::npos) {
                error = "Cannot partition M0+ translation"; return false;
            }
            for (size_t at = first; at < end;) {
                const size_t next = all.find("  case 0x", at + 1);
                const size_t stop = next == std::string::npos || next > end ? end : next;
                const uint32_t pc = static_cast<uint32_t>(std::stoul(all.substr(at + 9), nullptr, 16));
                pages[pc & ~0xfffu] += all.substr(at, stop - at);
                at = stop;
            }
            for (const auto& entry : fs::directory_iterator(source)) {
                const auto name = entry.path().filename().string();
                if (name.starts_with("m0_native_page_") && entry.path().extension() == ".c")
                    fs::remove(entry.path(), ec);
            }
            std::ofstream dispatch(source / "m0_native.c");
            dispatch << "#include \"m0_cpu.h\"\n";
            for (const auto& [page, body] : pages) {
                std::ostringstream symbol;
                symbol << "m0_native_page_" << std::hex << page;
                std::ofstream unit(source / (symbol.str() + ".c"));
                unit << "#include \"m0_cpu.h\"\nint " << symbol.str()
                     << "(M0Cpu *m) {\n  switch (m->r[15]) {\n" << body
                     << "  default: break;\n  }\n  return 0;\n}\n";
                if (!unit) { error = "Cannot write M0+ native page"; return false; }
                dispatch << "int " << symbol.str() << "(M0Cpu *m);\n";
            }
            dispatch << "int m0_native_step(M0Cpu *m) {\n  if (m->fault) return 0;\n"
                        "  switch (m->r[15] & ~0xfffu) {\n";
            for (const auto& [page, body] : pages) {
                dispatch << "  case 0x" << std::hex << page << "u: return m0_native_page_"
                         << page << "(m);\n";
            }
            dispatch << "  default: return 0;\n  }\n}\n";
            if (!dispatch) { error = "Cannot write M0+ native dispatcher"; return false; }
        }
        {
            std::ofstream status(output / "native-status.txt");
            status << "compiled_flash_entries=" << compiled.size() << "\n"
                   << "board_native_entries=" << board_native << "\n"
                   << "dynamic_or_ram_entries=interpreter\n";
        }
        if (aka) {
            /* the AKA's PlatformIO project: the same sources, the ESP32 core and
             * the AKA's pins and panel driver beside them */
            const fs::path targets = runtime_dir.parent_path().parent_path();
            const fs::path platform = runtime_dir.parent_path() / "platform";
            const fs::path core = targets / "common/consoles/esp32", common = targets / "common/platform";
            const fs::path aka_dir = targets / "consoles/aka";
            auto copy = [&](const fs::path& from, const fs::path& to) {
                std::error_code copy_error;
                fs::create_directories(to.parent_path(), copy_error);
                fs::copy_file(from, to, fs::copy_options::overwrite_existing, copy_error);
                if (copy_error) { error = "Cannot copy " + from.string() + ": " + copy_error.message(); return false; }
                return true;
            };
            for (const fs::path& file : {core / "esp32_hw.cpp", core / "esp32_hw.h", core / "esp32_sd.cpp",
                                         aka_dir / "esp32_config.h", aka_dir / "aka_hw.cpp", aka_dir / "aka_hw.h",
                                         platform / "md_audio.h", targets / "common/runtime/m0_scale.h",
                                         targets / "meta/runtime/gbl_board.h"})
                if (!copy(file, source / file.filename())) return false;
            for (const char* name : {"m0_target.h", "m0_clock.h", "m0_lcd.h", "m0_menu.h", "m0_audio.h", "m0_sd.h"})
                if (!copy(common / name, source / name)) return false;
            if (!copy(aka_dir / "boards/gamebuino_aka.json", output / "boards/gamebuino_aka.json")) return false;
            std::ofstream(source / "gbl_config.h") << "#pragma once\n#define GBL_TARGET_POKITTO 0\n#define GBL_TARGET_AKA 1\n"
                                                      "#define GBL_GEN_VERSION \"0.2.9\"\n";
            std::ofstream(source / "gbl_profile.h") << "#pragma once\n#include \"gbl_board.h\"\n#define META_OVERCLOCK 0\n"
                                                       "#define META_AUDIO_DMA 0\n";
            std::ofstream(source / "md_config.h") << "#pragma once\n";
            for (const auto& entry : fs::directory_iterator(aka_dir)) {
                if (entry.path().extension() != ".in") continue;
                std::ifstream in(entry.path());
                std::string text((std::istreambuf_iterator<char>(in)), {});
                for (size_t at = text.find("@NAME@"); at != std::string::npos; at = text.find("@NAME@", at))
                    text.replace(at, 6, "m0game");
                if (entry.path().filename() == "platformio.ini.in") {
                    const std::string flags = "-Os -Wno-unused-label";
                    const size_t at = text.find(flags);
                    if (at != std::string::npos)
                        text.replace(at, flags.size(), "-O2 -Wno-unused-label -DM0_FLASH_BASE=0x4000u");
                }
                std::ofstream(output / entry.path().stem()) << text;
            }
        } else if (system == SystemId::meta) {
            std::ofstream cmake(output / "CMakeLists.txt");
            cmake << "cmake_minimum_required(VERSION 3.20)\nproject(m0_native C)\n"
                     "set(CMAKE_C_STANDARD 11)\n"
                     "file(GLOB M0_PAGES CONFIGURE_DEPENDS src/m0_native_page_*.c)\n"
                     "set_property(GLOBAL PROPERTY JOB_POOLS m0_compile=4)\n"
                     "add_library(m0_core STATIC src/m0_native.c ${M0_PAGES} src/m0_rom.c "
                     "src/m0_cpu.c src/m0_machine.c src/samd21.c)\n"
                     "target_include_directories(m0_core PUBLIC src)\n"
                     "target_compile_definitions(m0_core PUBLIC M0_FLASH_BASE=0x4000u)\n"
                     "target_compile_options(m0_core PRIVATE -O2)\n"
                     "set_property(TARGET m0_core PROPERTY JOB_POOL_COMPILE m0_compile)\n"
                     "add_executable(m0_headless src/m0_pc.c)\n"
                     "target_compile_definitions(m0_headless PRIVATE M0_NO_SDL)\n"
                     "target_link_libraries(m0_headless PRIVATE m0_core m)\n"
                     "find_package(SDL2 QUIET)\n"
                     "if(SDL2_FOUND)\n"
                     "  add_executable(m0_firmware_pc src/m0_pc.c)\n"
                     "  if(WIN32)\n    target_link_libraries(m0_firmware_pc PRIVATE SDL2::SDL2main)\n  endif()\n"
                     "  target_link_libraries(m0_firmware_pc PRIVATE m0_core SDL2::SDL2 m)\n"
                     "endif()\n";
            if (!cmake) { error = "Cannot write M0+ native build"; return false; }
        } else {
            std::ofstream cmake(output / "CMakeLists.txt");
            cmake << "cmake_minimum_required(VERSION 3.20)\nproject(m0_native C)\n"
                     "set(CMAKE_C_STANDARD 11)\n"
                     "file(GLOB M0_PAGES CONFIGURE_DEPENDS src/m0_native_page_*.c)\n"
                     "add_executable(m0_native_pc src/m0_native_host.c src/m0_native.c ${M0_PAGES} "
                     "src/m0_rom.c src/m0_cpu.c src/m0_machine.c)\n"
                     "set_property(GLOBAL PROPERTY JOB_POOLS m0_compile=4)\n"
                     "set_property(TARGET m0_native_pc PROPERTY JOB_POOL_COMPILE m0_compile)\n"
                     "target_compile_options(m0_native_pc PRIVATE -O2)\n"
                     "target_compile_definitions(m0_native_pc PRIVATE M0_FLASH_BASE="
                  << (system == SystemId::meta ? "0x4000u" : "0u") << " M0_RAM_BASE="
                  << (system == SystemId::meta ? "0x20000000u" : "0x10000000u") << ")\n"
                     "find_package(SDL2 QUIET)\nfind_package(ZLIB QUIET)\n"
                     "if(SDL2_FOUND AND ZLIB_FOUND)\n"
                     "  add_executable(m0_firmware_pc src/gm0.c src/aka.c src/m0_rom.c)\n"
                     "  target_compile_options(m0_firmware_pc PRIVATE -O2)\n"
                     "  target_compile_definitions(m0_firmware_pc PRIVATE GM0_NATIVE_TRANSLATION GM0_EMBEDDED_GAME)\n"
                     "  if(WIN32)\n"
                     "    target_link_libraries(m0_firmware_pc PRIVATE SDL2::SDL2main)\n"
                     "  endif()\n"
                     "  target_link_libraries(m0_firmware_pc PRIVATE SDL2::SDL2 ZLIB::ZLIB m)\n"
                     "  add_executable(m0_board_test EXCLUDE_FROM_ALL src/gm0.c src/aka.c)\n"
                     "  target_compile_options(m0_board_test PRIVATE -O2)\n"
                     "  target_compile_definitions(m0_board_test PRIVATE GM0_NATIVE_TRANSLATION EMU_NODE_HEADLESS)\n"
                     "  target_link_libraries(m0_board_test PRIVATE SDL2::SDL2 ZLIB::ZLIB m)\n"
                     "endif()\n";
            if (!cmake) { error = "Cannot write M0+ native build"; return false; }
        }
    }
    {
        std::ofstream project(marker);
        project << board << (aka ? "-aka\n" : "-pc\n");
        if (!project) { error = "Cannot write project marker"; return false; }
    }
    if (aka) {
        /* README.md came from the AKA template */
    } else if (system == SystemId::meta) {
        {
            std::ofstream launcher(output / "run.py");
            launcher << "#!/usr/bin/env python3\n"
                        "\"\"\"Run the recompiled firmware (window with SDL2, or headless with --frames).\"\"\"\n"
                        "import subprocess\nimport sys\nfrom pathlib import Path\n\n"
                        "root = Path(__file__).resolve().parent\n"
                        "for name in ('m0_firmware_pc', 'm0_headless'):\n"
                        "    exe = root / 'build' / name\n"
                        "    if exe.is_file():\n"
                        "        sys.exit(subprocess.call([str(exe), *sys.argv[1:]], cwd=root))\n"
                        "sys.exit('build this project first: cmake -G Ninja -S . -B build && ninja -C build')\n";
            if (!launcher) { error = "Cannot write PC launcher"; return false; }
        }
        std::ofstream readme(output / "README.md");
        readme << "# meta firmware on PC (recompiled, no emulator)\n\n"
                  "The Thumb code of `firmware.bin` is translated to C at generation "
                  "(`src/m0_native_page_*.c`, the same cycles and side effects as the "
                  "fallback interpreter `m0_cpu.c`); code the walk did not reach "
                  "(RAM code, unprofiled paths) runs in that interpreter. `samd21.c` is "
                  "the board: TC4/TC5, DMAC, SERCOM4 (ST7735 and pad), PORT, DAC, with "
                  "`m0_machine.c` for SysTick and the NVIC. Poll loops whose value only "
                  "an event changes (load, compare, branch back) are spent up to the next "
                  "event.\n\n"
                  "```sh\ncmake -G Ninja -S . -B build && ninja -C build\n"
                  "build/m0_firmware_pc                  # window (SDL2): arrows, J=A K=B U=menu I=home\n"
                  "build/m0_headless --frames 600 --ppm out.ppm --wav out.wav\n```\n\n"
                  "Headless runs print `run_hash`, `image_hash` and `audio_hash`; "
                  "`--interp` runs the interpreter only and must print the same line.\n\n"
                  "## Profile\n\n"
                  "```sh\nbuild/m0_headless --frames 3000 --random 1 --profile p1   # p1.cov p1.mmio p1.hot\n"
                  "m0recomp firmware.bin --system meta --target pc --coverage p1.cov -o <project>\n```\n\n"
                  "`.cov` is the executed code (what a regeneration translates), `.mmio` the "
                  "register accesses (`UNMODELLED-READ`: a register the board answers 0 for "
                  "without a model), `.hot` the instructions that spend the most cycles. "
                  "Not modelled yet: the SD card (it reads as absent), flash writes, "
                  "Pokitto.\n";
        if (!readme) { error = "Cannot write PC README"; return false; }
    } else {
    {
            std::ofstream launcher(output / "run.py");
            launcher << "#!/usr/bin/env python3\n"
                        "\"\"\"Run the embedded firmware in the integrated gm0 board emulator.\"\"\"\n"
                        "import os\nimport shutil\nimport subprocess\nimport sys\n"
                        "from pathlib import Path\n\n"
                        "root = Path(__file__).resolve().parent\n"
                        "local = root / 'build' / 'm0_firmware_pc'\n"
                        "gm0 = os.environ.get('GM0') or (str(local) if local.is_file() else shutil.which('gm0'))\n"
                        "if not gm0:\n"
                        "    sys.exit('build this project with CMake/Ninja (SDL2 and zlib required), or set GM0=/path/to/gm0')\n"
                        "args = [gm0]\n"
                        "if Path(gm0).resolve() != local.resolve():\n"
                        "    args.append(str(root / 'firmware.bin'))\n"
                        "sys.exit(subprocess.call([*args, "
                     << "'--target', '" << board << "', *sys.argv[1:]], cwd=root))\n";
            if (!launcher) { error = "Cannot write PC launcher"; return false; }
        }
        {
            std::ofstream readme(output / "README.md");
            readme << "# " << board << " firmware on PC\n\n"
                      "This project contains the supplied firmware image and an integrated "
                      "gm0 board emulator (MIT, with third-party notices in `GM0-LICENSE`). "
                      "Build it with CMake/Ninja, SDL2 and zlib, then run:\n\n"
                      "```sh\ncmake -G Ninja -S . -B build && ninja -C build\n"
                      "python3 run.py [card.img or card/] "
                      "[gm0 options]\n```\n\n"
                      "The PC executable embeds the firmware, and can run independently of "
                      "`firmware.bin` or `run.py`; pass `--target " << board << "` when "
                      "running it directly. gm0 supplies the Cortex-M0+ machine, " << board
                   << " peripherals, display, input and SD card. The raw `.bin` has no "
                      "card contents; pass an image or folder when the application needs one. "
                      "For Pokitto loader `.pop` files, extract the firmware `.bin` first.\n\n"
                      "The board emulator executes generated C for selected Thumb instructions "
                      "and interprets the rest. Set `M0_INTERP=1` to use the interpreter only. "
                      "The separate `m0_native_pc` program builds with CMake/Ninja and "
                      "compares fixed-address C dispatch with the Thumb fallback "
                      "(`--interp --steps N`). It models the CPU, SRAM and NVIC only; "
                      "a real firmware requiring board peripherals runs through `run.py`.\n";
            if (!readme) { error = "Cannot write PC README"; return false; }
        }
    }
    error.clear();
    return true;
}
}
