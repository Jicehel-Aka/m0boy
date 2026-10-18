#pragma once
#include "m0boy/sg1000.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace m0boy::sg1000 {

/// Code location: ROM page << 16 | Z80 address.  Unbanked boards (SG-1000)
/// and the fixed areas of banked ones use page 0, so a location is then
/// the address itself.
using Loc = uint32_t;

/// A played location with the executions the profile recorded: the emitter
/// seeds its worklist and picks mapper defaults hottest first.
struct HotLoc {
    Loc loc;
    uint32_t hits; ///< execution count, summed over the coverage sessions
};

struct EmitOptions {
    bool precise = false;       ///< sync before every instruction (differential tests)
    /// Without precise: also sync before an instruction once this many
    /// T-states have run since the last sync on the straight path (0: loop
    /// heads only), which bounds how late an interrupt is taken.
    unsigned sync_cycles = 40;
    bool cart_ram = false;      ///< 8 KiB of cartridge RAM at 8000-BFFF
    /// Sega mapper (Game Gear): 0000-03FF fixed, three 16 KiB slots paged
    /// by FFFD-FFFF, 8 KiB of RAM at C000-FFFF.
    bool banked = false;
    bool fixed_kib = true; ///< banked: 0000-03FF always page 0 (Sega); false: Codemasters
    std::string io_out = "sg1000_out", io_in = "sg1000_in";
    size_t max_instructions = 6000; ///< flash budget, in instructions
    size_t max_region_instructions = 200;
    std::vector<Loc> entries; ///< dynamic jump targets seen by profiling
};

struct EmitStats {
    size_t covered = 0, compiled = 0, speculative = 0, regions = 0, entries = 0;
    size_t native = 0, generic = 0;
};

/// Z80 -> C for the Sega Z80 runtimes (sg_gen.h): regions of native code
/// for the played (hot first) and statically reachable instructions.
std::string emit_code(std::span<const uint8_t> rom, const std::vector<HotLoc>& hot_first,
                      const EmitOptions& options, EmitStats& stats);

} // namespace m0boy::sg1000
