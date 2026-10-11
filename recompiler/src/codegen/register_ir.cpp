#include "recompiler/codegen/register_ir.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace gbrecomp {
namespace {
const char* names[] = {"b", "c", "d", "e", "h", "l", nullptr, "a"};
constexpr int offsets[] = {1, 0, 3, 2, 5, 4, -1, 10};
struct Store {
    int destination, width, source, constant;
};
}

RegisterIR::RegisterIR() {
    for (int r = 0; r < 8; ++r) values_[r] = {r, 0};
}

bool RegisterIR::append(uint8_t op, uint8_t n8, uint16_t n16) {
    if (op == 0x00) return true;
    if (op >= 0x40 && op < 0x80 && (op & 7) != 6 && ((op >> 3) & 7) != 6) {
        values_[(op >> 3) & 7] = values_[op & 7];
        return true;
    }
    if (op <= 0x3E && (op & 7) == 6 && ((op >> 3) & 7) != 6) {
        values_[(op >> 3) & 7] = {-1, n8};
        return true;
    }
    if (op == 0x01 || op == 0x11 || op == 0x21) {
        const unsigned r = (op >> 4) * 2;
        values_[r] = {-1, static_cast<uint8_t>(n16 >> 8)};
        values_[r + 1] = {-1, static_cast<uint8_t>(n16)};
        return true;
    }
    return false;
}

std::string RegisterIR::emit() const {
    std::vector<Store> stores;
    for (int r = 0; r < 8; ++r) {
        if (r == 6) continue;
        const auto v = values_[r];
        if (v.source == r) continue;
        if (r < 6 && !(r & 1)) {
            const auto lo = values_[r + 1];
            if (v.source == -1 && lo.source == -1) {
                stores.push_back({offsets[r + 1], 2, -1, (v.constant << 8) | lo.constant});
                ++r;
                continue;
            }
            if (v.source >= 0 && v.source < 6 && !(v.source & 1) && lo.source == v.source + 1) {
                stores.push_back({offsets[r + 1], 2, offsets[lo.source], 0});
                ++r;
                continue;
            }
        }
        stores.push_back({offsets[r], 1, v.source < 0 ? -1 : offsets[v.source], v.constant});
    }
    // Snapshot every source before storing: permutations and copies of an old
    // value must survive writes to their original register.
    std::vector<std::pair<int, int>> sources;
    for (const auto& s : stores) {
        const auto src = std::make_pair(s.source, s.width);
        if (s.source >= 0 && std::find(sources.begin(), sources.end(), src) == sources.end())
            sources.push_back(src);
    }
    std::ostringstream o;
    if (stores.empty()) return "";
    o << "    { /* register IR: entry values -> final values */\n";
    // Four scratch registers leave low registers available for the base even
    // when GCC reserves r7 as a frame pointer. Larger plans use the C lowering.
    if (sources.size() <= 3) {
        o << "#if defined(__ARM_ARCH_6M__)\n"
          << "        __asm__ volatile(\n"
          << "            \".syntax unified\\n\\t\"\n";
        for (size_t i = 0; i < sources.size(); ++i)
            o << "            \"ldr" << (sources[i].second == 2 ? "h" : "b") << " r" << i
              << ", [%0, #" << sources[i].first << "]\\n\\t\"\n";
        int last_constant = -1;
        for (const auto& s : stores) {
            std::string reg;
            if (s.source >= 0) {
                const auto it = std::find(sources.begin(), sources.end(), std::make_pair(s.source, s.width));
                reg = "r" + std::to_string(it - sources.begin());
            } else {
                reg = "r3";
                if (s.constant != last_constant) {
                    if (s.constant > 255) {
                        o << "            \"movs r3, #" << (s.constant >> 8) << "\\n\\t\"\n"
                          << "            \"lsls r3, r3, #8\\n\\t\"\n";
                        if (s.constant & 255)
                            o << "            \"adds r3, #" << (s.constant & 255) << "\\n\\t\"\n";
                    } else o << "            \"movs r3, #" << s.constant << "\\n\\t\"\n";
                    last_constant = s.constant;
                }
            }
            o << "            \"str" << (s.width == 2 ? "h" : "b") << " " << reg
              << ", [%0, #" << s.destination << "]\\n\\t\"\n";
        }
        o << "#if !defined(__clang__)\n            \".syntax divided\\n\\t\"\n#endif\n";
        o << "            : : \"l\" (&gbr) : ";
        for (size_t i = 0; i < sources.size(); ++i) o << "\"r" << i << "\", ";
        if (last_constant >= 0) o << "\"r3\", \"cc\", ";
        o << "\"memory\");\n#else\n";
    }
    for (int r = 0; r < 8; ++r) {
        bool used = false;
        for (int d = 0; d < 8; ++d)
            if (d != 6 && values_[d].source == r && d != r) used = true;
        if (used) o << "        const uint8_t v" << r << " = gbr." << names[r] << ";\n";
    }
    for (int r = 0; r < 8; ++r) {
        if (r == 6 || values_[r].source == r) continue;
        o << "        gbr." << names[r] << " = ";
        if (values_[r].source < 0) o << unsigned(values_[r].constant);
        else o << "v" << values_[r].source;
        o << ";\n";
    }
    if (sources.size() <= 3) o << "#endif\n";
    o << "    }\n";
    return o.str();
}
} // namespace gbrecomp
