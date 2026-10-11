#pragma once
#include "m0boy/cpu/encoding.h"
#include <optional>
#include <string>

namespace m0boy::cpu::z80 {
enum class Flow { next, jump, call, ret, halt, repeat };

// Z80 disassembly and flow metadata, deliberately separate from the SM83 IR.
// Timing counts T-states without wait states. No execution is implemented.
struct Instruction {
    uint16_t address;
    size_t length = 0;
    unsigned cycles = 0;
    unsigned cycles_taken = 0;
    std::string mnemonic;
    Flow flow = Flow::next;
    bool conditional = false;
    bool undocumented = false;
    std::optional<uint16_t> target;
};

class Decoder {
public:
    explicit Decoder(const ByteReader& memory) : memory_(memory) {}
    Instruction decode(uint16_t address, BankId bank = 0) const;
private:
    const ByteReader& memory_;
};
} // namespace m0boy::cpu::z80
