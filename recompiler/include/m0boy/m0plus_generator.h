#pragma once

#include "m0boy/system.h"
#include <filesystem>
#include <string>
#include <vector>

namespace m0boy::m0plus {
// Package a validated firmware for the PC firmware runner. The board model
// lives in gm0, an independently licensed project, and is never copied here.
bool generate(const std::filesystem::path& firmware, SystemId system,
              const std::filesystem::path& output, std::string& error,
              const std::filesystem::path& runtime_dir = {},
              const std::vector<std::filesystem::path>& coverage = {});
}
