#include "m0boy/cpu/z80/decoder.h"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace m0boy::cpu::z80 {
namespace {
std::string hex(unsigned value, unsigned width = 4) {
    std::ostringstream out;
    out << "$" << std::uppercase << std::hex << std::setw(width)
        << std::setfill('0') << value;
    return out.str();
}
constexpr const char* pairs[] = {"BC", "DE", "HL", "SP"};
constexpr const char* conditions[] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
constexpr const char* alu[] = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
constexpr const char* rotates[] = {"RLC ", "RRC ", "RL ", "RR ", "SLA ", "SRA ", "SLL ", "SRL "};
}

Instruction Decoder::decode(uint16_t address, BankId bank) const {
    Instruction result{address};
    auto byte = [&]() {
        return memory_.read_banked(bank, static_cast<uint16_t>(address + result.length++));
    };
    auto word = [&]() {
        auto value = cpu::read_u16(memory_, bank, static_cast<uint16_t>(address + result.length));
        result.length += 2;
        return value;
    };
    auto immediate = [&]() { return hex(byte(), 2); };
    auto absolute = [&]() { return hex(word()); };
    auto transfer = [&](Flow flow, bool conditional, uint16_t target) {
        result.flow = flow;
        result.conditional = conditional;
        result.target = target;
        return hex(target);
    };
    auto relative = [&](bool conditional) {
        auto displacement = static_cast<int8_t>(byte());
        return transfer(Flow::jump, conditional,
                        cpu::relative_target(address, result.length, displacement));
    };

    uint8_t opcode = byte();
    unsigned prefixes = 0;
    const char* index = nullptr;
    while (opcode == 0xdd || opcode == 0xfd) {
        if (++prefixes == 65536) throw std::runtime_error("Z80 address space contains only index prefixes");
        index = opcode == 0xdd ? "IX" : "IY";
        opcode = byte();
    }
    const std::string hl = index ? index : "HL";
    auto pair = [&](unsigned p, bool stack = false) -> std::string {
        if (p == 2) return hl;
        if (p == 3 && stack) return "AF";
        return pairs[p];
    };
    std::string indexed_memory;
    auto reg = [&](unsigned code, bool substitute = true) -> std::string {
        if (index && code == 6) {
            if (indexed_memory.empty()) {
                auto offset = static_cast<int8_t>(byte());
                indexed_memory = "(" + hl + (offset < 0 ? "-" : "+") +
                    hex(offset < 0 ? -int(offset) : int(offset), 2) + ")";
            }
            return indexed_memory;
        }
        if (index && substitute && (code == 4 || code == 5)) {
            result.undocumented = true;
            return hl + (code == 4 ? "H" : "L");
        }
        return register_names8[code];
    };

    if (opcode == 0xcb) {
        // DDCB/FDCB: displacement precedes the final opcode; H/L destinations
        // remain H/L, and BIT never copies its result into a register.
        auto memory_operand = index ? reg(6) : std::string();
        OpcodeFields f(byte());
        auto operand = index ? memory_operand : reg(f.z);
        if (f.x == 0) {
            result.mnemonic = std::string(rotates[f.y]) + operand;
            result.undocumented |= f.y == 6;
        } else {
            const char* name = f.x == 1 ? "BIT " : f.x == 2 ? "RES " : "SET ";
            result.mnemonic = std::string(name) + std::to_string(f.y) + "," + operand;
        }
        if (index && f.z != 6) {
            result.undocumented = true;
            if (f.x != 1) result.mnemonic += "," + std::string(register_names8[f.z]);
        }
        result.cycles = index ? (f.x == 1 ? 20 : 23) + 4 * (prefixes - 1)
                              : f.z == 6 ? (f.x == 1 ? 12 : 15) : 8;
    } else if (opcode == 0xed) {
        // Index prefixes are ignored before ED instructions, including LD HL.
        OpcodeFields f(byte());
        result.cycles = 8;
        result.undocumented = true;
        result.mnemonic = "NOP";
        if (f.x == 1) {
            result.undocumented = false;
            switch (f.z) {
            case 0:
                result.mnemonic = f.y == 6 ? "IN (C)" : "IN " + std::string(register_names8[f.y]) + ",(C)";
                result.undocumented = f.y == 6;
                result.cycles = 12; break;
            case 1:
                result.mnemonic = "OUT (C)," + std::string(f.y == 6 ? "0" : register_names8[f.y]);
                result.undocumented = f.y == 6;
                result.cycles = 12; break;
            case 2:
                result.mnemonic = std::string(f.q ? "ADC HL," : "SBC HL,") + pairs[f.p];
                result.cycles = 15; break;
            case 3: {
                auto location = "(" + absolute() + ")";
                result.mnemonic = "LD " + (f.q ? std::string(pairs[f.p]) + "," + location
                                               : location + "," + pairs[f.p]);
                result.cycles = 20; break;
            }
            case 4:
                result.mnemonic = "NEG";
                result.undocumented = f.y != 0; break;
            case 5:
                result.mnemonic = f.y == 1 ? "RETI" : "RETN";
                result.undocumented = f.y > 1;
                result.flow = Flow::ret;
                result.cycles = 14; break;
            case 6: {
                constexpr unsigned modes[] = {0, 0, 1, 2, 0, 0, 1, 2};
                result.mnemonic = "IM " + std::to_string(modes[f.y]);
                result.undocumented = f.y != 0 && f.y != 2 && f.y != 3; break;
            }
            case 7: {
                constexpr const char* names[] = {"LD I,A", "LD R,A", "LD A,I", "LD A,R", "RRD", "RLD", "NOP", "NOP"};
                result.mnemonic = names[f.y];
                result.cycles = f.y < 4 ? 9 : f.y < 6 ? 18 : 8;
                result.undocumented = f.y >= 6; break;
            }
            }
        } else if (f.x == 2 && f.y >= 4 && f.z <= 3) {
            constexpr const char* blocks[4][4] = {
                {"LDI", "CPI", "INI", "OUTI"}, {"LDD", "CPD", "IND", "OUTD"},
                {"LDIR", "CPIR", "INIR", "OTIR"}, {"LDDR", "CPDR", "INDR", "OTDR"},
            };
            result.mnemonic = blocks[f.y - 4][f.z];
            result.undocumented = false;
            result.cycles = 16;
            if (f.y >= 6) {
                result.flow = Flow::repeat;
                result.conditional = true;
                result.target = static_cast<uint16_t>(address + prefixes);
                result.cycles_taken = 21 + 4 * prefixes;
            }
        }
        result.cycles += 4 * prefixes;
    } else {
        OpcodeFields f(opcode);
        if (f.x == 0) {
            switch (f.z) {
            case 0:
                switch (f.y) {
                case 0: result.mnemonic = "NOP"; result.cycles = 4; break;
                case 1: result.mnemonic = "EX AF,AF'"; result.cycles = 4; break;
                case 2: result.mnemonic = "DJNZ " + relative(true); result.cycles = 8; result.cycles_taken = 13; break;
                case 3: result.mnemonic = "JR " + relative(false); result.cycles = 12; break;
                default: result.mnemonic = "JR " + std::string(conditions[f.y - 4]) + "," + relative(true);
                         result.cycles = 7; result.cycles_taken = 12; break;
                } break;
            case 1:
                if (!f.q) { result.mnemonic = "LD " + pair(f.p) + "," + absolute(); result.cycles = 10; }
                else { result.mnemonic = "ADD " + hl + "," + pair(f.p); result.cycles = 11; }
                break;
            case 2: {
                std::string location;
                if (f.p < 2) location = "(" + std::string(pairs[f.p]) + ")";
                else location = "(" + absolute() + ")";
                auto value = f.p == 2 ? hl : "A";
                result.mnemonic = "LD " + (f.q ? value + "," + location : location + "," + value);
                result.cycles = f.p < 2 ? 7 : f.p == 2 ? 16 : 13; break;
            }
            case 3: result.mnemonic = std::string(f.q ? "DEC " : "INC ") + pair(f.p); result.cycles = 6; break;
            case 4: case 5:
                result.mnemonic = std::string(f.z == 4 ? "INC " : "DEC ") + reg(f.y);
                result.cycles = f.y == 6 ? (index ? 19 : 11) : 4; break;
            case 6: {
                auto destination = reg(f.y); // Consume displacement before n.
                result.mnemonic = "LD " + destination + "," + immediate();
                result.cycles = f.y == 6 ? (index ? 15 : 10) : 7; break;
            }
            case 7: {
                constexpr const char* names[] = {"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"};
                result.mnemonic = names[f.y]; result.cycles = 4; break;
            }
            }
        } else if (f.x == 1) {
            if (opcode == 0x76) {
                result.mnemonic = "HALT"; result.flow = Flow::halt; result.cycles = 4;
            } else {
                const bool memory = f.y == 6 || f.z == 6;
                auto destination = reg(f.y, !memory);
                auto source = reg(f.z, !memory);
                result.mnemonic = "LD " + destination + "," + source;
                result.cycles = memory ? (index ? 15 : 7) : 4;
            }
        } else if (f.x == 2) {
            result.mnemonic = std::string(alu[f.y]) + reg(f.z);
            result.cycles = f.z == 6 ? (index ? 15 : 7) : 4;
        } else {
            switch (f.z) {
            case 0:
                result.mnemonic = "RET " + std::string(conditions[f.y]);
                result.flow = Flow::ret; result.conditional = true;
                result.cycles = 5; result.cycles_taken = 11; break;
            case 1:
                if (!f.q) { result.mnemonic = "POP " + pair(f.p, true); result.cycles = 10; }
                else switch (f.p) {
                    case 0: result.mnemonic = "RET"; result.flow = Flow::ret; result.cycles = 10; break;
                    case 1: result.mnemonic = "EXX"; result.cycles = 4; break;
                    case 2: result.mnemonic = "JP (" + hl + ")"; result.flow = Flow::jump; result.cycles = 4; break;
                    case 3: result.mnemonic = "LD SP," + hl; result.cycles = 6; break;
                } break;
            case 2:
                result.mnemonic = "JP " + std::string(conditions[f.y]) + "," + transfer(Flow::jump, true, word());
                result.cycles = 10; break;
            case 3:
                switch (f.y) {
                case 0: result.mnemonic = "JP " + transfer(Flow::jump, false, word()); result.cycles = 10; break;
                case 2: result.mnemonic = "OUT (" + immediate() + "),A"; result.cycles = 11; break;
                case 3: result.mnemonic = "IN A,(" + immediate() + ")"; result.cycles = 11; break;
                case 4: result.mnemonic = "EX (SP)," + hl; result.cycles = 19; break;
                case 5: result.mnemonic = "EX DE,HL"; result.cycles = 4; break;
                case 6: result.mnemonic = "DI"; result.cycles = 4; break;
                case 7: result.mnemonic = "EI"; result.cycles = 4; break;
                default: throw std::logic_error("Unprocessed Z80 CB prefix");
                } break;
            case 4:
                result.mnemonic = "CALL " + std::string(conditions[f.y]) + "," + transfer(Flow::call, true, word());
                result.cycles = 10; result.cycles_taken = 17; break;
            case 5:
                if (!f.q) { result.mnemonic = "PUSH " + pair(f.p, true); result.cycles = 11; }
                else { result.mnemonic = "CALL " + transfer(Flow::call, false, word()); result.cycles = 17; }
                break;
            case 6: result.mnemonic = std::string(alu[f.y]) + immediate(); result.cycles = 7; break;
            case 7:
                result.mnemonic = "RST " + transfer(Flow::call, false, f.y * 8); result.cycles = 11; break;
            }
        }
        result.cycles += 4 * prefixes;
        if (result.cycles_taken) result.cycles_taken += 4 * prefixes;
    }
    if (!result.cycles_taken) result.cycles_taken = result.cycles;
    return result;
}
} // namespace m0boy::cpu::z80
