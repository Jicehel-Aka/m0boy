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
    TargetId target;
    std::vector<std::filesystem::path> coverage;
    bool precise = false;          ///< sync before every instruction (differential tests)
    std::string compiler = "gcc";  ///< firmware compiler: gcc or clang
    size_t code_budget_bytes = 0;  ///< 0: the flash left by the runtime and the ROM
};

struct GenerateStats {
    size_t covered = 0, compiled = 0, speculative = 0, regions = 0, entries = 0;
    bool cart_ram = false;
};

bool generate(const std::filesystem::path& rom, const GenerateOptions&, GenerateStats&, std::string& error);

} // namespace m0boy::sg1000
