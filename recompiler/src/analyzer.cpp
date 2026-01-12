/**
 * @file analyzer.cpp
 * @brief Control flow analyzer implementation (stub for MVP)
 */

#include "recompiler/analyzer.h"
#include <algorithm>
#include <queue>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace gbrecomp {

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

static uint32_t make_address(uint8_t bank, uint16_t addr) {
    return (static_cast<uint32_t>(bank) << 16) | addr;
}

static uint8_t get_bank(uint32_t addr) {
    return static_cast<uint8_t>(addr >> 16);
}

static uint16_t get_offset(uint32_t addr) {
    return static_cast<uint16_t>(addr & 0xFFFF);
}

/* ============================================================================
 * AnalysisResult Implementation
 * ========================================================================== */

const Instruction* AnalysisResult::get_instruction(uint8_t bank, uint16_t addr) const {
    uint32_t full_addr = make_addr(bank, addr);
    auto it = addr_to_index.find(full_addr);
    if (it != addr_to_index.end() && it->second < instructions.size()) {
        return &instructions[it->second];
    }
    return nullptr;
}

const BasicBlock* AnalysisResult::get_block(uint8_t bank, uint16_t addr) const {
    uint32_t full_addr = make_addr(bank, addr);
    auto it = blocks.find(full_addr);
    if (it != blocks.end()) {
        return &it->second;
    }
    return nullptr;
}

const Function* AnalysisResult::get_function(uint8_t bank, uint16_t addr) const {
    uint32_t full_addr = make_addr(bank, addr);
    auto it = functions.find(full_addr);
    if (it != functions.end()) {
        return &it->second;
    }
    return nullptr;
}

/* ============================================================================
 * RST Pattern Detection
 * ========================================================================== */

/**
 * @brief Check if a RST vector contains only 0xFF padding (not real code)
 * 
 * Many ROMs have 0xFF padding at unused RST vector locations.
 * This prevents infinite recursion when analyzing these vectors.
 * 
 * @param rom The ROM to check
 * @param vector The RST vector address (0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38)
 * @return true if the vector contains only 0xFF bytes
 */
static bool is_rst_padding(const ROM& rom, uint16_t vector) {
    // RST vectors are 8 bytes apart, check all bytes up to the next vector
    uint16_t end = vector + 8;
    if (end > 0x40) end = 0x40;  // Don't go past RST 38 region
    
    for (uint16_t addr = vector; addr < end; addr++) {
        if (rom.read_banked(0, addr) != 0xFF) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Check if RST 28 is a jump table dispatcher
 * 
 * Tetris and many other GB games use RST 28 as a computed jump table:
 *   ADD A,A       ; Double A (table entries are 2 bytes)
 *   POP HL        ; Get return address (points to table)
 *   ...
 *   JP (HL)       ; Jump to looked-up address
 * 
 * The bytes following RST 28 calls are table data, NOT code.
 */
static bool is_rst28_jump_table(const ROM& rom) {
    // Check for the pattern starting at 0x28:
    // 87 E1 ... E9 (ADD A,A; POP HL; ...; JP (HL))
    if (rom.read_banked(0, 0x28) == 0x87 &&  // ADD A,A
        rom.read_banked(0, 0x29) == 0xE1) {  // POP HL
        // Look for JP (HL) = 0xE9 somewhere in 0x28-0x3F region
        for (uint16_t addr = 0x2A; addr < 0x40; addr++) {
            if (rom.read_banked(0, addr) == 0xE9) {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Check if RST 28 falls through into RST 30
 * 
 * When RST 28 is a jump table dispatcher, it typically continues through
 * RST 30's space to reach JP (HL). In this case, RST 30 should NOT be
 * marked as a separate function entry since it's part of RST 28's routine.
 * 
 * Pattern: RST 28 at 0x28-0x2F falls through to code at 0x30-0x33 ending with JP (HL)
 */
static bool rst28_uses_rst30(const ROM& rom) {
    if (!is_rst28_jump_table(rom)) {
        return false;
    }
    
    // Check if JP (HL) (0xE9) is in the 0x30-0x37 range (RST 30 region)
    for (uint16_t addr = 0x30; addr < 0x38; addr++) {
        if (rom.read_banked(0, addr) == 0xE9) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Extract jump table entries following an RST 28 call site
 * 
 * When RST 28 is a jump table dispatcher, the bytes immediately following
 * the RST 28 opcode are 16-bit addresses (in little-endian format).
 * 
 * Pattern from Tetris:
 *   ldh  a,(0cdh)     ; Load index value
 *   rst  28h          ; Call jump table dispatcher (opcode 0xEF)
 *   .dw  l0078        ; Entry 0
 *   .dw  l009f        ; Entry 1
 *   ...
 * 
 * @param rom The ROM to read from
 * @param rst_call_addr Address of the RST 28 opcode (0xEF)
 * @param bank Bank number for the call site
 * @return Vector of extracted jump table target addresses
 */
static std::vector<uint16_t> extract_rst28_table_entries(const ROM& rom, uint16_t rst_call_addr, uint8_t bank) {
    std::vector<uint16_t> targets;
    
    // Table starts immediately after the RST 28 opcode (1 byte)
    uint16_t table_start = rst_call_addr + 1;
    
    // We don't know the table size statically.
    // Heuristic: Read addresses until we hit:
    // 1. An address that's clearly not code (below 0x0100 except for RST vectors)
    // 2. An address that overlaps with known code
    // 3. An unreasonably large number of entries (e.g., > 64)
    // 4. An address at or past 0x8000 (not ROM)
    
    const int MAX_TABLE_ENTRIES = 64;  // Tetris has up to 44 entries in its main state machine
    
    for (int i = 0; i < MAX_TABLE_ENTRIES; i++) {
        uint16_t entry_addr = table_start + i * 2;
        
        // Make sure we can read 2 bytes
        size_t rom_offset;
        if (entry_addr < 0x4000) {
            rom_offset = entry_addr;
        } else {
            rom_offset = static_cast<size_t>(bank) * 0x4000 + (entry_addr - 0x4000);
        }
        
        if (rom_offset + 1 >= rom.size()) {
            break;  // Past end of ROM
        }
        
        // Read 16-bit address (little-endian)
        // For addresses in bank 0 region (< 0x4000), always use bank 0
        uint8_t read_bank = (entry_addr < 0x4000) ? 0 : bank;
        uint8_t lo = rom.read_banked(read_bank, entry_addr);
        uint8_t hi = rom.read_banked(read_bank, entry_addr + 1);
        uint16_t target = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        
        // Validate the target address
        if (target >= 0x8000) {
            // Not ROM - likely end of table
            break;
        }
        
        // Address should be aligned to reasonable code
        // Very low addresses (0x00-0x3F) are RST/INT vectors, which is OK
        // Addresses 0x40-0xFF should be valid only for known interrupt handlers
        // Core code typically starts at 0x100+
        if (target == 0x0000 || target == 0xFFFF) {
            // Invalid entry, likely end of table
            break;
        }
        
        // Add the target if it looks valid
        targets.push_back(target);
    }
    
    return targets;
}

/* ============================================================================
 * Bank Switch Detection
 * ========================================================================== */

/**
 * @brief Detect immediate bank values from common patterns
 * 
 * Looks for patterns like:
 *   LD A, n      ; n is bank number
 *   LD (2000), A ; or LD (2100), A, etc.
 */
static std::set<uint8_t> detect_bank_values(const ROM& rom) {
    std::set<uint8_t> banks;
    banks.insert(0);  // Bank 0 is always present
    banks.insert(1);  // Bank 1 is the default switchable bank
    
    // Use ROM header to know how many banks exist
    uint16_t bank_count = rom.header().rom_banks;
    for (uint16_t i = 0; i < bank_count && i < 256; i++) {
        banks.insert(static_cast<uint8_t>(i));
    }
    
    return banks;
}

/**
 * @brief Heuristic check if an address looks like valid code start
 * 
 * Checks for:
 * 1. Repetitive byte patterns (indicative of data/tiles)
 * 2. Immediate invalid opcodes
 * 3. Dense invalid operations
 */
static int is_likely_valid_code(const ROM& rom, uint8_t bank, uint16_t addr) {
    // 1. Check for repetitive patterns (e.g. tile data)
    // Most code has entropy. Data often repeats.
    const int PATTERN_CHECK_LEN = 256;
    if (addr + PATTERN_CHECK_LEN < 0x8000) {
        uint8_t bytes[PATTERN_CHECK_LEN];
        for (int i = 0; i < PATTERN_CHECK_LEN; i++) {
            bytes[i] = rom.read_banked(bank, addr + i);
        }
        
        // Check for small repeating periods (1 to 8 bytes)
        // Check for repeating periods (1 to 8 bytes)
        // If we see many full repetitions of ANY pattern, assume data.
        // Code loops, but rarely repeats linear instruction sequences multiple times.
        for (int period = 1; period <= 8; period++) {
            // Check if pattern repeats significantly (covering > 64 bytes)
            // e.g. ABC ABC ABC ...
            
            const int REQUIRED_REPEATS = 32;
            const int REQUIRED_LEN = period * REQUIRED_REPEATS;
            
            if (PATTERN_CHECK_LEN < REQUIRED_LEN) continue;
            
            bool matches = true;
            for (int i = 0; i < REQUIRED_LEN - period; i++) {
                if (bytes[i] != bytes[i + period]) {
                    matches = false;
                    break;
                }
            }
            
            if (matches) {
                // std::cout << "[DEBUG] Bank " << (int)bank << " rejected: Repeating pattern period " << period << "\n";
                // Don't reject purely on pattern unless very strong
                // return false; 
                // Actually, let's keep pattern check but be permissive?
                // Castlevania Bank 1 starts with 01s.
                // My parameters passed the check.
                // Keeping existing logic:
                if (matches) return 0;
            }
        }
    }

    // 2. Decode ahead and check for validity
    Decoder decoder(rom);
    uint16_t curr = addr;
    int instructions_checked = 0;
    const int MAX_CHECK = 128; // Check smaller chunk to avoid hitting data islands

    while (instructions_checked < MAX_CHECK) {
        Instruction instr = decoder.decode(curr, bank);
        
        // Immediate hard failure
        if (instr.type == InstructionType::UNDEFINED || instr.type == InstructionType::INVALID) {
            // std::cout << "[DEBUG] Bank " << (int)bank << " rejected: Undefined/Invalid opcode " 
            //           << std::hex << (int)instr.opcode << " at " << curr << std::dec << "\n";
            return 0;
        }
        
        // Check for suspicious control flow
        // Data often interprets as mild control flow (conditional jumps)
        
        // If we hit logical terminator, we are probably okay
        // Unconditional Return or Jump usually implies we survived the block
        if (instr.is_return && !instr.is_conditional) return (curr + instr.length - addr);
        if (instr.type == InstructionType::JP_NN) return (curr + instr.length - addr);
        if (instr.type == InstructionType::JR_N) return (curr + instr.length - addr);
        
        curr += instr.length;
        if (curr >= 0x8000) return 0;
        instructions_checked++;
    }

    // If we survived this long without a terminator, doubtful.
    return 0;
}

/* ============================================================================
 * Analysis Implementation
 * ========================================================================== */

AnalysisResult analyze(const ROM& rom, const AnalyzerOptions& options) {
    AnalysisResult result;
    result.rom = &rom;
    result.entry_point = 0x100;
    
    // Add standard GameBoy entry points
    result.interrupt_vectors = {0x40, 0x48, 0x50, 0x58, 0x60};  // Interrupt vectors
    
    Decoder decoder(rom);
    
    // Detect which banks are used
    std::set<uint8_t> known_banks = detect_bank_values(rom);
    
    // Track addresses to explore: (addr, known_a, known_hl, current_bank_context)
    struct AnalysisState {
        uint32_t addr;
        int known_a;
        int known_hl; // -1 if unknown, otherwise 16-bit value
        uint8_t current_bank;
    };
    std::queue<AnalysisState> work_queue;
    std::set<uint32_t> visited;
    
    // Entry point is always a function (bank 0)
    result.call_targets.insert(make_address(0, 0x100));
    
    // RST vectors
    bool skip_rst30 = rst28_uses_rst30(rom);
    for (uint16_t vec : {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38}) {
        if (is_rst_padding(rom, vec)) continue;
        if (vec == 0x30 && skip_rst30) continue;
        result.call_targets.insert(make_address(0, vec));
        work_queue.push({make_address(0, vec), -1, -1, 1});
    }
    
    // Interrupt vectors
    for (uint16_t vec : result.interrupt_vectors) {
        result.call_targets.insert(make_address(0, vec));
        work_queue.push({make_address(0, vec), -1, -1, 1});
    }
    
    // Start from entry point
    work_queue.push({make_address(0, 0x100), -1, -1, 1});
    
    // For MBC games
    if (rom.header().mbc_type != MBCType::NONE && options.analyze_all_banks) {
        std::cerr << "Analyzing all " << known_banks.size() << " banks\n";
        for (uint8_t bank : known_banks) {
                if (bank > 0) {
                    if (is_likely_valid_code(rom, bank, 0x4000)) {
                        work_queue.push({make_address(bank, 0x4000), -1, -1, bank});
                        result.call_targets.insert(make_address(bank, 0x4000));
                    } else {
                        // std::cout << "[INFO] Skipping likely data bank " << (int)bank << " at 0x4000\n";
                    }
                }
        }
    }
    
    // Add overlay entry points
    for (const auto& ov : options.ram_overlays) {
        uint32_t addr = make_address(0, ov.ram_addr);
        result.call_targets.insert(addr);
        work_queue.push({addr, -1, -1, 1});
    }

    // Add manual entry points
    for (uint32_t addr : options.entry_points) {
        result.call_targets.insert(addr);
        uint8_t bank = get_bank(addr);
        uint8_t context = (bank > 0) ? bank : 1;
        work_queue.push({addr, -1, -1, context});
    }
    
    // Multi-pass analysis
    bool scanning_pass = false;

    // Explore all reachable code
    while (true) {
        // Drain work queue
        while (!work_queue.empty()) {
            auto item = work_queue.front();
        work_queue.pop();
        
        uint32_t addr = item.addr;
        int known_a = item.known_a;
        int known_hl = item.known_hl;
        uint8_t current_switchable_bank = item.current_bank;
        
        if (visited.count(addr)) continue;
        
        uint8_t bank = get_bank(addr);
        uint16_t offset = get_offset(addr);
        
        // Check if inside any RAM overlay
        const AnalyzerOptions::RamOverlay* overlay = nullptr;
        for (const auto& ov : options.ram_overlays) {
            if (offset >= ov.ram_addr && offset < ov.ram_addr + ov.size) {
                overlay = &ov;
                break;
            }
        }
        
        // Only analyze ROM space or RAM overlays
        if (offset >= 0x8000 && !overlay) continue;
        
        // Bank mapping rules
        if (offset < 0x4000) {
            bank = 0;  // Force bank 0 for this region
            addr = make_address(0, offset);
            if (visited.count(addr)) continue;
        } else if (offset < 0x8000 && bank == 0) {
            bank = 1;  // Default to bank 1
            addr = make_address(1, offset);
            if (visited.count(addr)) continue;
        } else if (overlay) {
             if (visited.count(addr)) continue;
        }
        
        if (bank > 0) current_switchable_bank = bank;
        
        // Calculate ROM offset
        size_t rom_offset;
        if (overlay) {
            uint8_t src_bank = get_bank(overlay->rom_addr);
            uint16_t src_addr = get_offset(overlay->rom_addr);
            if (src_addr < 0x4000) rom_offset = src_addr;
            else rom_offset = static_cast<size_t>(src_bank) * 0x4000 + (src_addr - 0x4000);
            rom_offset += (offset - overlay->ram_addr);
        } else if (offset < 0x4000) {
            rom_offset = offset;
        } else {
            rom_offset = static_cast<size_t>(bank) * 0x4000 + (offset - 0x4000);
        }
        if (rom_offset >= rom.size()) continue;
        
        visited.insert(addr);
        
        // Decode instruction
        Instruction instr;
        if (overlay) {
             uint8_t src_bank = get_bank(overlay->rom_addr);
             uint16_t src_addr = get_offset(overlay->rom_addr) + (offset - overlay->ram_addr);
             instr = decoder.decode(src_addr, src_bank);
             instr.address = offset; 
             instr.bank = 0; // RAM is bank 0
        } else {
            instr = decoder.decode(offset, bank);
        }

        /* -------------------------------------------------------------
         * Constant Propagation (A and HL)
         * ------------------------------------------------------------- */
         
        // Track A (for banking)
        if (instr.opcode == 0x3E) { // LD A, n
            known_a = instr.imm8;
            if (options.verbose && bank == 0 && offset == 0x00CC) {
                 std::cout << "[DEBUG] At 00CC: LD A, " << (int)known_a << ". known_a updated.\n";
            }
        } 
        else if (instr.opcode == 0x7E) { // LD A, (HL)
            if (known_hl != -1) {
                // Read from ROM at known HL
                uint16_t addr = (uint16_t)known_hl;
                // Only valid if reading from simple ROM regions
                if (addr < 0x4000) {
                     known_a = rom.read_banked(0, addr);
                } else if (addr < 0x8000) {
                     // Depends on which bank is active for DATA reading?
                     // Usually data tables are in the same bank or specific data banks.
                     // Analyzer doesn't track data bank context separate from code bank context,
                     // but usually they match.
                     if (bank > 0) {
                         known_a = rom.read_banked(bank, addr);
                     } else {
                         // If we are in bank 0 code reading bank 1 data, unknown
                         known_a = -1;
                     }
                } else {
                     // Reading RAM/Regs -> Unknown
                     known_a = -1;
                }
            } else {
                known_a = -1;
            }
        }
        else if (instr.opcode == 0xFA) { // LD A, (nn)
             uint16_t addr = instr.imm16;
             if (addr < 0x4000) {
                  known_a = rom.read_banked(0, addr);
             } else if (addr < 0x8000 && bank > 0) {
                  known_a = rom.read_banked(bank, addr);
             } else {
                  known_a = -1;
             }
        }
        // LD A, (BC)/(DE) or other loads invalidate A
        else if (instr.opcode == 0x0A || instr.opcode == 0x1A || 
                 instr.opcode == 0xF0 || instr.opcode == 0xF2) {
             known_a = -1;
        }
        else if (instr.opcode >= 0x40 && instr.opcode <= 0x7F) {
             // LD r, r'
             // Only invalidate if A is the destination (bits 3-5 are 111 => 7)
             if (((instr.opcode >> 3) & 7) == 7 && instr.opcode != 0x7F) {
                 // LD A, r (except LD A, A)
                 known_a = -1;
             }
        }
        else if (instr.opcode == 0xAF) { // XOR A
            known_a = 0;
        }
        // INC/DEC A, ADD, SUB etc invalidate A
        else if ((instr.opcode >= 0x80 && instr.opcode <= 0xBF) || 
                 instr.opcode == 0x3C || instr.opcode == 0x3D || instr.opcode == 0x27 || instr.opcode == 0x2F) {
            known_a = -1;
        }    
        
        // Track HL (for JP HL)
        if (instr.opcode == 0x21) { // LD HL, nn
            known_hl = instr.imm16;
        } else if (instr.opcode == 0xE1) { // POP HL
            known_hl = -1;
        } else if (instr.opcode == 0x2A || instr.opcode == 0x3A) { // LD A, (HL+) / LD A, (HL-)
            // HL changes
            known_hl = -1;
        } else if (instr.opcode == 0x22 || instr.opcode == 0x32) { // LD (HL+), A / LD (HL-), A
            // HL changes
            known_hl = -1;
        } else if (instr.opcode == 0x23 || instr.opcode == 0x2B || instr.opcode == 0x39 || 
                   instr.opcode == 0x09 || instr.opcode == 0x19 || instr.opcode == 0x29) {
            // INC/DEC HL, ADD HL, ...
            known_hl = -1;
        } else if (instr.opcode >= 0x60 && instr.opcode <= 0x6F) {
            // LD H, r / LD L, r
            // Invalidate unless we track H/L separately
            known_hl = -1;
        } else if (instr.opcode == 0xF8) { // LD HL, SP+n
             known_hl = -1; 
        }

        /* -------------------------------------------------------------
         * Bank Switching Detection
         * ------------------------------------------------------------- */
        if (instr.opcode == 0xEA) { // LD (nn), A
            if (instr.imm16 >= 0x2000 && instr.imm16 <= 0x3FFF) {
                bool is_dynamic = (known_a == -1);
                uint8_t target_b = is_dynamic ? 1 : (known_a == 0 ? 1 : known_a);
                target_b &= 0xFF;
                
                result.bank_tracker.record_bank_switch(addr, target_b, is_dynamic);
                
                if (!is_dynamic) {
                    current_switchable_bank = target_b;
                }
            }
        }

        // Trace logging
        if (options.trace_log) {
            std::cout << "[TRACE] " << std::hex << std::setfill('0') << std::setw(2) << (int)bank
                      << ":" << std::setw(4) << offset << " " << instr.disassemble() << std::dec << "\n";
        }
        
        // Check padding
        if (bank > 0 && instr.opcode == 0xFF) {
            bool is_padding = true;
            for (int i = 1; i < 16; i++) {
                if (rom.read_banked(bank, offset + i) != 0xFF) { is_padding = false; break; }
            }
            if (is_padding) continue;
        }

        if (instr.type == InstructionType::UNDEFINED) {
             std::cout << "[ERROR] Undefined instruction at " << std::hex << (int)bank << ":" << offset << "\n";
             continue;
        }

        if (options.max_instructions > 0 && result.instructions.size() >= options.max_instructions) {
            break;
        }
        
        size_t idx = result.instructions.size();
        result.instructions.push_back(instr);
        result.addr_to_index[addr] = idx;
        
        auto target_bank = [&](uint16_t target) -> uint8_t {
            if (target < 0x4000) return 0;
            if (rom.header().mbc_type == MBCType::NONE) return 1;
            return current_switchable_bank;
        };
        
        if (instr.type == InstructionType::RST) {
            if (is_rst_padding(rom, instr.rst_vector)) continue;
            
            result.call_targets.insert(make_address(0, instr.rst_vector));
            work_queue.push({make_address(0, instr.rst_vector), -1, -1, 1});
            
            bool is_rst28_jt = (instr.rst_vector == 0x28 && is_rst28_jump_table(rom));
            if (is_rst28_jt) {
                std::vector<uint16_t> table_targets = extract_rst28_table_entries(rom, offset, bank);
                for (uint16_t target : table_targets) {
                    uint8_t tbank = (target < 0x4000) ? 0 : bank;
                    if (tbank == 0 && target >= 0x4000) tbank = 1;

                    if (is_likely_valid_code(rom, tbank, target)) {
                        result.call_targets.insert(make_address(tbank, target));
                        work_queue.push({make_address(tbank, target), -1, -1, tbank});
                        result.label_addresses.insert(make_address(tbank, target));
                    }
                }
            } else {
                uint32_t fall_through = make_address(bank, offset + instr.length);
                result.label_addresses.insert(fall_through);
                work_queue.push({fall_through, known_a, known_hl, current_switchable_bank});
            }
        } else if (instr.is_call) {
            uint16_t target = instr.imm16;
            uint8_t tbank = target_bank(target);
            instr.resolved_target_bank = tbank;
            
            if (tbank > 0 && tbank != bank) {
                if (!is_likely_valid_code(rom, tbank, target)) continue;
            }

            result.call_targets.insert(make_address(tbank, target));
            work_queue.push({make_address(tbank, target), -1, -1, tbank});
            
            if (tbank != bank) {
                result.stats.cross_bank_calls++;
                result.bank_tracker.record_cross_bank_call(offset, target, bank, tbank);
            }
            
            uint32_t fall_through = make_address(bank, offset + instr.length);
            result.label_addresses.insert(fall_through);
            work_queue.push({fall_through, known_a, known_hl, current_switchable_bank});
        } else if (instr.is_jump) {
            if (instr.type == InstructionType::JP_NN || instr.type == InstructionType::JP_CC_NN) {
                uint16_t target = instr.imm16;
                uint8_t tbank = target_bank(target);
                instr.resolved_target_bank = tbank;
                if (target >= 0x4000 && target <= 0x7FFF) {
                    if (tbank > 0 && tbank != bank) {
                        if (!is_likely_valid_code(rom, tbank, target)) continue;
                    }
                    result.call_targets.insert(make_address(tbank, target));
                }
                result.label_addresses.insert(make_address(tbank, target));
                work_queue.push({make_address(tbank, target), known_a, known_hl, tbank});
            } else if (instr.type == InstructionType::JR_N || instr.type == InstructionType::JR_CC_N) {
                uint16_t target = offset + instr.length + instr.offset;
                result.label_addresses.insert(make_address(bank, target));
                work_queue.push({make_address(bank, target), known_a, known_hl, current_switchable_bank});
            } else if (instr.type == InstructionType::JP_HL) {
                // JP HL: Check if we know where HL points
                if (known_hl != -1) {
                    uint16_t target = (uint16_t)known_hl;
                    uint8_t tbank = target_bank(target);
                    instr.resolved_target_bank = tbank; // May help debugging
                    
                    std::cout << "[ANALYSIS] Resolved static JP HL at " 
                              << std::hex << (int)bank << ":" << offset 
                              << " -> " << (int)tbank << ":" << target << std::dec << "\n";
                              
                    result.call_targets.insert(make_address(tbank, target));
                    result.label_addresses.insert(make_address(tbank, target));
                    result.computed_jump_targets.insert(make_address(tbank, target));
                    work_queue.push({make_address(tbank, target), known_a, known_hl, tbank});
                } else {
                    std::cout << "[ANALYSIS] Unresolved JP HL at " << std::hex << (int)bank << ":" << offset << std::dec << "\n";
                }
            }
            
            if (instr.is_conditional) {
                uint32_t fall_through = make_address(bank, offset + instr.length);
                result.label_addresses.insert(fall_through);
                work_queue.push({fall_through, known_a, known_hl, current_switchable_bank});
            }
        } else if (instr.is_return) {
            if (instr.is_conditional) {
                uint32_t fall_through = make_address(bank, offset + instr.length);
                result.label_addresses.insert(fall_through);
                work_queue.push({fall_through, known_a, known_hl, current_switchable_bank});
            }
        } else {
            work_queue.push({make_address(bank, offset + instr.length), known_a, known_hl, current_switchable_bank});
        }
    } // End work_queue loop

    // Aggressive Code Scanning
    if (options.aggressive_scan && !scanning_pass) {
        scanning_pass = true; // prevent infinite loops if we find nothing new
        
        if (options.verbose) std::cout << "[ANALYSIS] Starting aggressive scan for missing code..." << std::endl;
        
        size_t found_count = 0;

        // Iterate through all known banks (and bank 0)
        std::vector<uint8_t> banks_to_scan;
        banks_to_scan.push_back(0);
        for (uint8_t b : known_banks) if (b > 0) banks_to_scan.push_back(b);

        for (uint8_t bank : banks_to_scan) {
            uint16_t start_addr = (bank == 0) ? 0x0000 : 0x4000;
            uint16_t end_addr = (bank == 0) ? 0x3FFF : 0x7FFF;
            
            for (uint32_t addr = start_addr; addr <= end_addr; ) {
                // If already visited, skip this address
                if (visited.count(make_address(bank, addr))) {
                    addr++; 
                    continue;
                }
                
                // Alignment heuristic: most functions start on some boundary? No.
                // But we can skip obvious padding (0xFF or 0x00)
                uint8_t byte = rom.read_banked(bank, addr);
                if (byte == 0xFF || byte == 0x00) {
                    addr++;
                    continue;
                }

                // Check if this looks like valid code
                int code_len = is_likely_valid_code(rom, bank, addr);
                if (code_len > 0) {
                    if (options.verbose) {
                        std::cout << "[ANALYSIS] Detected potential function at " 
                                  << std::hex << (int)bank << ":" << addr << std::dec << "\n";
                    }
                    
                    // Add as a new entry point
                    uint32_t entry = make_address(bank, addr);
                    result.call_targets.insert(entry);
                    
                    // Add to queue
                    uint8_t context = (bank > 0) ? bank : 1;
                    work_queue.push({entry, -1, -1, context});
                    found_count++;
                    
                    // Skip the block we just found to avoid overlapping detection
                    addr += code_len;
                    continue;
                } else {
                    // Not valid code, skip ahead.
                    addr++;
                }
            }
        }
        
        if (found_count > 0) {
            if (options.verbose) std::cout << "[ANALYSIS] Found " << found_count << " new entry points. Restarting analysis." << std::endl;
            scanning_pass = false; // Reset pass flag to allow further scanning after this batch is analyzed
            continue; // Go back to work_queue processing
        }
    }
    
    // If we get here, we are done
    break; 
    } // End while(true)
    
    // Build basic blocks from instruction boundaries
    std::set<uint32_t> block_starts;
    block_starts.insert(make_address(0, 0x100));  // Entry point
    
    for (uint32_t target : result.call_targets) {
        block_starts.insert(target);
    }
    for (uint32_t target : result.label_addresses) {
        block_starts.insert(target);
    }
    
    // Create blocks
    for (uint32_t start : block_starts) {
        if (!visited.count(start)) continue;
        
        BasicBlock block;
        block.start_address = get_offset(start);
        block.bank = get_bank(start);
        block.is_reachable = true;
        
        if (result.call_targets.count(start)) {
            block.is_function_entry = true;
        }
        
        // Find instructions in this block
        uint32_t curr = start;
        while (visited.count(curr)) {
            auto it = result.addr_to_index.find(curr);
            if (it == result.addr_to_index.end()) break;
            
            block.instruction_indices.push_back(it->second);
            const Instruction& instr = result.instructions[it->second];
            
            block.end_address = get_offset(curr) + instr.length;
            
            // Track successors for control flow
            if (instr.is_jump) {
                if (instr.type == InstructionType::JP_NN || instr.type == InstructionType::JP_CC_NN) {
                    block.successors.push_back(instr.imm16);
                } else if (instr.type == InstructionType::JR_N || instr.type == InstructionType::JR_CC_N) {
                    uint16_t target = get_offset(curr) + instr.length + instr.offset;
                    block.successors.push_back(target);
                }
                // Conditional jumps also fall through
                if (instr.is_conditional) {
                    block.successors.push_back(get_offset(curr) + instr.length);
                }
            } else if (instr.is_return && instr.is_conditional) {
                // Conditional returns fall through if condition is false
                block.successors.push_back(get_offset(curr) + instr.length);
            }
            
            // Check if this ends the block
            if (instr.is_jump || instr.is_return || instr.is_call) {
                // CALLs fall through to next instruction after return
                if (instr.is_call) {
                    block.successors.push_back(get_offset(curr) + instr.length);
                }
                break;
            }
            
            curr = make_address(block.bank, get_offset(curr) + instr.length);
            
            // Check if next instruction starts a new block
            if (block_starts.count(curr)) {
                // Fall through to the new block - add as successor
                block.successors.push_back(get_offset(curr));
                break;
            }
        }
        
        result.blocks[start] = block;
    }
    
    // Create functions from call targets
    for (uint32_t target : result.call_targets) {
        auto block_it = result.blocks.find(target);
        if (block_it == result.blocks.end()) continue;
        
        Function func;
        func.name = generate_function_name(get_bank(target), get_offset(target));
        func.entry_address = get_offset(target);
        func.bank = get_bank(target);
        func.block_addresses.push_back(get_offset(target));
        
        // Add all blocks reachable from this function (simple DFS)
        std::queue<uint32_t> func_queue;
        std::set<uint32_t> func_visited;
        func_queue.push(target);
        
        while (!func_queue.empty()) {
            uint32_t block_addr = func_queue.front();
            func_queue.pop();
            
            if (func_visited.count(block_addr)) continue;
            func_visited.insert(block_addr);
            
            auto blk = result.blocks.find(block_addr);
            if (blk == result.blocks.end()) continue;
            
            // Add this block to function if not already there
            if (block_addr != target) {
                func.block_addresses.push_back(get_offset(block_addr));
            }
            
            // Follow successors (but not into other functions)
            for (uint16_t succ : blk->second.successors) {
                uint32_t succ_addr = make_address(blk->second.bank, succ);
                // Don't follow into other function entry points
                if (result.call_targets.count(succ_addr) && succ_addr != target) continue;
                if (!func_visited.count(succ_addr)) {
                    func_queue.push(succ_addr);
                }
            }
        }
        
        result.functions[target] = func;
    }
    
    // Update stats
    result.stats.total_instructions = result.instructions.size();
    result.stats.total_blocks = result.blocks.size();
    result.stats.total_functions = result.functions.size();
    
    return result;
}

AnalysisResult analyze_bank(const ROM& rom, uint8_t bank, const AnalyzerOptions& options) {
    (void)bank; // Unused parameter
    // For now, just analyze the whole ROM
    // TODO: Filter to specific bank
    return analyze(rom, options);
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

std::string generate_function_name(uint8_t bank, uint16_t address) {
    std::ostringstream ss;
    
    // Check for known GameBoy entry points
    if (bank == 0) {
        switch (address) {
            case 0x0000: return "rst_00";
            case 0x0008: return "rst_08";
            case 0x0010: return "rst_10";
            case 0x0018: return "rst_18";
            case 0x0020: return "rst_20";
            case 0x0028: return "rst_28";
            case 0x0030: return "rst_30";
            case 0x0038: return "rst_38";
            case 0x0040: return "int_vblank";
            case 0x0048: return "int_lcd_stat";
            case 0x0050: return "int_timer";
            case 0x0058: return "int_serial";
            case 0x0060: return "int_joypad";
            case 0x0100: return "gb_main";  // Avoid shadowing C main()
        }
    }
    
    ss << "func_";
    if (bank > 0) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)bank << "_";
    }
    ss << std::hex << std::setfill('0') << std::setw(4) << address;
    return ss.str();
}

std::string generate_label_name(uint8_t bank, uint16_t address) {
    std::ostringstream ss;
    ss << "loc_";
    if (bank > 0) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)bank << "_";
    }
    ss << std::hex << std::setfill('0') << std::setw(4) << address;
    return ss.str();
}

void print_analysis_summary(const AnalysisResult& result) {
    std::cout << "=== Analysis Summary ===" << std::endl;
    std::cout << "Total instructions: " << result.stats.total_instructions << std::endl;
    std::cout << "Total basic blocks: " << result.stats.total_blocks << std::endl;
    std::cout << "Total functions: " << result.stats.total_functions << std::endl;
    std::cout << "Call targets: " << result.call_targets.size() << std::endl;
    std::cout << "Label addresses: " << result.label_addresses.size() << std::endl;
    std::cout << "Bank switches detected: " << result.bank_tracker.switches().size() << std::endl;
    std::cout << "Cross-bank calls tracked: " << result.bank_tracker.calls().size() << std::endl;
    
    std::cout << "\nFunctions found:" << std::endl;
    for (const auto& [addr, func] : result.functions) {
        std::cout << "  " << func.name << " @ ";
        if (func.bank > 0) {
            std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)func.bank << ":";
        }
        std::cout << std::hex << std::setfill('0') << std::setw(4) << func.entry_address << std::endl;
    }
}

bool is_likely_data(const AnalysisResult& result, uint8_t bank, uint16_t address) {
    uint32_t full_addr = AnalysisResult::make_addr(bank, address);
    return result.addr_to_index.find(full_addr) == result.addr_to_index.end();
}

} // namespace gbrecomp
