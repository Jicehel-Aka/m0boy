/* NES project generation: the runtime, the recompiled 6502 code and the
 * cartridge as data (nes_prg[], nes_chr[]), the host harness, and for a
 * console the frontend and drivers the other systems share
 * (console_project.h): the META and the Pokitto. */
#include "m0boy/nes_generator.h"
#include "m0boy/nes_emitter.h"
#include "m0boy/console_project.h"
#include <cctype>
#include <cstring>
#include <map>
#include <fstream>
#include <vector>
#include <sstream>
#include <stdexcept>

namespace m0boy::nes {
namespace fs = std::filesystem;
namespace {

using project::read;
using project::write;
using project::replace;
using project::copy_template;

/* the ROM as C data, 16 bytes a line */
std::string rom_data(const uint8_t* data, size_t n) {
    std::ostringstream out;
    for (size_t i = 0; i < n; i++) {
        char b[8];
        snprintf(b, sizeof b, "0x%02X,", data[i]);
        out << b;
        if ((i & 15) == 15 || i + 1 == n) out << "\n";
    }
    return out.str();
}

} // namespace

/* the harness's coverage files: "addr hits [d]" lines, summed */
std::vector<CoverageEntry> read_coverage(const std::vector<fs::path>& files) {
    std::map<uint16_t, CoverageEntry> sum;
    for (const auto& path : files) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("Cannot read coverage file: " + path.string());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string addr, flag;
            uint64_t hits = 0;
            if (!(fields >> addr >> hits)) throw std::runtime_error("Bad coverage line in " + path.string() + ": " + line);
            fields >> flag;
            const unsigned long pc = std::stoul(addr, nullptr, 16);
            if (pc < 0x8000 || pc > 0xFFFF) continue;
            auto& e = sum[uint16_t(pc)];
            e.pc = uint16_t(pc);
            e.hits = uint32_t(std::min<uint64_t>(uint64_t(e.hits) + hits, 0xFFFFFFFFu));
            e.dynamic = e.dynamic || flag == "d";
        }
    }
    std::vector<CoverageEntry> out;
    for (const auto& [pc, e] : sum) out.push_back(e);
    return out;
}

bool generate(const fs::path& cartridge, const GenerateOptions& o, GenerateStats& stats, std::string& error) {
    error.clear();
    try {
        if (o.target != TargetId::meta && o.target != TargetId::pokitto && o.target != TargetId::desktop)
            throw std::runtime_error("NES projects target the META, the Pokitto or the desktop harness");
        const bool pokitto = o.target == TargetId::pokitto;
        const auto tpl = o.meta_templates.parent_path() / "nes";
        const auto common_dir = o.meta_templates.parent_path() / "common";
        if (!fs::is_regular_file(tpl / "runtime" / "nes.c"))
            throw std::runtime_error("NES templates missing next to targets/meta");

        /* the cartridge: iNES header, PRG, CHR */
        std::ifstream file(cartridge, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot read " + cartridge.string());
        char header[16];
        file.read(header, 16);
        if (!file || std::memcmp(header, "NES\x1a", 4) != 0)
            throw std::runtime_error("Not an iNES image: " + cartridge.string());
        const unsigned prg_size = (unsigned char)header[4] * 16384u;
        const unsigned chr_size = (unsigned char)header[5] * 8192u;
        if (!prg_size || prg_size > 32768u)
            throw std::runtime_error("Only NROM cartridges (16 or 32 KiB of PRG) for now: " + cartridge.string());
        if (chr_size != 8192u)
            throw std::runtime_error("Only CHR-ROM cartridges (8 KiB) for now: " + cartridge.string());
        std::vector<uint8_t> prg(prg_size), chr(chr_size);
        file.read(reinterpret_cast<char*>(prg.data()), prg_size);
        if (file.gcount() != (std::streamsize)prg_size)
            throw std::runtime_error("Truncated cartridge: " + cartridge.string());
        file.read(reinterpret_cast<char*>(chr.data()), chr_size);
        if (file.gcount() != (std::streamsize)chr_size)
            throw std::runtime_error("Truncated cartridge: " + cartridge.string());

        const auto marker = o.output / "nes-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output)) {
            bool foreign = false; /* compile.sh writes its log into the project directory before generation */
            for (const auto& e : fs::directory_iterator(o.output)) foreign = foreign || e.path().filename() != "compile.log";
            if (foreign) throw std::runtime_error("Output directory is not a NES project; choose a fresh directory");
        }
        const std::string target = o.target == TargetId::meta ? "meta" : pokitto ? "pokitto" : "desktop";
        if (fs::exists(marker) && read(marker) != target + "\n")
            throw std::runtime_error("Use a separate output directory for each destination");
        const auto src = o.output / "src";
        fs::create_directories(src);
        fs::create_directories(o.output / "host");

        /* the cartridge data and the machine's facts */
        write(src / "nes_cart.c",
              "/* The cartridge, generated by gbrecomp (PRG then CHR). */\n"
              "#include <stdint.h>\n\n"
              "const uint8_t nes_prg[] = {\n" + rom_data(prg.data(), prg_size) + "};\n\n"
              "const uint8_t nes_chr[] = {\n" + rom_data(chr.data(), chr_size) + "};\n");
        std::ostringstream config;
        config << "#pragma once\n"
               << "#define NES_PRG_SIZE " << prg_size << "u\n"
               << "#define NES_CHR_SIZE " << chr_size << "u\n"
               << "#define NES_VERTICAL_MIRROR " << (header[6] & 1) << "\n"
               << "/* the panel's byte order: the META's SPI takes the high byte first */\n"
               << "#define NES_PANEL_SWAP " << (o.target == TargetId::meta ? 1 : 0) << "\n";
        /* the recompiled code's dispatcher (6502.c) */
        if (!o.interpret) config << "#ifndef NES_NATIVE\n#define NES_NATIVE 1\n#endif\n";
        /* the Pokitto runs the PPU's loops from SRAM (ppu_asm.S) */
        if (pokitto) config << "#ifndef NES_PPU_RAMFUNC\n#define NES_PPU_RAMFUNC 1\n#endif\n";
        write(src / "nes_config.h", config.str());

        for (const char* name : { "6502.c", "6502.h", "nes_native.h", "ppu.c", "ppu.h", "ppu_asm.S", "apu.c", "apu.h",
                                  "nes.c", "nes.h", "nes_gen.h" })
            copy_template(tpl / "runtime" / name, src / name);
        /* the panel renderer's box filter (ppu.c, and the harness's check) */
        copy_template(common_dir / "runtime" / "m0_scale.h", src / "m0_scale.h");
        /* the idle loops' count (nes_gen.h) */
        copy_template(common_dir / "runtime" / "m0_idle.h", src / "m0_idle.h");

        /* the recompiled 6502 code: the vectors, the profile, what they
         * reach; on a console, as much as the flash budget holds, played
         * code first */
        const bool native = !o.interpret;
        if (native) {
            EmitOptions emit;
            emit.prg_size = prg_size;
            emit.precise = o.precise;
            const unsigned bpi = o.bytes_per_instruction ? o.bytes_per_instruction : 24u;
            /* the META's 256 KiB less the bootloader's 16, the Pokitto's
             * 228 below its loader: the runtime and the drivers (66, the
             * panel renderer's loops included), the cartridge (PRG, CHR)
             * and the lookup tables */
            const size_t flash = pokitto ? 0x39000u / 1024u : 256 - 16;
            const size_t budget = o.code_budget_bytes ? o.code_budget_bytes
                                : o.target == TargetId::desktop ? 0
                                : (flash - 66) * 1024 - prg_size - chr_size;
            emit.max_instructions = budget ? budget / bpi : 0;
            EmitStats es;
            write(src / "nes_gen.c", emit_code(prg, read_coverage(o.coverage), emit, es));
            stats.compiled = es.compiled;
            stats.covered = es.covered;
            stats.regions = es.regions;
            stats.entries = es.entries;
        }

        /* the harness sits next to src/ in a generated project, not next to
         * ../runtime as in the checkout */
        write(o.output / "host" / "nes_host.c",
              replace(read(tpl / "host" / "nes_host.c"), "#include \"../runtime/nes.h\"", "#include \"../src/nes.h\""));
        write(o.output / "host" / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(" + o.name + "_nes_host C)\nset(CMAKE_C_STANDARD 11)\n"
              "if(NOT CMAKE_BUILD_TYPE)\n    set(CMAKE_BUILD_TYPE Release)\nendif()\n"
              "set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)\n"
              "add_executable(" + o.name + "_host nes_host.c ${SRC}/nes.c ${SRC}/6502.c ${SRC}/ppu.c ${SRC}/apu.c ${SRC}/nes_cart.c" +
              (native ? " ${SRC}/nes_gen.c" : "") + ")\n"
              "target_include_directories(" + o.name + "_host PRIVATE ${SRC})\n"
              "target_compile_definitions(" + o.name + "_host PRIVATE NES_EMBEDDED NES_COVERAGE NES_PPU_REFERENCE)\n"
              "target_compile_options(" + o.name + "_host PRIVATE -w)\ntarget_link_libraries(" + o.name + "_host PRIVATE m)\n");

        if (o.target != TargetId::desktop) {
            const auto t = project::Templates::from_meta(o.meta_templates);
            copy_template(tpl / "platform" / "nes_main.cpp", src / "nes_main.cpp");
            project::copy_frontend(t, src);
            /* the audio driver: the frame's samples are 29781 CPU cycles at
             * 1789773 Hz */
            project::write_audio(t, o.target, src, "29781u", "1789773u", "nes.h", "nes_audio_render", "nes_audio_set_rate");
            project::write_config(o.target, src);
            /* the Pokitto: rows pushed and the audio interrupt in Thumb-1
             * from SRAM, its 2 KiB banks in use (the Game Gear's choice) */
            project::write_profile(o.target, src, {pokitto, pokitto, pokitto});
            if (pokitto) {
                /* no card: pokitto_sd.c stays out (nes_main.cpp) */
                project::copy_pokitto_drivers(t, src, {"pokitto_lcd_asm.S", "pokitto_audio_asm.S"});
                const std::string sources = "${SRC}/nes.c ${SRC}/6502.c ${SRC}/ppu.c ${SRC}/ppu_asm.S ${SRC}/apu.c ${SRC}/nes_cart.c" +
                                            std::string(native ? " ${SRC}/nes_gen.c" : "") + " ${SRC}/nes_main.cpp ${SRC}/m0_menu.cpp";
                project::write_pokitto_project(t, o.output, o.name, o.compiler, "nes", sources, " ${SRC}/pokitto_*.S",
                                               project::pokitto_ld(t, o.name));
            } else {
                /* Every META firmware offers USB SD mode: the card's driver */
                for (const char* name : { "m0_sd.c", "m0_sd.h", "m0_fat.c", "m0_fat.h", "m0_card.h" })
                    copy_template(t.frontend() / name, src / name);
                copy_template(t.console(TargetId::meta) / "meta_sd_bus.cpp", src / "meta_sd_bus.cpp");
                /* the picture's columns are sampled 256 -> 160: the panel's
                 * full width, the meta_lcd.cpp column window patch is not
                 * needed */
                project::copy_meta_drivers(t, src);
                project::write_meta_project(t, o.output, o.name, o.compiler);
            }
        }
        write(marker, target + "\n");
        write(o.output / "README.md",
              "# NES project: " + o.name + "\n\n" +
              (native ? std::string("The cartridge's 6502 code recompiled to C (src/nes_gen.c) on the 2A03/2C02 runtime,\n"
                                    "the interpreter for the rest.  A profile: host/build/" + o.name + "_host --interp --coverage cov.txt\n"
                                    "(then gbrecomp --system nes ... --coverage cov.txt).\n\n")
                      : std::string("The cartridge interpreted on the 2A03/2C02 runtime.\n\n")) +
              "Host: cmake -G Ninja -S host -B host/build && ninja -C host/build\n"
              "Run: host/build/" + o.name + "_host --frames 300 [--ppm shot.ppm] [--input script.txt] [--interp] [--coverage f]\n\n" +
              (o.target == TargetId::meta ? "Firmware: pio run  (flash with pio run -t upload)\n"
               : pokitto ? "Firmware: cmake -G Ninja -S . -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake && ninja -C build\n" : ""));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace m0boy::nes
