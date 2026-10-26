#pragma once
#include "m0boy/system.h"
#include <filesystem>
#include <string>
#include <vector>

namespace m0boy::megadrive {
struct GenerateOptions {
    TargetId target = TargetId::desktop;
    std::string name;
    std::filesystem::path output;
    std::filesystem::path targets_dir;
    std::string compiler = "gcc";
    std::vector<std::filesystem::path> coverage;
};
bool generate(const std::filesystem::path &rom, const GenerateOptions &, std::string &error);
}
