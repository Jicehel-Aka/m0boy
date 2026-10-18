#pragma once
#include "m0boy/system.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace m0boy::sg1000 {

struct GenerateOptions {
    std::filesystem::path output, meta_templates;
    std::string name;
    std::string sd_name; ///< 8.3 base of NAME.BIN, NAME.GG/.SMS, NAME.SAV; empty: derived from name
    TargetId target;
    std::vector<std::filesystem::path> coverage;
    bool precise = false;          ///< sync before every instruction (differential tests)
    unsigned sync_cycles = 40;     ///< EmitOptions::sync_cycles
    bool gamegear = false;         ///< Game Gear cartridge (Sega mapper, VDP mode 4)
    bool mastersystem = false;     ///< with gamegear: a Master System cartridge
    std::string compiler = "gcc";  ///< firmware compiler: gcc or clang
    size_t code_budget_bytes = 0;  ///< 0: the flash left by the runtime and the ROM
    size_t bytes_per_instruction = 0; ///< flash per compiled instruction; 0: the default estimate
};

struct GenerateStats {
    size_t covered = 0, compiled = 0, speculative = 0, regions = 0, entries = 0;
    bool cart_ram = false;
    size_t streamed_pages = 0; ///< Game Gear pages left on the SD card
    size_t pool_sectors = 0;   ///< Game Gear sectors of those pages kept in flash
    bool packed = false;      ///< all ROM data in flash, no ROM file required
    size_t packed_sectors = 0, pool_bytes = 0;
};

bool generate(const std::filesystem::path& rom, const GenerateOptions&, GenerateStats&, std::string& error);

} // namespace m0boy::sg1000
