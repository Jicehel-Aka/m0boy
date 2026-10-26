#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace m0boy::megadrive {
struct EmitStats { size_t compiled = 0, regions = 0, entries = 0, idle_loops = 0; };
/* 68000 -> C for the PC runtime (md_full_code.c): the instructions reached
 * from `seeds` (vectors and the coverage profile) through ROM, grouped into
 * regions behind md_full_step_native. */
std::string emit_m68k(const std::vector<uint8_t> &rom, const std::set<uint32_t> &seeds, EmitStats &stats);
}
