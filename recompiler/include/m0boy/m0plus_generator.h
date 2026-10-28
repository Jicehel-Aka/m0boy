#pragma once

#include "m0boy/system.h"
#include <filesystem>
#include <string>
#include <vector>

namespace m0boy::m0plus {
// Package a validated firmware, the integrated board emulator and the
// experimental CPU-only native translation harness for PC.
bool generate(const std::filesystem::path& firmware, SystemId system,
              const std::filesystem::path& output, std::string& error,
              const std::filesystem::path& runtime_dir = {},
              const std::vector<std::filesystem::path>& coverage = {},
              bool aka = false);
}
