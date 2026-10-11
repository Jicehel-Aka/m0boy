// This target links only the CPU library: no GB cartridge parser or mapper.
#include "recompiler/cpu/sm83/decoder.h"
#include <array>
#include <iostream>

struct Reader : m0boy::ByteReader {
    uint8_t read_banked(m0boy::BankId bank, uint16_t address) const override {
        // LD A,n; CB BIT 7,A; JR NZ,-6. Distinct banks test read routing.
        constexpr std::array<uint8_t, 6> bytes{0x3e, 0x42, 0xcb, 0x7f, 0x20, 0xfa};
        return bank == 7 && address < bytes.size() ? bytes[address] : 0;
    }
};

int main() {
    Reader memory;
    gbrecomp::Decoder decoder(memory);
    auto load = decoder.decode(0, 7);
    auto bit = decoder.decode(2, 7);
    auto branch = decoder.decode(4, 7);
    auto other_bank = decoder.decode(0, 0);
    if (load.type != gbrecomp::InstructionType::LD_R_N || load.imm8 != 0x42 ||
        load.length != 2 || load.cycles != 8 ||
        bit.type != gbrecomp::InstructionType::BIT_N_R || bit.bit_index != 7 ||
        bit.reg8_dst != gbrecomp::Reg8::A ||
        branch.type != gbrecomp::InstructionType::JR_CC_N || branch.offset != -6 ||
        branch.cycles != 8 || branch.cycles_branch != 12 ||
        other_bank.type != gbrecomp::InstructionType::NOP) {
        std::cerr << "SM83 byte-reader decoding mismatch\n";
        return 1;
    }
}
