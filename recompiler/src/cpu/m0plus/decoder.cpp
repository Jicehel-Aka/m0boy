#include "m0boy/cpu/m0plus/decoder.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace m0boy::cpu::m0plus {
namespace {
std::string hex(uint32_t value, int width = 0) {
    std::ostringstream out;
    out << "$" << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << value;
    return out.str();
}
std::string reg(unsigned n) {
    return n == 13 ? "sp" : n == 14 ? "lr" : n == 15 ? "pc" : "r" + std::to_string(n);
}
std::string regs(uint16_t bits) {
    std::string out = "{";
    for (unsigned r = 0; r < 16; ++r) if (bits & (1u << r)) {
        if (out.size() > 1) out += ", ";
        out += reg(r);
    }
    return out + "}";
}
int32_t sign_extend(uint32_t value, unsigned width) {
    return int32_t(value << (32 - width)) >> (32 - width);
}
} // namespace

Instruction decode(std::span<const uint8_t> bytes, uint32_t address) {
    if (bytes.size() < 2) throw std::runtime_error("truncated Thumb instruction");
    const uint16_t op = uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
    const unsigned rd = op & 7u, rn = (op >> 3) & 7u, rm = (op >> 6) & 7u;
    auto one = [](std::string s) { return Instruction{2, std::move(s)}; };
    if ((op & 0xF800) == 0xF000) { // BL, the only 32-bit M0+ instruction
        if (bytes.size() < 4) throw std::runtime_error("truncated Thumb BL instruction");
        const uint16_t second = uint16_t(bytes[2]) | (uint16_t(bytes[3]) << 8);
        if ((second & 0xF800) == 0xF800) {
            const int32_t delta = sign_extend(((op & 0x7FFu) << 12) | ((second & 0x7FFu) << 1), 23);
            return {4, "BL " + hex(address + 4u + uint32_t(delta), 8)};
        }
    }
    if ((op & 0xF800) < 0x1800) {
        static const char* shift[] = {"LSL", "LSR", "ASR"};
        const unsigned kind = op >> 11;
        unsigned amount = (op >> 6) & 31u;
        if (kind && !amount) amount = 32;
        return one(std::string(shift[kind]) + " " + reg(rd) + ", " + reg(rn) + ", #" + std::to_string(amount));
    }
    if ((op & 0xF800) == 0x1800) {
        const bool sub = op & 0x200, imm = op & 0x400;
        return one(std::string(sub ? "SUB " : "ADD ") + reg(rd) + ", " + reg(rn) + ", " +
                   (imm ? "#" + std::to_string(rm) : reg(rm)));
    }
    if ((op & 0xE000) == 0x2000) {
        static const char* names[] = {"MOV", "CMP", "ADD", "SUB"};
        return one(std::string(names[(op >> 11) & 3]) + " " + reg((op >> 8) & 7) + ", #" + std::to_string(op & 255));
    }
    if ((op & 0xFC00) == 0x4000) {
        static const char* names[] = {"AND", "EOR", "LSL", "LSR", "ASR", "ADC", "SBC", "ROR",
                                      "TST", "RSB", "CMP", "CMN", "ORR", "MUL", "BIC", "MVN"};
        const unsigned kind = (op >> 6) & 15;
        return one(std::string(names[kind]) + " " + reg(rd) + ", " + reg(rn) +
                   (kind == 9 ? ", #0" : ""));
    }
    if ((op & 0xFC00) == 0x4400) {
        const unsigned kind = (op >> 8) & 3;
        const unsigned dst = rd | ((op >> 4) & 8), src = rn | ((op >> 3) & 8);
        if (kind == 3) return one(std::string(op & 0x80 ? "BLX " : "BX ") + reg(src));
        static const char* names[] = {"ADD", "CMP", "MOV"};
        return one(std::string(names[kind]) + " " + reg(dst) + ", " + reg(src));
    }
    if ((op & 0xF800) == 0x4800)
        return one("LDR " + reg((op >> 8) & 7) + ", [pc, #" + std::to_string((op & 255) * 4) + "]");
    if ((op & 0xF000) == 0x5000) {
        static const char* names[] = {"STR", "STRH", "STRB", "LDRSB", "LDR", "LDRH", "LDRB", "LDRSH"};
        return one(std::string(names[(op >> 9) & 7]) + " " + reg(rd) + ", [" + reg(rn) + ", " + reg(rm) + "]");
    }
    if ((op & 0xE000) == 0x6000) {
        const bool byte = op & 0x1000, load = op & 0x800;
        const unsigned amount = ((op >> 6) & 31) * (byte ? 1 : 4);
        return one(std::string(load ? "LDR" : "STR") + (byte ? "B " : " ") + reg(rd) + ", [" + reg(rn) +
                   ", #" + std::to_string(amount) + "]");
    }
    if ((op & 0xF000) == 0x8000)
        return one(std::string(op & 0x800 ? "LDRH " : "STRH ") + reg(rd) + ", [" + reg(rn) +
                   ", #" + std::to_string(((op >> 6) & 31) * 2) + "]");
    if ((op & 0xF000) == 0x9000)
        return one(std::string(op & 0x800 ? "LDR " : "STR ") + reg((op >> 8) & 7) +
                   ", [sp, #" + std::to_string((op & 255) * 4) + "]");
    if ((op & 0xF000) == 0xA000)
        return one(std::string(op & 0x800 ? "ADD " : "ADR ") + reg((op >> 8) & 7) +
                   (op & 0x800 ? ", sp, #" : ", #") + std::to_string((op & 255) * 4));
    if ((op & 0xFF80) == 0xB000)
        return one(std::string(op & 0x80 ? "SUB" : "ADD") + " sp, #" + std::to_string((op & 127) * 4));
    if ((op & 0xFE00) == 0xB400) return one("PUSH " + regs((op & 255) | (op & 0x100 ? 1u << 14 : 0)));
    if ((op & 0xFE00) == 0xBC00) return one("POP " + regs((op & 255) | (op & 0x100 ? 1u << 15 : 0)));
    if ((op & 0xF500) == 0xB100) {
        const unsigned delta = (((op >> 9) & 1) << 6) | (((op >> 3) & 31) << 1);
        return one(std::string(op & 0x800 ? "CBNZ " : "CBZ ") + reg(rd) + ", " + hex(address + 4 + delta, 8));
    }
    if ((op & 0xFFC0) == 0xB200 || (op & 0xFFC0) == 0xB240 ||
        (op & 0xFFC0) == 0xB280 || (op & 0xFFC0) == 0xB2C0) {
        static const char* names[] = {"SXTH", "SXTB", "UXTH", "UXTB"};
        return one(std::string(names[(op >> 6) & 3]) + " " + reg(rd) + ", " + reg(rn));
    }
    if ((op & 0xFFC0) == 0xBA00 || (op & 0xFFC0) == 0xBA40 || (op & 0xFFC0) == 0xBAC0) {
        const char* name = (op & 0xC0) == 0 ? "REV" : (op & 0xC0) == 0x40 ? "REV16" : "REVSH";
        return one(std::string(name) + " " + reg(rd) + ", " + reg(rn));
    }
    if ((op & 0xFFE8) == 0xB660) return one(std::string("CPS") + (op & 0x10 ? "ID i" : "IE i"));
    if ((op & 0xFF00) == 0xBE00) return one("BKPT #" + std::to_string(op & 255));
    if ((op & 0xF000) == 0xC000)
        return one(std::string(op & 0x800 ? "LDMIA " : "STMIA ") + reg((op >> 8) & 7) + "!, " + regs(op & 255));
    if ((op & 0xFF00) == 0xDF00) return one("SVC #" + std::to_string(op & 255));
    if ((op & 0xF000) == 0xD000 && ((op >> 8) & 15) < 14) {
        static const char* cond[] = {"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
                                     "HI", "LS", "GE", "LT", "GT", "LE"};
        return one(std::string("B") + cond[(op >> 8) & 15] + " " +
                   hex(address + 4u + uint32_t(sign_extend((op & 255) << 1, 9)), 8));
    }
    if ((op & 0xF800) == 0xE000)
        return one("B " + hex(address + 4u + uint32_t(sign_extend((op & 0x7FF) << 1, 12)), 8));
    if (op == 0xBF00) return one("NOP");
    if (op == 0xBF10) return one("YIELD");
    if (op == 0xBF20) return one("WFE");
    if (op == 0xBF30) return one("WFI");
    if (op == 0xBF40) return one("SEV");
    return one(".hword " + hex(op, 4));
}
} // namespace m0boy::cpu::m0plus
