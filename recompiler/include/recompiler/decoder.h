#pragma once
// Compatibility include for the SM83 decoder and Game Boy bank sweep.
#include "recompiler/rom.h"
#include "recompiler/cpu/sm83/decoder.h"

namespace gbrecomp {
std::vector<Instruction> decode_bank(const ROM& rom, BankId bank = 0);
}
