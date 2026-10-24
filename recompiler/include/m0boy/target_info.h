#pragma once

#include <string_view>

#include "m0boy/system.h"

namespace m0boy::consoles {

/* The generator-facing side of each destination console, kept in one table
 * instead of one if-ladder per generator (Game Boy, Sega, PICO-8): where the
 * console's drivers and templates live under targets/, the config define the
 * firmwares compile with, the VERSION file that stamps them, and how the ROM
 * and the saves are stored.  Wiring a new console is one row here plus its
 * copy lists in the generators. */
enum class Storage {
    sd_card,          // m0_sd/m0_fat/m0_state on a real card
    flash_beside_app, // ROM image and saves in flash partitions (ESPboy)
    host_file,        // the SD protocol over a file image (pc)
};

struct TargetInfo {
    TargetId id;
    std::string_view console_dir;    // targets/consoles/<dir>; empty for desktop
    std::string_view config_define;  // written into gbl_config.h
    std::string_view version_define; // GBRECOMP_*_VERSION
    std::string_view version_file;   // relative to targets/
    Storage storage;
};

const TargetInfo* find(TargetId id);

// The console's version, from the VERSION file CMake compiles in.
std::string_view version(TargetId id);

} // namespace m0boy::consoles
