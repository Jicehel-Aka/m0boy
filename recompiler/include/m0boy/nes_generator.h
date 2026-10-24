/* NES project generation: the interpreter runtime, the cartridge embedded
 * as data and, for the META, the console drivers shared with the other
 * frontends. */
#pragma once

#include "m0boy/target_info.h"
#include <filesystem>
#include <string>

namespace m0boy::nes {

struct GenerateOptions {
    TargetId target = TargetId::desktop;
    std::string name;             /* project prefix, file-safe */
    std::filesystem::path output; /* project directory */
    std::filesystem::path meta_templates; /* targets/meta (siblings: consoles/, common/) */
    std::string compiler;         /* the firmware compiler's path (compile.sh) */
};

bool generate(const std::filesystem::path& cartridge, const GenerateOptions& o, std::string& error);

} // namespace m0boy::nes
