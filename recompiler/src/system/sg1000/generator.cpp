/* SG-1000 project generation: the recompiled code (emitter.cpp), the
 * cartridge image, the runtime and, for META/Pokitto, the console drivers
 * shared with the Game Boy frontend. */
#include "m0boy/sg1000_generator.h"
#include "m0boy/sg1000.h"
#include "m0boy/sg1000_emitter.h"
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace m0boy::sg1000 {
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

/* Hottest first, across files (first occurrence wins); "# cart-ram" when
 * the profiled run wrote to 8000-BFFF; "# entry XXXX": a dynamic jump target. */
std::vector<uint16_t> read_coverage(const std::vector<fs::path>& files, bool& cart_ram, std::vector<uint16_t>& entries) {
    std::vector<uint16_t> hot;
    std::set<uint16_t> seen;
    for (const auto& path : files) {
        std::ifstream file(path);
        std::string line;
        if (!file || !std::getline(file, line) || line != "# m0boy.sg1000.coverage.v1")
            throw std::runtime_error("Expected SG-1000 coverage v1: " + path.string());
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (line == "# cart-ram") { cart_ram = true; continue; }
            if (line.rfind("# entry ", 0) == 0) { /* target of a RET or JP (HL) */
                size_t used = 0;
                const unsigned long value = std::stoul(line.substr(8), &used, 16);
                if (used != line.size() - 8 || value > 0xffff) throw std::runtime_error("Invalid SG-1000 coverage entry");
                entries.push_back(static_cast<uint16_t>(value));
                continue;
            }
            if (line[0] == '#') continue;
            size_t used = 0;
            unsigned long value = std::stoul(line, &used, 16);
            if (used != line.size() || value > 0xffff) throw std::runtime_error("Invalid SG-1000 coverage address");
            if (seen.insert(static_cast<uint16_t>(value)).second) hot.push_back(static_cast<uint16_t>(value));
        }
        if (!file.eof()) throw std::runtime_error("Cannot read SG-1000 coverage");
    }
    return hot;
}

/* Flash for generated code: the application area, less the runtime and
 * drivers (measured at about 24 KiB) and the ROM image. */
constexpr size_t runtime_bytes = 24 * 1024;
constexpr size_t bytes_per_instruction = 32; /* measured 13-27 with GCC 7.2 -O2 */

} // namespace

bool generate(const fs::path& path, const GenerateOptions& o, GenerateStats& stats, std::string& error) {
    error.clear();
    auto cartridge = Cartridge::load(path, error);
    if (!cartridge) return false;
    try {
        const auto tpl = o.meta_templates.parent_path() / "sg1000";
        if (!fs::is_regular_file(tpl / "runtime" / "sg_gen.h"))
            throw std::runtime_error("SG-1000 templates missing next to targets/meta");
        bool cart_ram = false;
        std::vector<uint16_t> entries;
        const auto hot = read_coverage(o.coverage, cart_ram, entries);
        const size_t rom_size = cartridge->bytes().size();
        cart_ram = cart_ram && rom_size <= 0x8000; /* 40/48 KiB boards: ROM at 8000 */

        // compile.sh writes its log into the project directory before generation.
        auto foreign = [&] {
            for (const auto& e : fs::directory_iterator(o.output))
                if (e.path().filename() != "compile.log") return true;
            return false;
        };
        const auto marker = o.output / "sg1000-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && foreign())
            throw std::runtime_error("Output directory is not an SG-1000 project; choose a fresh directory");
        // A project owns one platform and cannot silently mix drivers.
        const std::string target = o.target == TargetId::meta ? "meta" : o.target == TargetId::pokitto ? "pokitto" : "desktop";
        if (fs::exists(marker) && read(marker) != target + "\n")
            throw std::runtime_error("Use a separate output directory for each destination");
        fs::create_directories(o.output / "src");
        fs::create_directories(o.output / "host");
        const auto src = o.output / "src";

        EmitOptions eo;
        eo.precise = o.precise;
        eo.cart_ram = cart_ram;
        eo.entries = entries;
        size_t budget = o.code_budget_bytes;
        if (!budget && o.target != TargetId::desktop) {
            const size_t area = o.target == TargetId::pokitto ? 0x39000 : 240 * 1024;
            budget = area > runtime_bytes + rom_size ? area - runtime_bytes - rom_size : 0;
        }
        if (budget) eo.max_instructions = budget / bytes_per_instruction;
        EmitStats es;
        write(src / "sg_gen.c", emit_code(*cartridge, hot, eo, es));
        stats.covered = es.covered;
        stats.compiled = es.compiled;
        stats.speculative = es.speculative;
        stats.regions = es.regions;
        stats.entries = es.entries;
        stats.cart_ram = cart_ram;

        const bool pow2 = rom_size && !(rom_size & (rom_size - 1));
        std::ostringstream config;
        config << "/* Generated by m0recomp --system sg1000. */\n#pragma once\n"
               << "#define SG_ROM_SIZE " << rom_size << "u\n"
               << "#define SG_ROM_MASK " << (pow2 ? rom_size - 1 : 0) << "u\n"
               << "#define SG_CART_RAM " << (cart_ram ? 1 : 0) << "\n"
               << "#define SG_LOW_RAM " << (cart_ram ? 1 : 0) << "\n"
               << "#define SG_PRECISE " << (o.precise ? 1 : 0) << "\n"
               << "#define SG_PANEL_SWAP " << (o.target == TargetId::meta ? 1 : 0) << " /* META SPI: high byte first */\n";
        write(src / "sg_config.h", config.str());
        std::ostringstream data;
        data << "#include <stdint.h>\n#include <stddef.h>\nconst uint8_t sg_rom[] = {";
        size_t count = 0;
        for (auto byte : cartridge->bytes()) data << (count++ % 32 ? "" : "\n") << unsigned(byte) << ",";
        data << "\n};\nconst size_t sg_rom_size = sizeof(sg_rom);\n";
        write(src / "sg_rom.c", data.str());

        for (const auto& entry : fs::directory_iterator(tpl / "runtime"))
            if (entry.is_regular_file()) copy_template(entry.path(), src / entry.path().filename());
        copy_template(tpl / "host" / "sg_host.c", o.output / "host" / "sg_host.c");
        write(o.output / "host" / "CMakeLists.txt", replace(read(tpl / "host" / "CMakeLists.txt.in"), "@NAME@", o.name));

        if (o.target != TargetId::desktop) {
            copy_template(tpl / "platform" / "sg_main.cpp", src / "sg_main.cpp");
            auto platform = o.meta_templates / "platform" / (o.target == TargetId::pokitto ? "pokitto" : "meta");
            for (const char* name : { "meta_target.h", "meta_lcd.h", "meta_clock.h" })
                copy_template(o.meta_templates / "platform" / "src" / name, src / name);
            copy_template(tpl / "platform" / "sg_audio.h", src / "sg_audio.h");
            auto audio_header = read(o.meta_templates / "platform/src/meta_audio.h");
            audio_header = replace(replace(audio_header, "70224u", "59736u"), "4194304u", "3579545u");
            write(src / "meta_audio.h", audio_header);
            const std::string audio_name = o.target == TargetId::pokitto ? "pokitto_audio.c" : "meta_audio.cpp";
            auto audio = read(platform / audio_name);
            audio = replace(audio, "#include \"gbl.h\"", "#include \"sg_audio.h\"");
            audio = replace(replace(audio, "gbl_apu_render", "sg_platform_audio_render"), "gbl_apu_set_rate", "sg_platform_audio_rate");
            write(src / audio_name, audio);
            std::string version = read(o.meta_templates / (o.target == TargetId::pokitto ? fs::path("platform/pokitto/VERSION") : fs::path("VERSION")));
            while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();
            write(src / "gbl_config.h", "#pragma once\n#define GBL_TARGET_POKITTO " + std::to_string(o.target == TargetId::pokitto) +
                                        "\n#define SG_GEN_VERSION \"" + version + "\"\n");
            write(src / "gbl_profile.h", "#pragma once\n#define META_LCD_DMA 1\n#define META_LCD_12BPP 0\n#define META_LCD_SPI_MHZ 24\n"
                                         "#define POKITTO_LCD_ASM 0\n#define POKITTO_RAM_BANKS 0\n#define POKITTO_AUDIO_ASM 0\n#define META_AUDIO_DMA 0\n");
            if (o.target == TargetId::pokitto) {
                for (const char* name : { "pokitto_startup.c", "pokitto_clock.c", "pokitto_buttons.c", "pokitto_lcd.c", "pokitto_hw.h", "lpc11u6x.h" })
                    copy_template(platform / name, src / name);
                write(o.output / "pokitto.ld", replace(read(platform / "pokitto.ld.in"), "@NAME@", o.name));
                copy_template(tpl / "checksum.py", o.output / "checksum.py");
                copy_template(tpl / "arm-toolchain.cmake", o.output / "arm-toolchain.cmake");
                fs::create_directories(o.output / "toolchain");
                for (const auto& entry : fs::directory_iterator(o.meta_templates / "toolchain"))
                    if (entry.is_regular_file()) copy_template(entry.path(), o.output / "toolchain" / entry.path().filename());
                write(o.output / "CMakeLists.txt", replace(replace(read(tpl / "CMakeLists.txt.in"), "@NAME@", o.name), "@COMPILER@", o.compiler));
            } else {
                for (const char* name : { "meta_startup.c", "meta_clock.cpp", "meta_lcd.cpp", "meta_lcd_pack.h", "meta_lcd_queue.h",
                                          "meta_dma.h", "meta_dma.cpp", "meta_spi.c", "meta_hw.h" })
                    copy_template(platform / name, src / name);
                write(o.output / "platformio.ini", replace(replace(read(platform / "platformio.ini.in"), "@NAME@", o.name), "@COMPILER@", o.compiler));
                write(o.output / "gamebuino_meta.ld", replace(read(platform / "gamebuino_meta.ld.in"), "@NAME@", o.name));
                fs::create_directories(o.output / "boards");
                copy_template(platform / "boards" / "gamebuino_meta.json", o.output / "boards" / "gamebuino_meta.json");
                fs::create_directories(o.output / "toolchain");
                for (const auto& entry : fs::directory_iterator(o.meta_templates / "toolchain"))
                    if (entry.is_regular_file()) copy_template(entry.path(), o.output / "toolchain" / entry.path().filename());
            }
        }
        write(marker, target + "\n");
        write(o.output / "README.md",
              "# SG-1000 project: " + o.name + "\n\nRecompiled Z80 code (regions of C) with interpreter fallback.\n\n" +
              "Source: SG-1000. Destination: " + target + ". Compiled instructions: " + std::to_string(es.compiled) +
              " (" + std::to_string(es.covered) + " played), regions: " + std::to_string(es.regions) + ".\n\n" +
              "Host: cmake -G Ninja -S host -B host/build && ninja -C host/build\n" +
              "Run: host/build/" + o.name + "_host --frames 300 [--interp] [--shot image.ppm]\n\n" +
              (o.target == TargetId::pokitto ? "Firmware: cmake -G Ninja -S . -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake && ninja -C build\n"
               : o.target == TargetId::meta ? "Firmware: pio run\n" : "") +
              "\nBaseline NTSC, unbanked cartridge (optional 8 KiB cartridge RAM), TMS9918A, SN76489.\n");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace m0boy::sg1000
