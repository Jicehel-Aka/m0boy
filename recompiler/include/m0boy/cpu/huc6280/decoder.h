#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "m0boy/cpu/m6502/decoder.h"
#include "m0boy/memory.h"

namespace m0boy::cpu::huc6280 {

/* The HuC6280 keeps the 6502's documented set and replaces most illegal
 * opcodes with its own: the memory manager (TAM/TMA), the block moves
 * (TII/TIN/TIA/TAI/TDD, six bytes each), the bit branches (BBR/BBS), BSR
 * and the SET flag writes.  This decoder reuses the 6502 table (a 6502
 * program is a HuC6280 program) and adds those, from the public opcode
 * charts — they still need a hardware cross-check before an emitter
 * trusts their timing, which the flag `extended` keeps visible. */
struct Instruction : m6502::Instruction {
    bool extended = false; /* a HuC6280 opcode the 6502 does not have */
};

class Decoder {
public:
    explicit Decoder(const ByteReader& memory) : memory_(memory) {}
    Instruction decode(uint16_t address, BankId bank = 0) const;
private:
    const ByteReader& memory_;
};

} // namespace m0boy::cpu::huc6280
