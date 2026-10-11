#pragma once
#include "m0boy/memory.h"
#include <array>
#include <cstddef>

namespace m0boy::cpu {
// Register codes shared by 8080-derived encodings. Index-prefix substitution
// is a Z80 rule; flag layout and instruction semantics are CPU-specific.
enum class Reg8 : uint8_t { B, C, D, E, H, L, HL_IND, A };
inline constexpr std::array registers8 = {
    Reg8::B, Reg8::C, Reg8::D, Reg8::E,
    Reg8::H, Reg8::L, Reg8::HL_IND, Reg8::A,
};
inline constexpr std::array<const char*, 8> register_names8 = {
    "B", "C", "D", "E", "H", "L", "(HL)", "A",
};

struct OpcodeFields {
    uint8_t x, y, z, p, q;
    constexpr explicit OpcodeFields(uint8_t opcode)
        : x(opcode >> 6), y((opcode >> 3) & 7), z(opcode & 7),
          p(y >> 1), q(y & 1) {}
};

inline uint16_t read_u16(const ByteReader& memory, BankId bank, uint16_t address) {
    const auto lo = memory.read_banked(bank, address);
    const auto hi = memory.read_banked(bank, static_cast<uint16_t>(address + 1));
    return static_cast<uint16_t>(lo | (hi << 8));
}

constexpr uint16_t relative_target(uint16_t address, size_t length, int8_t offset) {
    return static_cast<uint16_t>(address + static_cast<uint16_t>(length) + offset);
}
} // namespace m0boy::cpu
