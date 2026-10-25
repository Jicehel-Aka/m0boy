/* PC Engine project generation: the interpreter runtime and the HuCard
 * image embedded as data, the desktop harness (the consoles follow once
 * the picture's budget says where). */
#pragma once

#include "m0boy/target_info.h"
#include <filesystem>
#include <string>

namespace m0boy::pce {

struct GenerateOptions {
    TargetId target = TargetId::desktop;
    std::string name;             /* project prefix, file-safe */
    std::filesystem::path output; /* project directory */
    std::filesystem::path meta_templates; /* targets/meta (siblings: consoles/, common/) */
    std::string compiler;         /* the firmware compiler's path (compile.sh) */
};

bool generate(const std::filesystem::path& cartridge, const GenerateOptions& o, std::string& error);

} // namespace m0boy::pce
