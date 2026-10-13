#pragma once
#include "m0boy/sg1000.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace m0boy::sg1000 {

struct EmitOptions {
    bool precise = false;       ///< sync before every instruction (differential tests)
    bool cart_ram = false;      ///< 8 KiB of cartridge RAM at 8000-BFFF
    size_t max_instructions = 6000; ///< flash budget, in instructions
    size_t max_region_instructions = 200;
};

struct EmitStats {
    size_t covered = 0, compiled = 0, speculative = 0, regions = 0, entries = 0;
    size_t native = 0, generic = 0;
};

/// Z80 -> C for the SG-1000 runtime (sg_gen.h): regions of native code for
/// the played (hot first) and statically reachable instructions.
std::string emit_code(const Cartridge& cartridge, const std::vector<uint16_t>& hot_first,
                      const EmitOptions& options, EmitStats& stats);

} // namespace m0boy::sg1000
