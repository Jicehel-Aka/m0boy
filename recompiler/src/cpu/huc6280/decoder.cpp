#include "m0boy/cpu/huc6280/decoder.h"

#include <cstdio>
#include <string>

namespace m0boy::cpu::huc6280 {

/* The 6502 opcodes decode through the verified table; the HuC6280's own
 * live here, from the public opcode charts (they still want a hardware
 * cross-check before an emitter trusts their timing — hence `extended`). */
Instruction Decoder::decode(uint16_t address, BankId bank) const {
    const uint8_t opcode = memory_.read_banked(bank, address);
    Instruction ins;
    ins.address = address;

    switch (opcode) {
    case 0x53: /* TAM #$m: load the mapped pages from A */
    case 0x43: { /* TMA #$m: store the mapped pages to A */
        ins.mnemonic = opcode == 0x53 ? "TAM" : "TMA";
        ins.length = 2;
        ins.cycles = opcode == 0x53 ? 4 : 3;
        ins.immediate = memory_.read_banked(bank, uint16_t(address + 1));
        ins.extended = true;
        return ins;
    }
    case 0x99: { /* BSR label: a 16-bit relative call */
        const uint8_t lo = memory_.read_banked(bank, uint16_t(address + 1));
        const uint8_t hi = memory_.read_banked(bank, uint16_t(address + 2));
        ins.mnemonic = "BSR";
        ins.length = 3;
        ins.cycles = 8;
        ins.target = uint16_t(address + 3 + int16_t(uint16_t(lo | hi << 8)));
        ins.flow = m6502::Flow::call;
        ins.extended = true;
        return ins;
    }
    case 0xF4: { /* SET: the sub-opcode names the flag, then the value */
        char name[12];
        std::snprintf(name, sizeof name, "SET %02X", memory_.read_banked(bank, uint16_t(address + 1)));
        ins.mnemonic = name;
        ins.length = 3;
        ins.cycles = 4;
        ins.immediate = memory_.read_banked(bank, uint16_t(address + 2));
        ins.extended = true;
        return ins;
    }
    default: break;
    }
    if ((opcode & 0x0F) == 0x0F && opcode < 0xF0) {
        /* BBR0-7 (0x0F..0x7F) and BBS0-7 (0x8F..0xEF): zp,rel */
        char name[8];
        std::snprintf(name, sizeof name, "%s%u", opcode & 0x80 ? "BBS" : "BBR", (opcode >> 4) & 7);
        ins.mnemonic = name;
        ins.length = 3;
        ins.cycles = 6;
        ins.addressing = m6502::Addressing::relative;
        ins.target = uint16_t(address + 3 + int8_t(memory_.read_banked(bank, uint16_t(address + 2))));
        ins.flow = m6502::Flow::branch;
        ins.conditional = true;
        ins.extended = true;
        return ins;
    }
    if (opcode == 0x73 || opcode == 0x8B || opcode == 0xAB || opcode == 0xCB || opcode == 0xEB) {
        /* the block moves: source, destination and length, 16 bits each */
        static const char* const names[] = {"TII", "TAI", "TIA", "TIN", "TDD"};
        ins.mnemonic = opcode == 0x73 ? names[0] : opcode == 0x8B ? names[2]
                            : opcode == 0xAB ? names[3] : opcode == 0xCB ? names[1] : names[4];
        ins.length = 7;
        ins.cycles = 6; /* per byte, once the fixed part is paid */
        const uint16_t lo = memory_.read_banked(bank, uint16_t(address + 1));
        const uint16_t hi = memory_.read_banked(bank, uint16_t(address + 2));
        ins.target = uint16_t(lo | hi << 8);
        ins.reads_memory = ins.writes_memory = true;
        ins.extended = true;
        return ins;
    }

    /* an ordinary 6502 opcode: the shared table, through our own reader */
    m6502::Decoder base(memory_);
    static_cast<m6502::Instruction&>(ins) = base.decode(address, bank);
    return ins;
}

} // namespace m0boy::cpu::huc6280
