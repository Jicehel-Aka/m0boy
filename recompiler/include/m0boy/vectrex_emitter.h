/* 6809 -> C for the Vectrex runtime (targets/vectrex/runtime/vx_gen.h). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace m0boy::vectrex {

/* One line of a coverage file the host writes (--interp --coverage f):
 * an address and how many instructions started there. */
struct CoverageEntry {
    uint16_t pc = 0;
    uint32_t hits = 0;
};

struct EmitOptions {
    /* the slice ends after the same instruction as the interpreter's (a
     * check after every instruction, every instruction an entry) */
    bool precise = false;
    size_t max_instructions = 0; /* 0: every reachable instruction */
    bool sweep = false;          /* --vectrex-sweep: the walk also seeds from the JSR/JMP extended targets */
    unsigned check_cycles = 64;  /* fast mode: the most cycles between two checks */
};

struct EmitStats {
    size_t compiled = 0, covered = 0, regions = 0, entries = 0;
};

/* `bios`: the 8 KiB at E000 when known (its code is compiled and its bytes
 * folded), else empty: only the cartridge is compiled. */
std::string emit_code(const std::vector<uint8_t>& cart, const std::vector<uint8_t>& bios,
                      const std::vector<CoverageEntry>& coverage, const EmitOptions& options,
                      EmitStats& stats);

/* the checksum vx_bios_sum() computes on the device */
uint32_t bios_sum(const std::vector<uint8_t>& bios);

} // namespace m0boy::vectrex
