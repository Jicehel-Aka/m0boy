/* SNES project generation: the recompiled 65C816 code (or the interpreter
 * alone) and the cartridge image embedded as data, the desktop harness
 * (the picture's PPU/APU work comes later — the consoles wait for it). */
#pragma once

#include "m0boy/target_info.h"
#include <filesystem>
#include <string>
#include <vector>

namespace m0boy::snes {

struct GenerateOptions {
    TargetId target = TargetId::desktop;
    std::string name;             /* project prefix, file-safe */
    std::filesystem::path output; /* project directory */
    std::filesystem::path meta_templates; /* targets/meta (siblings: targets/) */
    std::string compiler;         /* the firmware compiler's path (compile.sh) */
    std::vector<std::filesystem::path> coverage; /* the harness's profile files (--coverage, repeatable) */
    bool precise = false;         /* --meta-precise: the slice ends on the interpreter's instruction */
    bool interpret = false;       /* --nes-interp: no recompiled code, the interpreter only */
    size_t code_budget_bytes = 0; /* the recompiled code's flash budget (0: all of it) */
    unsigned bytes_per_instruction = 0; /* the budget's cost estimate, default 24 */
};

struct GenerateStats {
    size_t compiled = 0, covered = 0, regions = 0, entries = 0;
};

bool generate(const std::filesystem::path& cartridge, const GenerateOptions& o,
              GenerateStats& stats, std::string& error);

} // namespace m0boy::snes
