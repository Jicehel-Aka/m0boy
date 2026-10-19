#pragma once
#include "m0boy/system.h"
#include <filesystem>
#include <string>

namespace m0boy::pico8 {

struct GenerateOptions {
    std::filesystem::path output, meta_templates;
    std::string name;
    TargetId target;
    unsigned heap_bytes = 0;      ///< 0: the runtime's default
    std::string compiler = "gcc"; ///< firmware compiler: gcc or clang
};

// Compiles the cartridge's Lua to C (p8c) and writes a META firmware project
// or a host harness project.
bool generate(const std::filesystem::path& cartridge, const GenerateOptions&, std::string& error);

} // namespace m0boy::pico8
