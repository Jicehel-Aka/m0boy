#include "recompiler/codegen/register_ir.h"

#include <array>
#include <cstdio>
#include <random>
#include <fstream>
#include <cstring>

using gbrecomp::RegisterIR;

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--emit-native") == 0) {
        std::ofstream out(argv[2]);
        out << "#include \"gbl.h\"\ngbl_cpu_t gbr;\n";
        const char* names[] = {"b", "c", "d", "e", "h", "l", nullptr, "a"};
        std::mt19937 random(0x600D);
        for (unsigned i = 0; i < 256; ++i) {
            RegisterIR ir;
            std::string reference;
            for (unsigned j = 0, n = 2 + random() % 20; j < n; ++j) {
                unsigned d = random() % 8, src = random() % 8;
                if (d == 6) d = 7;
                if (src == 6) src = 7;
                if (i == 0) { // Three LDs -> one constant materialisation + pair store.
                    d = j == 1 ? 1 : 0;
                    ir.append(0x06 + 8 * d, 0);
                    reference += std::string("gbr.") + names[d] + " = 0;\n";
                } else if (i == 1) { // Copy HL to BC, then remove redundant self copies.
                    d = j == 0 ? 0 : 1;
                    src = j == 0 ? 4 : j == 1 ? 5 : 1;
                    ir.append(0x40 + 8 * d + src);
                    reference += std::string("gbr.") + names[d] + " = gbr." + names[src] + ";\n";
                } else if (i < 32 || random() % 3 == 0) {
                    ir.append(0x40 + 8 * d + src);
                    reference += std::string("gbr.") + names[d] + " = gbr." + names[src] + ";\n";
                } else {
                    const unsigned value = random() % 256;
                    ir.append(0x06 + 8 * d, value);
                    reference += std::string("gbr.") + names[d] + " = " + std::to_string(value) + ";\n";
                }
            }
            out << "void native_case_" << i << "(void) {\n" << ir.emit() << "}\n";
            out << "void reference_case_" << i << "(void) {\n" << reference << "}\n";
        }
        return out ? 0 : 1;
    }
    // Independent byte-register execution oracle, including overwrites,
    // propagation of old values, pair immediates and self copies.
    std::mt19937 rng(0x1989);
    for (int trial = 0; trial < 20000; ++trial) {
        RegisterIR ir;
        std::array<uint8_t, 8> entry, expected;
        for (auto& v : entry) v = rng();
        expected = entry;
        for (unsigned j = 0, count = 1 + rng() % 40; j < count; ++j) {
            unsigned d = rng() % 8, s = rng() % 8;
            if (d == 6) d = 7;
            if (s == 6) s = 7;
            const uint8_t n = rng();
            const uint16_t nn = rng();
            bool ok;
            switch (rng() % 4) {
            case 0:
                ok = ir.append(0x40 + 8 * d + s);
                expected[d] = expected[s];
                break;
            case 1:
                ok = ir.append(0x06 + 8 * d, n);
                expected[d] = n;
                break;
            case 2:
                d = (rng() % 3) * 2;
                ok = ir.append(0x01 + 8 * d, 0, nn);
                expected[d] = nn >> 8;
                expected[d + 1] = nn;
                break;
            default: ok = ir.append(0x00); break;
            }
            if (!ok) return 1;
            for (int r = 0; r < 8; ++r) {
                const auto v = ir.values()[r];
                const auto actual = v.source < 0 ? v.constant : entry[v.source];
                if (actual != expected[r]) {
                    std::fprintf(stderr, "register mismatch trial=%d step=%u reg=%d\n", trial, j, r);
                    return 1;
                }
            }
        }
    }
    // Every unsupported opcode must reject without modifying the state.
    for (unsigned op = 0; op < 256; ++op) {
        RegisterIR ir;
        const auto before = ir.values();
        const bool supported = op == 0 || op == 1 || op == 0x11 || op == 0x21 ||
            (op >= 0x40 && op < 0x80 && (op & 7) != 6 && ((op >> 3) & 7) != 6) ||
            (op <= 0x3E && (op & 7) == 6 && ((op >> 3) & 7) != 6);
        if (ir.append(op, 42, 0x1234) != supported) return 1;
        if (!supported && ir.values() != before) return 1;
    }
    std::puts("register IR: 20,000 randomized sequences and opcode boundaries passed");
}
