#include "m0boy/cpu/huc6280/decoder.h"

#include <cstdio>
#include <string>

namespace m0boy::cpu::huc6280 {

/* The 6502 opcodes decode through the shared table; the HuC6280's own live
 * here, cross-checked against the mednafen core's opcode table (huc6280's
 * huc6280_ops.inc): the cycle counts are the mednafen ones, the flag
 * `extended` keeps them visible for an emitter.  The 6502's illegal slots
 * that the HuC6280 leaves undefined (the default case of the core) decode
 * as one-byte two-cycle NOPs. */
Instruction Decoder::decode(uint16_t address, BankId bank) const {
    const uint8_t opcode = memory_.read_banked(bank, address);
    Instruction ins;
    ins.address = address;
    const auto at = [this, bank, address](uint16_t offset) {
        return memory_.read_banked(bank, uint16_t(address + offset));
    };

    switch (opcode) {
    /* the VDC's ports: ST0 select, ST1 data low, ST2 data high */
    case 0x03: ins.mnemonic = "ST0"; ins.length = 2; ins.cycles = 4;
        ins.addressing = m6502::Addressing::immediate; ins.immediate = at(1);
        ins.writes_memory = true; ins.extended = true; return ins;
    case 0x13: ins.mnemonic = "ST1"; ins.length = 2; ins.cycles = 4;
        ins.addressing = m6502::Addressing::immediate; ins.immediate = at(1);
        ins.writes_memory = true; ins.extended = true; return ins;
    case 0x23: ins.mnemonic = "ST2"; ins.length = 2; ins.cycles = 4;
        ins.addressing = m6502::Addressing::immediate; ins.immediate = at(1);
        ins.writes_memory = true; ins.extended = true; return ins;
    /* the MPR pages */
    case 0x53: ins.mnemonic = "TAM"; ins.length = 2; ins.cycles = 5;
        ins.addressing = m6502::Addressing::immediate; ins.immediate = at(1);
        ins.extended = true; return ins;
    case 0x43: ins.mnemonic = "TMA"; ins.length = 2; ins.cycles = 4;
        ins.addressing = m6502::Addressing::immediate; ins.immediate = at(1);
        ins.extended = true; return ins;
    /* the relative call */
    case 0x44: ins.mnemonic = "BSR"; ins.length = 2; ins.cycles = 8;
        ins.addressing = m6502::Addressing::relative;
        ins.target = uint16_t(address + 2 + int8_t(at(1)));
        ins.flow = m6502::Flow::call; ins.extended = true; return ins;
    /* SET: sets the T flag, one byte (the operand-less form the mednafen
     * core runs) */
    case 0xF4: ins.mnemonic = "SET"; ins.length = 1; ins.cycles = 2;
        ins.extended = true; return ins;
    /* the speed switches and the register clears and swaps */
    case 0x54: ins.mnemonic = "CSL"; ins.length = 1; ins.cycles = 3; ins.extended = true; return ins;
    case 0xD4: ins.mnemonic = "CSH"; ins.length = 1; ins.cycles = 3; ins.extended = true; return ins;
    case 0x62: ins.mnemonic = "CLA"; ins.length = 1; ins.cycles = 2; ins.extended = true; return ins;
    case 0x82: ins.mnemonic = "CLX"; ins.length = 1; ins.cycles = 2; ins.extended = true; return ins;
    case 0xC2: ins.mnemonic = "CLY"; ins.length = 1; ins.cycles = 2; ins.extended = true; return ins;
    case 0x02: ins.mnemonic = "SXY"; ins.length = 1; ins.cycles = 3; ins.extended = true; return ins;
    case 0x22: ins.mnemonic = "SAX"; ins.length = 1; ins.cycles = 3; ins.extended = true; return ins;
    case 0x42: ins.mnemonic = "SAY"; ins.length = 1; ins.cycles = 3; ins.extended = true; return ins;
    /* the block moves: source, destination and length, 16 bits each */
    case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3: {
        static const char* const names[5] = {"TII", "TDD", "TIN", "TIA", "TAI"};
        const int which = opcode == 0x73 ? 0 : opcode == 0xC3 ? 1 : opcode == 0xD3 ? 2
                              : opcode == 0xE3 ? 3 : 4;
        ins.mnemonic = names[which];
        ins.length = 7;
        ins.cycles = 17; /* the fixed part; six more a byte */
        ins.target = uint16_t(at(1) | at(2) << 8);
        ins.reads_memory = ins.writes_memory = true;
        ins.extended = true;
        return ins;
    }
    /* the 65C02 set the HuC6280 carries and the shared 6502 table does not
     * know: the (zp) addressing, STZ, TSB/TRB, BRA, the register pushes and
     * the accumulator inc/dec */
    case 0x72: case 0x32: case 0x52: case 0xD2: {
        static const char* const names[4] = {"ADC", "AND", "EOR", "SBC"};
        ins.mnemonic = names[opcode == 0x72 ? 0 : opcode == 0x32 ? 1 : opcode == 0x52 ? 2 : 3];
        ins.length = 2; ins.cycles = 7;
        ins.addressing = m6502::Addressing::indirect; /* (zp), the wrapless form */
        ins.extended = true; return ins;
    }
    case 0x64: case 0x74: case 0x9C: case 0x9E:
        ins.mnemonic = "STZ";
        ins.length = opcode >= 0x9C ? 3 : 2;
        ins.cycles = opcode >= 0x9C ? 5 : 4;
        ins.addressing = opcode == 0x64 || opcode == 0x9C ? m6502::Addressing::zero_page
                         : opcode == 0x74 ? m6502::Addressing::zero_page_x
                         : m6502::Addressing::absolute_x;
        ins.writes_memory = true; ins.extended = true; return ins;
    case 0x04: case 0x0C: ins.mnemonic = "TSB";
        ins.length = opcode == 0x04 ? 2 : 3; ins.cycles = opcode == 0x04 ? 6 : 7;
        ins.addressing = opcode == 0x04 ? m6502::Addressing::zero_page : m6502::Addressing::absolute;
        ins.reads_memory = ins.writes_memory = true; ins.extended = true; return ins;
    case 0x14: case 0x1C: ins.mnemonic = "TRB";
        ins.length = opcode == 0x14 ? 2 : 3; ins.cycles = opcode == 0x14 ? 6 : 7;
        ins.addressing = opcode == 0x14 ? m6502::Addressing::zero_page : m6502::Addressing::absolute;
        ins.reads_memory = ins.writes_memory = true; ins.extended = true; return ins;
    case 0x80: ins.mnemonic = "BRA"; ins.length = 2; ins.cycles = 4;
        ins.addressing = m6502::Addressing::relative;
        ins.target = uint16_t(address + 2 + int8_t(at(1)));
        ins.flow = m6502::Flow::branch; ins.extended = true; return ins;
    case 0xDA: ins.mnemonic = "PHX"; ins.length = 1; ins.cycles = 3; ins.extended = true; return ins;
    case 0x5A: ins.mnemonic = "PHY"; ins.length = 1; ins.cycles = 3; ins.extended = true; return ins;
    case 0xFA: ins.mnemonic = "PLX"; ins.length = 1; ins.cycles = 4; ins.extended = true; return ins;
    case 0x7A: ins.mnemonic = "PLY"; ins.length = 1; ins.cycles = 4; ins.extended = true; return ins;
    case 0x1A: ins.mnemonic = "INA"; ins.length = 1; ins.cycles = 2; ins.extended = true; return ins;
    case 0x3A: ins.mnemonic = "DEA"; ins.length = 1; ins.cycles = 2; ins.extended = true; return ins;
    case 0x89: ins.mnemonic = "BIT"; ins.length = 2; ins.cycles = 2;
        ins.addressing = m6502::Addressing::immediate; ins.immediate = at(1);
        ins.extended = true; return ins;
    case 0x83: case 0xA3: case 0x93: case 0xB3: {
        static const char* const forms[4] = {"TST #nn,zp", "TST #nn,zpx", "TST #nn,abs", "TST #nn,absx"};
        ins.mnemonic = forms[opcode == 0x83 ? 0 : opcode == 0xA3 ? 1 : opcode == 0x93 ? 2 : 3];
        ins.length = opcode <= 0xA3 ? 3 : 4;
        ins.cycles = opcode <= 0xA3 ? 7 : 8;
        ins.reads_memory = true; ins.extended = true; return ins;
    }
    default: break;
    }
    if (opcode >= 0x07 && opcode <= 0xF7 && (opcode & 0x0F) == 0x07) {
        /* RMB0-7 ($07..$77) and SMB0-7 ($87..$F7): zp */
        char name[8];
        std::snprintf(name, sizeof name, "%s%u", opcode & 0x80 ? "SMB" : "RMB", (opcode >> 4) & 7);
        ins.mnemonic = name;
        ins.length = 2;
        ins.cycles = 7;
        ins.addressing = m6502::Addressing::zero_page;
        ins.reads_memory = ins.writes_memory = true;
        ins.extended = true;
        return ins;
    }
    if ((opcode & 0x0F) == 0x0F && opcode < 0xF0) {
        /* BBR0-7 (0x0F..0x7F) and BBS0-7 (0x8F..0xEF): zp,rel */
        char name[8];
        std::snprintf(name, sizeof name, "%s%u", opcode & 0x80 ? "BBS" : "BBR", (opcode >> 4) & 7);
        ins.mnemonic = name;
        ins.length = 3;
        ins.cycles = 6; /* 8 taken */
        ins.addressing = m6502::Addressing::relative;
        ins.target = uint16_t(address + 3 + int8_t(at(2)));
        ins.flow = m6502::Flow::branch;
        ins.conditional = true;
        ins.extended = true;
        return ins;
    }

    /* an ordinary 6502 opcode: the shared table, through our own reader */
    m6502::Decoder base(memory_);
    static_cast<m6502::Instruction&>(ins) = base.decode(address, bank);
    /* the HuC6280's cycle counts run above the 6502's on most memory
     * instructions and pay no page-cross penalty (the mednafen core's
     * counts); the emitter wants those, the disassembly keeps both */
    static const uint8_t cycle_bump[13] = {
        /* implied 0, accumulator 0, immediate 0, zp +1, zpx 0, zpy 0,
         * abs +1, absx +1, absy +1, (ind) +2, (nn,X) +1, (nn),Y +2,
         * relative 0 */
        0, 0, 0, 1, 0, 0, 1, 1, 1, 2, 1, 2, 0,
    };
    const int bump = cycle_bump[int(ins.addressing)];
    ins.cycles += bump;
    ins.cycles_page_cross = 0;
    return ins;
}

} // namespace m0boy::cpu::huc6280
