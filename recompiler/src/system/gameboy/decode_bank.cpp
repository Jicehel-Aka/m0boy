#include "recompiler/decoder.h"
#include <algorithm>

namespace gbrecomp {

std::vector<Instruction> decode_bank(const ROM& rom, BankId bank) {
    std::vector<Instruction> instructions;
    
    // Determine bank boundaries
    const size_t BANK_SIZE = 0x4000;  // 16KB
    size_t start_offset, end_offset;
    
    if (bank == 0) {
        start_offset = 0;
        end_offset = std::min(BANK_SIZE, rom.size());
    } else {
        start_offset = bank * BANK_SIZE;
        end_offset = std::min(start_offset + BANK_SIZE, rom.size());
    }
    
    if (start_offset >= rom.size()) {
        return instructions;
    }
    
    Decoder decoder(rom);
    uint16_t addr = (bank == 0) ? 0x0000 : 0x4000;
    size_t offset = start_offset;
    
    while (offset < end_offset) {
        Instruction instr = decoder.decode(addr, bank);
        instructions.push_back(instr);
        
        offset += instr.length;
        addr += instr.length;
    }
    
    return instructions;
}


} // namespace gbrecomp
