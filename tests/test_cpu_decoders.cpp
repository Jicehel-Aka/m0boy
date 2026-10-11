#include "m0boy/cpu/z80/decoder.h"
#include "recompiler/cpu/sm83/decoder.h"
#include <iostream>
#include <stdexcept>
#include <vector>

struct Reader : m0boy::ByteReader {
    std::vector<uint8_t> bytes;
    uint8_t read_banked(m0boy::BankId bank, uint16_t address) const override {
        return bank == 0 && address < bytes.size() ? bytes[address] : 0;
    }
};

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    using namespace m0boy::cpu;
    try {
        Reader memory;
        z80::Decoder z(memory);
        gbrecomp::Decoder gb(memory);
        auto check = [&](std::vector<uint8_t> bytes, const char* mnemonic,
                         size_t length, unsigned cycles, unsigned taken = 0) {
            memory.bytes = std::move(bytes);
            const auto instruction = z.decode(0);
            if (instruction.mnemonic != mnemonic || instruction.length != length ||
                instruction.cycles != cycles || instruction.cycles_taken != (taken ? taken : cycles)) {
                std::cerr << mnemonic << " -> " << instruction.mnemonic << " length="
                          << instruction.length << " cycles=" << instruction.cycles << "/"
                          << instruction.cycles_taken << '\n';
                throw std::runtime_error("Z80 reference decoding mismatch");
            }
        };
        check({0x31, 0xff, 0xc3}, "LD SP,$C3FF", 3, 10);
        check({0x10, 0xfe}, "DJNZ $0000", 2, 8, 13);
        require(z.decode(0).conditional && z.decode(0).target == 0, "DJNZ flow incorrect");
        check({0x38, 0xfe}, "JR C,$0000", 2, 7, 12);
        check({0xea, 0x34, 0x12}, "JP PE,$1234", 3, 10);
        check({0xcd, 0x34, 0x12}, "CALL $1234", 3, 17);
        require(z.decode(0).flow == z80::Flow::call, "CALL flow missing");
        check({0xe0}, "RET PO", 1, 5, 11);
        check({0xd3, 0x7f}, "OUT ($7F),A", 2, 11);
        check({0xdb, 0xbe}, "IN A,($BE)", 2, 11);
        check({0x08}, "EX AF,AF'", 1, 4);
        check({0xd9}, "EXX", 1, 4);
        check({0xcb, 0x46}, "BIT 0,(HL)", 2, 12);
        check({0xcb, 0x36}, "SLL (HL)", 2, 15);
        require(gb.decode(0, 0).type == gbrecomp::InstructionType::SWAP_HL,
                "Z80 SLL semantics leaked into SM83 SWAP");
        check({0xed, 0x4b, 0x34, 0x12}, "LD BC,($1234)", 4, 20);
        check({0xed, 0x44}, "NEG", 2, 8);
        check({0xed, 0x4d}, "RETI", 2, 14);
        check({0xed, 0x56}, "IM 1", 2, 8);
        check({0xed, 0xb0}, "LDIR", 2, 16, 21);
        require(z.decode(0).flow == z80::Flow::repeat && z.decode(0).target == 0,
                "Block repeat flow missing");
        check({0xed, 0x00}, "NOP", 2, 8);
        require(z.decode(0).undocumented, "Undefined ED encoding not marked");
        check({0xdd, 0x21, 0x34, 0x12}, "LD IX,$1234", 4, 14);
        check({0xfd, 0x36, 0xfe, 0x42}, "LD (IY-$02),$42", 4, 19);
        check({0xdd, 0x66, 0x02}, "LD H,(IX+$02)", 3, 19);
        check({0xdd, 0x74, 0x02}, "LD (IX+$02),H", 3, 19);
        check({0xdd, 0x44}, "LD B,IXH", 2, 8);
        require(z.decode(0).undocumented, "IXH encoding not marked");
        check({0xdd, 0x76}, "HALT", 2, 8);
        check({0xdd, 0xeb}, "EX DE,HL", 2, 8);
        check({0xfd, 0xe9}, "JP (IY)", 2, 8);
        require(z.decode(0).flow == z80::Flow::jump && !z.decode(0).target,
                "Indirect jump given a static target");
        check({0xdd, 0xcb, 0xff, 0x46}, "BIT 0,(IX-$01)", 4, 20);
        check({0xfd, 0xcb, 0x01, 0xc0}, "SET 0,(IY+$01),B", 4, 23);
        check({0xdd, 0xfd, 0x21, 0x34, 0x12}, "LD IY,$1234", 5, 18);
        check({0xdd, 0xed, 0x6b, 0x34, 0x12}, "LD HL,($1234)", 5, 24);
        check({0xdd, 0xed, 0xb0}, "LDIR", 3, 20, 25);
        require(z.decode(0).target == 1, "Ignored prefix included in repeat target");
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            const OpcodeFields fields(opcode);
            require((fields.x << 6 | fields.y << 3 | fields.z) == opcode &&
                    fields.p == fields.y / 2 && fields.q == fields.y % 2,
                    "Shared opcode decomposition changed");
            memory.bytes = {0xcb, static_cast<uint8_t>(opcode)};
            const auto sm83 = gb.decode(0, 0);
            require(static_cast<unsigned>(sm83.reg8_dst) == fields.z &&
                    sm83.bit_index == fields.y, "SM83 CB operands differ from common encoding");
            for (const auto prefix : {0, 0xcb, 0xed, 0xdd, 0xfd}) {
                memory.bytes = prefix ? std::vector<uint8_t>{static_cast<uint8_t>(prefix), static_cast<uint8_t>(opcode), 0, 0, 0}
                                      : std::vector<uint8_t>{static_cast<uint8_t>(opcode), 0, 0, 0};
                const auto decoded = z.decode(0);
                require(!decoded.mnemonic.empty() && decoded.length <= 5 &&
                        decoded.cycles > 0 && decoded.cycles_taken >= decoded.cycles,
                        "Z80 opcode family has an invalid boundary or timing");
            }
        }
        for (unsigned code = 0; code < 8; ++code) {
            memory.bytes = {static_cast<uint8_t>(0x06 | code << 3), 0x42};
            auto sm83 = gb.decode(0, 0);
            auto zx = z.decode(0);
            require(sm83.disassemble() == zx.mnemonic && sm83.length == zx.length,
                    "Common LD r,n disassembly differs");
            if (code != 6) require(static_cast<unsigned>(sm83.reg8_dst) == code,
                                  "Register encoding differs");
            require(zx.cycles == (code == 6 ? 10u : 7u) &&
                    sm83.cycles == (code == 6 ? 12u : 8u), "CPU timings incorrectly shared");
        }
        require(relative_target(0xffff, 2, -2) == 0xffff &&
                relative_target(0xffff, 2, 0) == 1, "Relative target does not wrap");
        memory.bytes.assign(65536, 0xdd);
        bool refused = false;
        try { z.decode(0); } catch (const std::runtime_error&) { refused = true; }
        require(refused, "Prefix-only image loops forever");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
