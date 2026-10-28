#pragma once
#include "m0boy/system.h"
#include <filesystem>
#include <string>
#include <vector>
namespace m0boy::vectrex {
struct GenerateOptions {
  TargetId target = TargetId::pc;
  std::string name;
  std::filesystem::path output;
  std::filesystem::path targets_dir; /* targets/ of the checkout */
  std::string compiler = "gcc";
  /* the system ROM (4 or 8 KiB): built into a console firmware; empty, a
   * file named *bios* of that size next to the cartridge */
  std::filesystem::path bios;
  bool full_boot = false; /* keep the BIOS's intro (console targets) */
  /* the 6809 recompiled to C (emitter.cpp) in front of the run loop;
   * interpret: the run loop alone */
  bool interpret = false;
  bool precise = false; /* the slice ends on the interpreter's instruction */
  bool sweep = false;   /* --vectrex-sweep: the walk also seeds from the JSR/JMP extended targets */
  std::vector<std::filesystem::path> coverage; /* the host's --coverage files */
  size_t max_instructions = 0;
  /* the console's flash for the recompiled code (0: what the firmware
   * leaves), and its cost per instruction (0: the measured average) */
  size_t code_budget_bytes = 0;
  unsigned bytes_per_instruction = 0;
};
struct GenerateStats {
  size_t compiled = 0, covered = 0, regions = 0, entries = 0;
  bool bios_compiled = false;
};
bool generate(const std::filesystem::path &cartridge, const GenerateOptions &,
              GenerateStats &, std::string &error);
} // namespace m0boy::vectrex
