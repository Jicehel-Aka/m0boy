/* HuC6280 -> C for the PCE runtime (targets/pce/runtime/pce_gen.h). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace m0boy::pce {

/* One line of a coverage file the harness writes (--coverage): a logical
 * address, its executions, the MPR contexts it ran under (the harness
 * records the eight MPR bytes of every traced instruction, up to four per
 * address — bank switching gives one address several), and whether a
 * dynamic transfer (RTS, RTI, JMP (ind), JMP (abs,X), an interrupt)
 * landed on it.  Older files carry no context: their addresses rank and
 * label whatever instances the static walk found. */
struct CoverageEntry {
    uint16_t pc = 0;
    uint32_t hits = 0;
    bool dynamic = false;
    std::vector<uint64_t> contexts; /* packed MPR bytes, MPR0 lowest */
};

struct EmitOptions {
    uint32_t rom_bytes = 0x8000; /* padded to the MPR's 8 KiB pages */
    bool precise = false;        /* the slice ends after the same instruction as the interpreter's */
    size_t max_instructions = 0; /* 0: every reachable instruction */
    unsigned check_cycles = 48;  /* fast mode: the most cycles between two budget checks */
    bool sweep = false;          /* the card swept for JSR/JMP abs targets the walk cannot reach */
};

struct EmitStats {
    size_t compiled = 0, covered = 0, regions = 0, entries = 0;
};

std::string emit_code(const std::vector<uint8_t>& rom, const std::vector<CoverageEntry>& coverage,
                      const EmitOptions& options, EmitStats& stats);

} // namespace m0boy::pce
