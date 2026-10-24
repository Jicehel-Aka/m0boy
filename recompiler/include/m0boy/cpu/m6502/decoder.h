#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "m0boy/memory.h"

namespace m0boy::cpu::m6502 {

/* The 6502's aaabbbcc encoding is its own: register codes and addressing
 * modes live in the opcode's low five bits (the 8080 family's x/y/z split
 * in m0boy/cpu/encoding.h does not describe it). */
enum class Addressing {
    implied,    /* INX, CLC */
    accumulator,/* ASL A */
    immediate,  /* LDA #nn */
    zero_page,  /* LDA nn */
    zero_page_x,
    zero_page_y,
    absolute,   /* LDA nnnn */
    absolute_x,
    absolute_y,
    indirect,   /* JMP (nnnn) */
    indirect_x, /* (nn,X) */
    indirect_y, /* (nn),Y */
    relative,   /* branches */
};

enum class Flow { next, branch, jump, call, ret, brk };

/* 6502 disassembly and flow metadata, after the Z80 decoder's model: no
 * execution, no IR.  `cycles` is the base cycle count, `cycles_page_cross`
 * the penalty an indexed read pays when it crosses a page (0 when none);
 * the NES's 2A03 runs this encoding without decimal mode, which changes
 * nothing at decode time. */
struct Instruction {
    uint16_t address;
    size_t length = 0;
    unsigned cycles = 0;
    unsigned cycles_page_cross = 0;
    std::string mnemonic;
    Addressing addressing = Addressing::implied;
    Flow flow = Flow::next;
    bool conditional = false;
    bool undocumented = false;
    std::optional<uint16_t> target;     /* jump/call absolute, branch base */
    std::optional<uint8_t> immediate;   /* immediate or relative operand */
    bool reads_memory = false;
    bool writes_memory = false;
};

class Decoder {
public:
    explicit Decoder(const ByteReader& memory) : memory_(memory) {}
    Instruction decode(uint16_t address, BankId bank = 0) const;
private:
    const ByteReader& memory_;
};

} // namespace m0boy::cpu::m6502
