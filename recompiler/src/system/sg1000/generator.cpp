/* SG-1000 project generation: the recompiled code (emitter.cpp), the
 * cartridge image, the runtime and, for META/Pokitto, the console drivers
 * shared with the Game Boy frontend. */
#include "m0boy/sg1000_generator.h"
#include "m0boy/sg1000.h"
#include "m0boy/sg1000_emitter.h"
#include <algorithm>
#include <fstream>
#include <map>
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
 * the profiled run wrote to 8000-BFFF (enabled it, Game Gear); "# entry
 * XXXX": a dynamic jump target.  Game Gear locations are page << 16 | pc. */
std::vector<Loc> read_coverage(const std::vector<fs::path>& files, bool gamegear, bool sms, bool& cart_ram,
                               std::vector<Loc>& entries, std::map<uint32_t, uint32_t>& reads) {
    std::vector<Loc> hot;
    std::set<Loc> seen;
    const unsigned long limit = gamegear ? 0xffffff : 0xffff;
    const std::string header = sms ? "# m0boy.sms.coverage.v1" : gamegear ? "# m0boy.gg.coverage.v1" : "# m0boy.sg1000.coverage.v1";
    for (const auto& path : files) {
        std::ifstream file(path);
        std::string line;
        if (!file || !std::getline(file, line) || line != header)
            throw std::runtime_error("Expected " + header.substr(2) + ": " + path.string());
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (line == "# cart-ram") { cart_ram = true; continue; }
            if (line.rfind("# entry ", 0) == 0) { /* target of a RET or JP (HL) */
                size_t used = 0;
                const unsigned long value = std::stoul(line.substr(8), &used, 16);
                if (used != line.size() - 8 || value > limit) throw std::runtime_error("Invalid coverage entry");
                entries.push_back(static_cast<Loc>(value));
                continue;
            }
            if (line.rfind("# read ", 0) == 0) { /* ROM sector read as data: frames */
                std::istringstream in(line.substr(7));
                uint32_t sector = 0, frames = 0;
                if (in >> std::hex >> sector >> std::dec >> frames) reads[sector] += frames;
                continue;
            }
            if (line[0] == '#') continue;
            size_t used = 0;
            unsigned long value = std::stoul(line, &used, 16);
            if (used != line.size() || value > limit) throw std::runtime_error("Invalid coverage address");
            if (seen.insert(static_cast<Loc>(value)).second) hot.push_back(static_cast<Loc>(value));
        }
        if (!file.eof()) throw std::runtime_error("Cannot read coverage");
    }
    return hot;
}

/* Flash for generated code: the application area, less the runtime and
 * drivers (measured at about 24 KiB) and the ROM image. */
constexpr size_t runtime_bytes = 24 * 1024;
constexpr size_t bytes_per_instruction = 32; /* measured 13-27 with GCC 7.2 -O2 */
constexpr size_t gg_bytes_per_instruction = 36; /* Game Gear: paged reads, measured 28-33 */

/* ROM file name on the card (8.3): project name, letters and digits */
std::string stream_name(const std::string& project) {
    std::string n;
    for (char c : project) {
        if (std::isalnum(static_cast<unsigned char>(c))) n += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (n.size() == 8) break;
    }
    return n.empty() ? "ROM" : n;
}

} // namespace

bool generate(const fs::path& path, const GenerateOptions& o, GenerateStats& stats, std::string& error) {
    error.clear();
    const bool gg = o.gamegear;
    std::vector<uint8_t> rom;
    if (gg) { /* whole 16 KiB pages, padded with FF */
        std::ifstream file(path, std::ios::binary);
        if (!file) { error = "Cannot read Game Gear cartridge: " + path.string(); return false; }
        rom.assign(std::istreambuf_iterator<char>(file), {});
        if (o.mastersystem && (rom.size() & 0x3fff) == 512) rom.erase(rom.begin(), rom.begin() + 512); /* copier header */
        if (rom.size() < 0x400 || rom.size() > 0x400000) { error = "Game Gear cartridge must contain 1 KiB to 4 MiB"; return false; }
        rom.resize((rom.size() + 0x3fff) & ~size_t(0x3fff), 0xff);
    } else {
        auto cartridge = Cartridge::load(path, error);
        if (!cartridge) return false;
        rom.assign(cartridge->bytes().begin(), cartridge->bytes().end());
    }
    const std::string system_name = o.mastersystem ? "Master System" : gg ? "Game Gear" : "SG-1000";
    try {
        const auto shared = o.meta_templates.parent_path() / "sg1000";
        const auto tpl = o.meta_templates.parent_path() / (gg ? "gamegear" : "sg1000");
        if (!fs::is_regular_file(tpl / "runtime" / "sg_gen.h"))
            throw std::runtime_error(system_name + " templates missing next to targets/meta");
        bool cart_ram = false;
        std::vector<Loc> entries;
        std::map<uint32_t, uint32_t> reads;
        const auto hot = read_coverage(o.coverage, gg, o.mastersystem, cart_ram, entries, reads);
        const size_t rom_size = rom.size();
        /* consoles: no room for 8 KiB more, the RAM lives in a save file */
        const bool cart_on_card = gg && cart_ram && o.target != TargetId::desktop;
        if (!gg) cart_ram = cart_ram && rom_size <= 0x8000; /* 40/48 KiB boards: ROM at 8000 */

        // compile.sh writes its log into the project directory before generation.
        auto foreign = [&] {
            for (const auto& e : fs::directory_iterator(o.output))
                if (e.path().filename() != "compile.log") return true;
            return false;
        };
        const auto marker = o.output / "sg1000-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && foreign())
            throw std::runtime_error("Output directory is not an " + system_name + " project; choose a fresh directory");
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
        eo.entries.assign(entries.begin(), entries.end());
        /* Codemasters boards: checksum and its complement at 7FE6/7FE8 */
        bool codemasters = false;
        if (gg && rom_size >= 0x8000) {
            const unsigned sum = rom[0x7fe6] | rom[0x7fe7] << 8, inverse = rom[0x7fe8] | rom[0x7fe9] << 8;
            codemasters = sum && !((sum + inverse) & 0xffff);
        }
        if (gg) { eo.banked = true; eo.fixed_kib = !codemasters; eo.io_out = "gg_out"; eo.io_in = "gg_in"; }
        size_t budget = 0;
        /* Game Gear ROMs larger than the flash leave some pages on the card:
         * the code gets the flash its played instructions need (at most
         * half), the pages most read as data fill the rest; page 0 (the
         * fixed KiB) always stays. */
        std::vector<int> page_index; /* flash slot of each page, -1: card */
        const size_t pages = rom_size / 0x4000;
        const size_t per_instruction = o.bytes_per_instruction ? o.bytes_per_instruction
                                       : gg ? gg_bytes_per_instruction : bytes_per_instruction;
        if (o.target != TargetId::desktop) {
            const size_t area = o.target == TargetId::pokitto ? 0x39000 : 240 * 1024;
            const size_t free = area > runtime_bytes ? area - runtime_bytes : 0;
            const size_t code_need = std::min(hot.size() * per_instruction + 16 * 1024, free / 2);
            if (gg && rom_size + code_need > free) {
                std::vector<std::pair<uint64_t, size_t>> score;
                for (size_t p = 1; p < pages; p++) {
                    uint64_t sum = 0;
                    for (uint32_t s = static_cast<uint32_t>(p * 32); s < (p + 1) * 32; s++)
                        if (auto it = reads.find(s); it != reads.end()) sum += it->second;
                    score.push_back({ sum, p });
                }
                std::stable_sort(score.begin(), score.end(), [](auto& a, auto& b) { return a.first > b.first; });
                const size_t resident = std::max<size_t>(1, (free - code_need) / 0x4000);
                page_index.assign(pages, -1);
                page_index[0] = 0;
                int next = 1;
                for (size_t k = 0; k + 1 < resident && k < score.size(); k++) page_index[score[k].second] = next++;
                budget = free - static_cast<size_t>(next) * 0x4000;
            } else budget = free > rom_size ? free - rom_size : 0;
        }
        if (o.code_budget_bytes) budget = o.code_budget_bytes;
        const bool streamed = !page_index.empty();
        if (budget) eo.max_instructions = budget / per_instruction;
        EmitStats es;
        write(src / "sg_gen.c", emit_code(rom, hot, eo, es));
        stats.covered = es.covered;
        stats.compiled = es.compiled;
        stats.speculative = es.speculative;
        stats.regions = es.regions;
        stats.entries = es.entries;
        stats.cart_ram = cart_ram;

        const bool pow2 = rom_size && !(rom_size & (rom_size - 1));
        std::ostringstream config;
        if (gg) config << "/* Generated by m0recomp --system gg. */\n#pragma once\n"
               << "#define SG_ROM_SIZE " << rom_size << "u\n"
               << "#define SG_CART_RAM " << (cart_ram ? 1 : 0) << "\n"
               << "#define SG_PRECISE " << (o.precise ? 1 : 0) << "\n"
               << "#define GG_PANEL_SWAP " << (o.target == TargetId::meta ? 1 : 0) << " /* META SPI: high byte first */\n"
               << "#define SG_ROM_STREAM " << (streamed ? 1 : 0) << "\n"
               << "#define GG_MAPPER " << (codemasters ? 1 : 0) << " /* 0 Sega, 1 Codemasters */\n"
               << "#define GG_CART_SD " << (cart_on_card ? 1 : 0) << " /* cartridge RAM in the save file */\n"
               << "#define GG_SMS " << (o.mastersystem ? 1 : 0) << " /* Master System */\n";
        if (cart_on_card) {
            std::string n83 = stream_name(o.name);
            n83.resize(8, ' ');
            config << "#define GG_SAVE_NAME \"" << stream_name(o.name) << ".SAV\"\n"
                   << "#define GG_SAVE_NAME83 \"" << n83 << "SAV\"\n";
        }
        if (gg && streamed) {
            std::string n83 = stream_name(o.name);
            n83.resize(8, ' ');
            const std::string ext = o.mastersystem ? "SMS" : "GG";
            config << "#define GG_STREAM_NAME \"" << stream_name(o.name) << "." << ext << "\"\n"
                   << "#define GG_STREAM_NAME83 \"" << n83 << (o.mastersystem ? "SMS" : "GG ") << "\"\n";
        }
        else config << "/* Generated by m0recomp --system sg1000. */\n#pragma once\n"
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
        if (streamed) { /* the pages kept in flash, in flash slot order */
            std::vector<size_t> order(pages);
            for (size_t p = 0; p < pages; p++) if (page_index[p] >= 0) order[static_cast<size_t>(page_index[p])] = p;
            size_t kept = 0;
            for (int i : page_index) kept += i >= 0;
            for (size_t k = 0; k < kept; k++)
                for (size_t b = 0; b < 0x4000; b++) data << (count++ % 32 ? "" : "\n") << unsigned(rom[order[k] * 0x4000 + b]) << ",";
            data << "\n};\nconst size_t sg_rom_size = " << rom_size << ";\nconst uint8_t gg_page_index[" << pages << "] = {";
            for (size_t p = 0; p < pages; p++) data << (p ? "," : "") << (page_index[p] < 0 ? 255 : page_index[p]);
            data << "};\n";
            stats.streamed_pages = pages - kept;
        } else {
            for (auto byte : rom) data << (count++ % 32 ? "" : "\n") << unsigned(byte) << ",";
            data << "\n};\nconst size_t sg_rom_size = sizeof(sg_rom);\n";
        }
        write(src / "sg_rom.c", data.str());

        for (const auto& entry : fs::directory_iterator(tpl / "runtime"))
            if (entry.is_regular_file()) copy_template(entry.path(), src / entry.path().filename());
        if (gg) /* the Z80 core is the SG-1000 runtime's */
            for (const char* name : { "z80.c", "z80.h", "z80_ops.h" }) copy_template(shared / "runtime" / name, src / name);
        const std::string host_c = gg ? "gg_host.c" : "sg_host.c", main_cpp = gg ? "gg_main.cpp" : "sg_main.cpp";
        copy_template(tpl / "host" / host_c, o.output / "host" / host_c);
        write(o.output / "host" / "CMakeLists.txt", replace(read(tpl / "host" / "CMakeLists.txt.in"), "@NAME@", o.name));

        if (o.target != TargetId::desktop) {
            copy_template(tpl / "platform" / main_cpp, src / main_cpp);
            /* files of an earlier generation that this one does not use */
            for (const char* name : { "gg_stream.c", "meta_sd.c", "meta_sd.h", "meta_fat.c", "meta_fat.h", "meta_sd_bus.cpp", "pokitto_sd.c" })
                fs::remove(src / name);
            if (gg && (streamed || cart_on_card)) {
                copy_template(tpl / "platform" / "gg_stream.c", src / "gg_stream.c");
                for (const char* name : { "meta_sd.c", "meta_sd.h", "meta_fat.c", "meta_fat.h" })
                    copy_template(o.meta_templates / "platform" / "src" / name, src / name);
                const auto bus = o.target == TargetId::pokitto ? fs::path("pokitto/pokitto_sd.c") : fs::path("meta/meta_sd_bus.cpp");
                copy_template(o.meta_templates / "platform" / bus, src / bus.filename());
            }
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
            if (gg && o.target == TargetId::meta) audio = replace(audio, "#define RING 1024u", "#define RING 512u"); /* RAM: about 1.4 frames */
            write(src / audio_name, audio);
            std::string version = read(o.meta_templates / (o.target == TargetId::pokitto ? fs::path("platform/pokitto/VERSION") : fs::path("VERSION")));
            while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();
            write(src / "gbl_config.h", "#pragma once\n#define GBL_TARGET_POKITTO " + std::to_string(o.target == TargetId::pokitto) +
                                        "\n#define SG_GEN_VERSION \"" + version + "\"\n");
            write(src / "gbl_profile.h", "#pragma once\n#define META_LCD_DMA 1\n#define META_LCD_12BPP 0\n#define META_LCD_SPI_MHZ 24\n"
                                         "#define POKITTO_LCD_ASM " + std::string(gg ? "1" : "0") + "\n#define POKITTO_RAM_BANKS " + std::string(gg ? "1" : "0") + "\n#define POKITTO_AUDIO_ASM 0\n#define META_AUDIO_DMA 0\n"
                                         "#define META_SD_SPI_HZ 12000000\n");
            if (o.target == TargetId::pokitto) {
                for (const char* name : { "pokitto_startup.c", "pokitto_clock.c", "pokitto_buttons.c", "pokitto_lcd.c", "pokitto_hw.h", "lpc11u6x.h" })
                    copy_template(platform / name, src / name);
                if (gg) copy_template(platform / "pokitto_lcd_asm.S", src / "pokitto_lcd_asm.S"); /* rows pushed in Thumb-1 */
                auto ld = replace(read(platform / "pokitto.ld.in"), "@NAME@", o.name);
                if (gg) /* the 2 KiB USB RAM bank: pair tables and the card cache */
                    ld = replace(ld, "*(.bss.gbl_lookup_cache)", "*(.bss.gbl_lookup_cache)\n    *(.bss.pair53)\n    *(.bss.card_buf)");
                write(o.output / "pokitto.ld", ld);
                copy_template(shared / "checksum.py", o.output / "checksum.py");
                copy_template(shared / "arm-toolchain.cmake", o.output / "arm-toolchain.cmake");
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
              "# " + system_name + " project: " + o.name + "\n\nRecompiled Z80 code (regions of C) with interpreter fallback.\n\n" +
              "Source: " + system_name + ". Destination: " + target + ". Compiled instructions: " + std::to_string(es.compiled) +
              " (" + std::to_string(es.covered) + " played), regions: " + std::to_string(es.regions) + ".\n\n" +
              "Host: cmake -G Ninja -S host -B host/build && ninja -C host/build\n" +
              "Run: host/build/" + o.name + "_host --frames 300 [--interp] [--shot image.ppm]\n\n" +
              (o.target == TargetId::pokitto ? "Firmware: cmake -G Ninja -S . -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake && ninja -C build\n"
               : o.target == TargetId::meta ? "Firmware: pio run\n" : "") +
              (gg ? std::string("\nSega mapper (16 KiB pages), 8 KiB RAM, VDP mode 4 (160x144 window), SN76489.\n")
                  : std::string("\nBaseline NTSC, unbanked cartridge (optional 8 KiB cartridge RAM), TMS9918A, SN76489.\n")));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace m0boy::sg1000
