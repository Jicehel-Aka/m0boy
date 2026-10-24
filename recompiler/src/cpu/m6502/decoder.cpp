#include "m0boy/cpu/m6502/decoder.h"

#include <cstdio>

namespace m0boy::cpu::m6502 {
namespace {

/* Every documented opcode, explicitly: name, addressing mode, base cycles,
 * page-cross penalty.  The table is the truth; the pattern in the opcode's
 * aaabbbcc bits explains it but its holes (the illegal opcodes) argue for
 * spelling it out.  Modes: IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND,
 * IZX, IZY, REL. */
enum M : uint8_t { IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL };
struct Entry {
    const char* mnemonic;
    uint8_t mode;
    uint8_t cycles;
    uint8_t cross;
};

/* The operand length and addressing of each mode. */
const Addressing decode_mode[] = {
    Addressing::implied,  Addressing::accumulator, Addressing::immediate,  Addressing::zero_page,
    Addressing::zero_page_x, Addressing::zero_page_y, Addressing::absolute, Addressing::absolute_x,
    Addressing::absolute_y, Addressing::indirect,  Addressing::indirect_x, Addressing::indirect_y,
    Addressing::relative,
};
const uint8_t mode_length[] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 2, 2, 2};

#define IMP1(name) {#name, IMP, 2, 0}
#define ENTRY(a, b, c, d) {a, b, c, d}
const Entry table[256] = {
    /* 0x00 */ {"BRK", IMP, 7, 0}, {"ORA", IZX, 6, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0x04 */ {0,0,0,0}, {"ORA", ZP, 3, 0}, {"ASL", ZP, 5, 0}, {0,0,0,0},
    /* 0x08 */ {"PHP", IMP, 3, 0}, {"ORA", IMM, 2, 0}, {"ASL", ACC, 2, 0}, {0,0,0,0},
    /* 0x0c */ {0,0,0,0}, {"ORA", ABS, 4, 0}, {"ASL", ABS, 6, 0}, {0,0,0,0},
    /* 0x10 */ {"BPL", REL, 2, 1}, {"ORA", IZY, 5, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0x14 */ {0,0,0,0}, {"ORA", ZPX, 4, 0}, {"ASL", ZPX, 6, 0}, {0,0,0,0},
    /* 0x18 */ {"CLC", IMP, 2, 0}, {"ORA", ABY, 4, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0x1c */ {0,0,0,0}, {"ORA", ABX, 4, 1}, {"ASL", ABX, 7, 0}, {0,0,0,0},
    /* 0x20 */ {"JSR", ABS, 6, 0}, {"AND", IZX, 6, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0x24 */ {"BIT", ZP, 3, 0}, {"AND", ZP, 3, 0}, {"ROL", ZP, 5, 0}, {0,0,0,0},
    /* 0x28 */ {"PLP", IMP, 4, 0}, {"AND", IMM, 2, 0}, {"ROL", ACC, 2, 0}, {0,0,0,0},
    /* 0x2c */ {"BIT", ABS, 4, 0}, {"AND", ABS, 4, 0}, {"ROL", ABS, 6, 0}, {0,0,0,0},
    /* 0x30 */ {"BMI", REL, 2, 1}, {"AND", IZY, 5, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0x34 */ {0,0,0,0}, {"AND", ZPX, 4, 0}, {"ROL", ZPX, 6, 0}, {0,0,0,0},
    /* 0x38 */ {"SEC", IMP, 2, 0}, {"AND", ABY, 4, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0x3c */ {0,0,0,0}, {"AND", ABX, 4, 1}, {"ROL", ABX, 7, 0}, {0,0,0,0},
    /* 0x40 */ {"RTI", IMP, 6, 0}, {"EOR", IZX, 6, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0x44 */ {0,0,0,0}, {"EOR", ZP, 3, 0}, {"LSR", ZP, 5, 0}, {0,0,0,0},
    /* 0x48 */ {"PHA", IMP, 3, 0}, {"EOR", IMM, 2, 0}, {"LSR", ACC, 2, 0}, {0,0,0,0},
    /* 0x4c */ {"JMP", ABS, 3, 0}, {"EOR", ABS, 4, 0}, {"LSR", ABS, 6, 0}, {0,0,0,0},
    /* 0x50 */ {"BVC", REL, 2, 1}, {"EOR", IZY, 5, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0x54 */ {0,0,0,0}, {"EOR", ZPX, 4, 0}, {"LSR", ZPX, 6, 0}, {0,0,0,0},
    /* 0x58 */ {"CLI", IMP, 2, 0}, {"EOR", ABY, 4, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0x5c */ {0,0,0,0}, {"EOR", ABX, 4, 1}, {"LSR", ABX, 7, 0}, {0,0,0,0},
    /* 0x60 */ {"RTS", IMP, 6, 0}, {"ADC", IZX, 6, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0x64 */ {0,0,0,0}, {"ADC", ZP, 3, 0}, {"ROR", ZP, 5, 0}, {0,0,0,0},
    /* 0x68 */ {"PLA", IMP, 4, 0}, {"ADC", IMM, 2, 0}, {"ROR", ACC, 2, 0}, {0,0,0,0},
    /* 0x6c */ {"JMP", IND, 5, 0}, {"ADC", ABS, 4, 0}, {"ROR", ABS, 6, 0}, {0,0,0,0},
    /* 0x70 */ {"BVS", REL, 2, 1}, {"ADC", IZY, 5, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0x74 */ {0,0,0,0}, {"ADC", ZPX, 4, 0}, {"ROR", ZPX, 6, 0}, {0,0,0,0},
    /* 0x78 */ {"SEI", IMP, 2, 0}, {"ADC", ABY, 4, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0x7c */ {0,0,0,0}, {"ADC", ABX, 4, 1}, {"ROR", ABX, 7, 0}, {0,0,0,0},
    /* 0x80 */ {0,0,0,0}, {"STA", IZX, 6, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0x84 */ {"STY", ZP, 3, 0}, {"STA", ZP, 3, 0}, {"STX", ZP, 3, 0}, {0,0,0,0},
    /* 0x88 */ {"DEY", IMP, 2, 0}, {0,0,0,0}, {"TXA", IMP, 2, 0}, {0,0,0,0},
    /* 0x8c */ {"STY", ABS, 4, 0}, {"STA", ABS, 4, 0}, {"STX", ABS, 4, 0}, {0,0,0,0},
    /* 0x90 */ {"BCC", REL, 2, 1}, {"STA", IZY, 6, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0x94 */ {"STY", ZPX, 4, 0}, {"STA", ZPX, 4, 0}, {"STX", ZPY, 4, 0}, {0,0,0,0},
    /* 0x98 */ {"TYA", IMP, 2, 0}, {"STA", ABY, 5, 0}, {"TXS", IMP, 2, 0}, {0,0,0,0},
    /* 0x9c */ {0,0,0,0}, {"STA", ABX, 5, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0xa0 */ {"LDY", IMM, 2, 0}, {"LDA", IZX, 6, 0}, {"LDX", IMM, 2, 0}, {0,0,0,0},
    /* 0xa4 */ {"LDY", ZP, 3, 0}, {"LDA", ZP, 3, 0}, {"LDX", ZP, 3, 0}, {0,0,0,0},
    /* 0xa8 */ {"TAY", IMP, 2, 0}, {"LDA", IMM, 2, 0}, {"TAX", IMP, 2, 0}, {0,0,0,0},
    /* 0xac */ {"LDY", ABS, 4, 0}, {"LDA", ABS, 4, 0}, {"LDX", ABS, 4, 0}, {0,0,0,0},
    /* 0xb0 */ {"BCS", REL, 2, 1}, {"LDA", IZY, 5, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0xb4 */ {"LDY", ZPX, 4, 0}, {"LDA", ZPX, 4, 0}, {"LDX", ZPY, 4, 0}, {0,0,0,0},
    /* 0xb8 */ {"CLV", IMP, 2, 0}, {"LDA", ABY, 4, 1}, {"TSX", IMP, 2, 0}, {0,0,0,0},
    /* 0xbc */ {"LDY", ABX, 4, 1}, {"LDA", ABX, 4, 1}, {"LDX", ABY, 4, 1}, {0,0,0,0},
    /* 0xc0 */ {"CPY", IMM, 2, 0}, {"CMP", IZX, 6, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0xc4 */ {"CPY", ZP, 3, 0}, {"CMP", ZP, 3, 0}, {"DEC", ZP, 5, 0}, {0,0,0,0},
    /* 0xc8 */ {"INY", IMP, 2, 0}, {"CMP", IMM, 2, 0}, {"DEX", IMP, 2, 0}, {0,0,0,0},
    /* 0xcc */ {"CPY", ABS, 4, 0}, {"CMP", ABS, 4, 0}, {"DEC", ABS, 6, 0}, {0,0,0,0},
    /* 0xd0 */ {"BNE", REL, 2, 1}, {"CMP", IZY, 5, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0xd4 */ {0,0,0,0}, {"CMP", ZPX, 4, 0}, {"DEC", ZPX, 6, 0}, {0,0,0,0},
    /* 0xd8 */ {"CLD", IMP, 2, 0}, {"CMP", ABY, 4, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0xdc */ {0,0,0,0}, {"CMP", ABX, 4, 1}, {"DEC", ABX, 7, 0}, {0,0,0,0},
    /* 0xe0 */ {"CPX", IMM, 2, 0}, {"SBC", IZX, 6, 0}, {0,0,0,0}, {0,0,0,0},
    /* 0xe4 */ {"CPX", ZP, 3, 0}, {"SBC", ZP, 3, 0}, {"INC", ZP, 5, 0}, {0,0,0,0},
    /* 0xe8 */ {"INX", IMP, 2, 0}, {"SBC", IMM, 2, 0}, {"NOP", IMP, 2, 0}, {0,0,0,0},
    /* 0xec */ {"CPX", ABS, 4, 0}, {"SBC", ABS, 4, 0}, {"INC", ABS, 6, 0}, {0,0,0,0},
    /* 0xf0 */ {"BEQ", REL, 2, 1}, {"SBC", IZY, 5, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0xf4 */ {0,0,0,0}, {"SBC", ZPX, 4, 0}, {"INC", ZPX, 6, 0}, {0,0,0,0},
    /* 0xf8 */ {"SED", IMP, 2, 0}, {"SBC", ABY, 4, 1}, {0,0,0,0}, {0,0,0,0},
    /* 0xfc */ {0,0,0,0}, {"SBC", ABX, 4, 1}, {"INC", ABX, 7, 0}, {0,0,0,0},
};
#undef ENTRY
#undef IMP1

} // namespace

Instruction Decoder::decode(uint16_t address, BankId bank) const {
    Instruction ins;
    ins.address = address;
    const uint8_t opcode = memory_.read_banked(bank, address);
    const Entry& entry = table[opcode];
    if (!entry.mnemonic) {
        /* an illegal opcode: the socle names none of them yet; the length
         * guess (1, or 2 for the illegal-NOP holes of cc=01/cc=10) keeps a
         * linear sweep aligned. */
        ins.undocumented = true;
        char buf[8];
        std::snprintf(buf, sizeof buf, "*%02X", opcode);
        ins.mnemonic = buf;
        /* the multi-byte illegal NOPs are the only ones a linear sweep has
         * to step over; every other hole is a jam or a one-byte illusion */
        switch (opcode) {
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2:   /* NOP #nn */
        case 0x04: case 0x44: case 0x64:                          /* NOP nn */
        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: /* NOP nn,X */
            ins.length = 2; break;
        case 0x0C:                                                /* NOP nnnn */
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: /* NOP nnnn,X */
            ins.length = 3; break;
        default: ins.length = 1; break;
        }
        ins.cycles = 2;
        ins.addressing = Addressing::implied;
        return ins;
    }
    ins.mnemonic = entry.mnemonic;
    ins.addressing = decode_mode[entry.mode];
    ins.length = mode_length[entry.mode];
    ins.cycles = entry.cycles;
    ins.cycles_page_cross = entry.cross;
    for (unsigned i = 1; i < ins.length; i++)
        if (i == 1) ins.immediate = memory_.read_banked(bank, uint16_t(address + 1));
    if (ins.addressing == Addressing::absolute || ins.addressing == Addressing::absolute_x ||
        ins.addressing == Addressing::absolute_y || ins.addressing == Addressing::indirect) {
        const uint16_t lo = memory_.read_banked(bank, uint16_t(address + 1));
        const uint16_t hi = memory_.read_banked(bank, uint16_t(address + 2));
        ins.target = uint16_t(lo | (hi << 8));
    } else if (ins.addressing == Addressing::relative && ins.immediate) {
        ins.target = uint16_t(address + ins.length + int8_t(*ins.immediate));
    }
    /* flow */
    if (ins.addressing == Addressing::relative) {
        ins.flow = Flow::branch;
        ins.conditional = true;
    } else if (ins.mnemonic == "JSR") {
        ins.flow = Flow::call;
    } else if (ins.mnemonic == "RTS" || ins.mnemonic == "RTI") {
        ins.flow = Flow::ret;
    } else if (ins.mnemonic == "JMP") {
        ins.flow = Flow::jump;
    } else if (ins.mnemonic == "BRK") {
        ins.flow = Flow::brk;
    }
    /* memory */
    const bool reads_or_writes = ins.addressing != Addressing::implied &&
                                 ins.addressing != Addressing::immediate &&
                                 ins.addressing != Addressing::accumulator &&
                                 ins.addressing != Addressing::relative;
    const char stores[] = "ST";
    ins.writes_memory = reads_or_writes &&
                        (ins.mnemonic[0] == stores[0] && ins.mnemonic[1] == stores[1]);
    ins.reads_memory = reads_or_writes && !ins.writes_memory;
    return ins;
}

} // namespace m0boy::cpu::m6502
