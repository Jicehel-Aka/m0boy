/* What the project generators of the non-Game Boy systems (SG-1000 and
 * its family, PICO-8, NES) write alike for a console: the shared frontend
 * (targets/common/platform), the audio driver renamed to the system's
 * renderer, gbl_config.h and gbl_profile.h, and each console's build
 * project (PlatformIO for the META, the shared CMake for the Pokitto).
 * Each generator keeps what is its own: its runtime, its card plan, its
 * source lists. */
#pragma once

#include "m0boy/target_info.h"
#include <filesystem>
#include <initializer_list>
#include <string>

namespace m0boy::project {

std::string read(const std::filesystem::path& path);
void write(const std::filesystem::path& path, const std::string& data);
void copy_template(const std::filesystem::path& from, const std::filesystem::path& to);
std::string replace(std::string s, const std::string& what, const std::string& value);

/* targets/ seen from a generator: its option points at targets/meta */
struct Templates {
    std::filesystem::path targets;
    static Templates from_meta(const std::filesystem::path& meta_templates) { return { meta_templates.parent_path() }; }
    std::filesystem::path common() const { return targets / "common"; }
    std::filesystem::path frontend() const { return targets / "common" / "platform"; }
    /* targets/consoles/<dir> of a console */
    std::filesystem::path console(TargetId target) const;
};

/* The frontend every console shares: the setup, pad and pacing macros
 * (sg_frontend.h), the option menu (sg_menu.h, m0_menu.*, m0_font.h), the
 * console facts (m0_target.h, m0_lcd.h, m0_clock.h, m0_pad.h), the rows'
 * way to the panel (m0_rows.h) and the
 * META's board settings (gbl_board.h). */
void copy_frontend(const Templates& t, const std::filesystem::path& src);

/* m0_audio.h with the system's frame (frame_cycles CPU cycles at cpu_hz),
 * then the console's audio driver (meta_audio.cpp, pokitto_audio.c; the
 * others have theirs elsewhere) with gbl.h and the Game Boy APU's two entry
 * points renamed to the system's. */
void write_audio(const Templates& t, TargetId target, const std::filesystem::path& src,
                 const std::string& frame_cycles, const std::string& cpu_hz,
                 const std::string& header, const std::string& render, const std::string& set_rate);

/* gbl_config.h: the console's define and the firmware's version stamp */
void write_config(TargetId target, const std::filesystem::path& src);

/* gbl_profile.h: gbl_board.h, META_OVERCLOCK (on by default on the META,
 * on the Pokitto with POKITTO_HC, off on the PC and ESP32), the Pokitto's
 * three switches, then the META's audio ring (ring_everywhere: its #ifndef
 * lines on every console, the PICO-8's choice; else META_AUDIO_DMA 0 off
 * the META). */
struct PokittoSwitches { bool lcd_asm = false, ram_banks = false, audio_asm = false; };
void write_profile(TargetId target, const std::filesystem::path& src, PokittoSwitches pokitto,
                   bool ring_everywhere = false);

/* toolchain/ (the clang wrapper) at the project's root */
void copy_toolchain(const Templates& t, const std::filesystem::path& output);

/* The META: its bare-metal drivers (meta_lcd.cpp unless the generator
 * writes its own), then platformio.ini (build_flags put first), the linker
 * script and the board. */
void copy_meta_drivers(const Templates& t, const std::filesystem::path& src, bool lcd = true);
void write_meta_project(const Templates& t, const std::filesystem::path& output, const std::string& name,
                        const std::string& compiler, const std::string& build_flags = "");

/* The Pokitto: its bare-metal drivers plus extra ones, then the firmware
 * project: the linker script (ld, already rendered), the LPC boot checksum,
 * the ARM toolchain and the shared CMake (targets/common/cmake) with the
 * system's sources and the platform's extra ones. */
void copy_pokitto_drivers(const Templates& t, const std::filesystem::path& src,
                          std::initializer_list<const char*> extra = {});
std::string pokitto_ld(const Templates& t, const std::string& name);
void write_pokitto_project(const Templates& t, const std::filesystem::path& output, const std::string& name,
                           const std::string& compiler, const std::string& system, const std::string& sources,
                           const std::string& platform_extra, const std::string& ld);

} // namespace m0boy::project
