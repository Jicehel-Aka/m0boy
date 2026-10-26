#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace m0boy::cpu::m0plus {
struct Instruction {
    uint8_t length;
    std::string mnemonic;
};

// Address is the firmware's flash address, not a file offset.  Unknown or
// reserved encodings are displayed as data, so a linear listing stays aligned.
Instruction decode(std::span<const uint8_t> bytes, uint32_t address);
} // namespace m0boy::cpu::m0plus
