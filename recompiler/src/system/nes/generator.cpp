/* NES project generation: the interpreter runtime and the cartridge as
 * data (nes_prg[], nes_chr[]), the host harness or the console drivers
 * shared with the other frontends (the PICO-8 generator's plan). */
#include "m0boy/nes_generator.h"
#include "m0boy/nes_emitter.h"
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

std::string read(const fs::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot read template: " + path.string());
    return { std::istreambuf_iterator<char>(file), {} };
}
void write(const fs::path& path, const std::string& data) {
    std::ofstream file(path);
    if (!file || !(file << data)) throw std::runtime_error("Cannot write generated file: " + path.string());
}
void copy_template(const fs::path& from, const fs::path& to) {
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}
std::string replace(std::string s, const std::string& what, const std::string& value) {
    size_t pos = 0;
    while ((pos = s.find(what, pos)) != std::string::npos) {
        s.replace(pos, what.size(), value);
        pos += value.size();
    }
    return s;
}
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
        if (o.target != TargetId::meta && o.target != TargetId::desktop)
            throw std::runtime_error("NES projects target the META (--target meta) or the desktop harness");
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

        const bool esp = false; /* no ESPboy pipeline for the NES yet */
        const bool pokitto = false;
        const auto marker = o.output / "nes-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output)) {
            bool foreign = false; /* compile.sh writes its log into the project directory before generation */
            for (const auto& e : fs::directory_iterator(o.output)) foreign = foreign || e.path().filename() != "compile.log";
            if (foreign) throw std::runtime_error("Output directory is not a NES project; choose a fresh directory");
        }
        const std::string target = o.target == TargetId::meta ? "meta" : "desktop";
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
        write(src / "nes_config.h", config.str());

        for (const char* name : { "6502.c", "6502.h", "nes_native.h", "ppu.c", "ppu.h", "apu.c", "apu.h",
                                  "nes.c", "nes.h", "nes_gen.h" })
            copy_template(tpl / "runtime" / name, src / name);

        /* the recompiled 6502 code: the vectors, the profile, what they
         * reach; on a console, as much as the flash budget holds, played
         * code first */
        const bool native = !o.interpret;
        if (native) {
            EmitOptions emit;
            emit.prg_size = prg_size;
            emit.precise = o.precise;
            const unsigned bpi = o.bytes_per_instruction ? o.bytes_per_instruction : 24u;
            /* the META's 256 KiB: the bootloader's 16, the runtime and the
             * drivers, the cartridge (PRG, CHR) and the lookup tables */
            const size_t budget = o.code_budget_bytes ? o.code_budget_bytes
                                : o.target == TargetId::desktop ? 0
                                : size_t(256 - 16 - 58) * 1024 - prg_size - chr_size;
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
              "target_compile_definitions(" + o.name + "_host PRIVATE NES_EMBEDDED NES_COVERAGE" +
              (native ? " NES_NATIVE" : "") + ")\n"
              "target_compile_options(" + o.name + "_host PRIVATE -w)\ntarget_link_libraries(" + o.name + "_host PRIVATE m)\n");

        if (o.target == TargetId::meta) {
            copy_template(tpl / "platform" / "nes_main.cpp", src / "nes_main.cpp");
            copy_template(common_dir / "platform" / "sg_frontend.h", src / "sg_frontend.h");
            copy_template(common_dir / "platform" / "sg_menu.h", src / "sg_menu.h");
            const auto platform = o.meta_templates.parent_path() / "consoles" / "meta";
            const auto shared = o.meta_templates.parent_path() / "common" / "platform";
            for (const char* name : { "m0_target.h", "m0_lcd.h", "m0_clock.h", "m0_pad.h" })
                copy_template(shared / name, src / name);
            copy_template(common_dir / "runtime" / "m0_scale.h", src / "m0_scale.h");
            copy_template(shared / "m0_menu.h", src / "m0_menu.h");
            copy_template(shared / "m0_font.h", src / "m0_font.h");
            copy_template(shared / "m0_menu.cpp", src / "m0_menu.cpp");
            copy_template(o.meta_templates / "runtime" / "gbl_board.h", src / "gbl_board.h");
            for (const char* name : { "m0_sd.c", "m0_sd.h", "m0_fat.c", "m0_fat.h" })
                copy_template(shared / name, src / name);
            copy_template(shared / "m0_card.h", src / "m0_card.h");
            copy_template(platform / "meta_sd_bus.cpp", src / "meta_sd_bus.cpp");
            /* the audio driver: the frame's samples are 29781 CPU cycles at
             * 1789773 Hz (m0_audio_frame_samples' DMG constants) */
            auto audio_header = read(shared / "m0_audio.h");
            audio_header = replace(replace(audio_header, "70224u", "29781u"), "4194304u", "1789773u");
            write(src / "m0_audio.h", audio_header);
            auto audio = read(platform / "meta_audio.cpp");
            audio = replace(audio, "#include \"gbl.h\"", "#include \"nes.h\"");
            audio = replace(replace(audio, "gbl_apu_render", "nes_audio_render"), "gbl_apu_set_rate", "nes_audio_set_rate");
            write(src / "meta_audio.cpp", audio);
            write(src / "gbl_config.h", std::string("#pragma once\n#define GBL_TARGET_POKITTO 0\n#define GBL_GEN_VERSION \"") + GBRECOMP_META_VERSION + "\"\n");
            write(src / "gbl_profile.h",
                  "#pragma once\n#include \"gbl_board.h\"\n"
                  "#ifndef META_OVERCLOCK\n#define META_OVERCLOCK 1\n#endif\n"
                  "#define POKITTO_LCD_ASM 0\n#define POKITTO_RAM_BANKS 0\n#define POKITTO_AUDIO_ASM 0\n"
                  "#ifndef META_AUDIO_DMA\n#define META_AUDIO_DMA 0\n#endif\n"
                  "#ifndef META_AUDIO_RING\n#define META_AUDIO_RING 512u\n#endif\n");
            for (const char* name : { "meta_startup.c", "meta_clock.cpp", "meta_lcd.cpp", "meta_lcd_pack.h", "meta_lcd_queue.h",
                                      "meta_dma.h", "meta_dma.cpp", "meta_spi.c", "meta_hw.h", "meta_usb.c", "meta_usb.h",
                                      "meta_usb_msc.c", "meta_usb_msc.h" })
                copy_template(platform / name, src / name);
            /* the picture's columns are sampled 256 -> 160: the panel's full
             * width, the meta_lcd.cpp column window patch is not needed */
            auto ini = replace(replace(read(platform / "platformio.ini.in"), "@NAME@", o.name), "@COMPILER@", o.compiler);
            if (native) ini = replace(ini, "build_flags = ", "build_flags = -DNES_NATIVE ");
            write(o.output / "platformio.ini", ini);
            write(o.output / "gamebuino_meta.ld", replace(read(platform / "gamebuino_meta.ld.in"), "@NAME@", o.name));
            fs::create_directories(o.output / "boards");
            copy_template(platform / "boards" / "gamebuino_meta.json", o.output / "boards" / "gamebuino_meta.json");
            fs::create_directories(o.output / "toolchain");
            for (const auto& entry : fs::directory_iterator(common_dir / "toolchain"))
                if (entry.is_regular_file()) copy_template(entry.path(), o.output / "toolchain" / entry.path().filename());
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
              (o.target == TargetId::meta ? "Firmware: pio run  (flash with pio run -t upload)\n" : ""));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace m0boy::nes
