/* PICO-8 project generation: p8c compiles the cartridge's Lua to C, the
 * runtime and, for the META, the console drivers shared with the other
 * frontends come along. */
#include "m0boy/pico8_generator.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef M0BOY_PICO8
extern "C" int p8c_main(int argc, char** argv);
#endif

namespace m0boy::pico8 {
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

} // namespace

bool generate(const fs::path& cartridge, const GenerateOptions& o, std::string& error) {
    error.clear();
#ifndef M0BOY_PICO8
    (void)cartridge; (void)o;
    error = "m0boy was built without PICO-8 support (set FEMTO8_DIR to a femto8 checkout)";
    return false;
#else
    try {
        if (o.target != TargetId::meta && o.target != TargetId::desktop && o.target != TargetId::espboy)
            throw std::runtime_error("PICO-8 projects target the META (--target meta), the ESPboy or the desktop harness");
        const bool esp = o.target == TargetId::espboy;
        const auto tpl = o.meta_templates.parent_path() / "pico8";
        const auto common_dir = o.meta_templates.parent_path() / "common";
        if (!fs::is_regular_file(tpl / "runtime" / "p8rt.c"))
            throw std::runtime_error("PICO-8 templates missing next to targets/meta");
        const auto marker = o.output / "pico8-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output)) {
            bool foreign = false; /* compile.sh writes its log into the project directory before generation */
            for (const auto& e : fs::directory_iterator(o.output)) foreign = foreign || e.path().filename() != "compile.log";
            if (foreign) throw std::runtime_error("Output directory is not a PICO-8 project; choose a fresh directory");
        }
        const std::string target = o.target == TargetId::meta ? "meta" : esp ? "espboy" : "desktop";
        if (fs::exists(marker) && read(marker) != target + "\n")
            throw std::runtime_error("Use a separate output directory for each destination");
        const auto src = o.output / "src";
        fs::create_directories(src);
        fs::create_directories(o.output / "host");

        /* the cartridge: p8_gen.c (the functions) and p8_cart.c (the data) */
        std::string cart = cartridge.string(), dir = src.string();
        char arg0[] = "p8c", arg_o[] = "-o";
        char* argv[] = { arg0, cart.data(), arg_o, dir.data(), nullptr };
        if (p8c_main(4, argv) != 0) throw std::runtime_error("p8c could not compile " + cartridge.string());

        for (const char* name : { "p8rt.c", "p8rt.h", "p8gfx.c", "p8gfx.h", "p8lib.c", "p8main.c", "p8main.h", "p8_audio.h",
                                  "p8_audio.c", "fix32.h", "trigtables.h", "p8_font.h", "p8_rom.h", "p8_natives.def", "p8_consts.def" })
            copy_template(tpl / "runtime" / name, src / name);
        std::ostringstream config;
        config << "#pragma once\n";
        /* what the 32 KiB of the SAMD21 leave once the screen (8 KiB), the drivers and a few KiB of C stack
         * are counted; the heap's block headers limit it to 32 KiB */
        config << "#define P8_HEAP_BYTES " << (o.heap_bytes ? o.heap_bytes : esp ? 28672u : 18432u) << "\n";
        write(src / "p8_config.h", config.str());

        copy_template(tpl / "host" / "p8_host.c", o.output / "host" / "p8_host.c");
        copy_template(common_dir / "host" / "m0_host_util.h", o.output / "host" / "m0_host_util.h");
        write(o.output / "host" / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(" + o.name + "_p8_host C)\nset(CMAKE_C_STANDARD 11)\n"
              "if(NOT CMAKE_BUILD_TYPE)\n    set(CMAKE_BUILD_TYPE Release)\nendif()\n"
              "set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)\n"
              "add_executable(" + o.name + "_host p8_host.c ${SRC}/p8rt.c ${SRC}/p8gfx.c ${SRC}/p8lib.c ${SRC}/p8main.c ${SRC}/p8_audio.c\n"
              "    ${SRC}/p8_gen.c ${SRC}/p8_cart.c)\n"
              "target_include_directories(" + o.name + "_host PRIVATE ${SRC})\n"
              "target_compile_definitions(" + o.name + "_host PRIVATE P8_HEAP_BYTES=65536)\n"
              "target_compile_options(" + o.name + "_host PRIVATE -w)\ntarget_link_libraries(" + o.name + "_host PRIVATE m)\n");

        if (o.target == TargetId::meta || esp) {
            copy_template(tpl / "platform" / "p8_main.cpp", src / "p8_main.cpp");
            copy_template(common_dir / "platform" / "sg_frontend.h", src / "sg_frontend.h");
            copy_template(common_dir / "platform" / "sg_menu.h", src / "sg_menu.h");
            const auto platform = o.meta_templates / "platform" / (esp ? "espboy" : "meta");
            for (const char* name : { "meta_target.h", "meta_lcd.h", "meta_clock.h" })
                copy_template(o.meta_templates / "platform" / "src" / name, src / name);
            /* the option menu (sg_menu.h runs it; meta_menu.cpp draws it) */
            copy_template(o.meta_templates / "platform" / "src" / "meta_menu.h", src / "meta_menu.h");
            copy_template(o.meta_templates / "platform" / "src" / "meta_font.h", src / "meta_font.h");
            copy_template(o.meta_templates / "platform" / "src" / "meta_menu.cpp", src / "meta_menu.cpp");
            copy_template(o.meta_templates / "runtime" / "gbl_board.h", src / "gbl_board.h");
            /* the audio driver counts frames of 1/30 s */
            auto audio_header = read(o.meta_templates / "platform/src/meta_audio.h");
            audio_header = replace(replace(audio_header, "70224u", "1u"), "4194304u", "30u");
            write(src / "meta_audio.h", audio_header);
            if (!esp) {
                auto audio = read(platform / "meta_audio.cpp");
                audio = replace(audio, "#include \"gbl.h\"", "#include \"p8_audio.h\"");
                audio = replace(replace(audio, "gbl_apu_render", "p8_audio_render"), "gbl_apu_set_rate", "p8_audio_set_rate");
                write(src / "meta_audio.cpp", audio);
            }
            write(src / "gbl_config.h", std::string("#pragma once\n#define GBL_TARGET_POKITTO 0\n") + (esp ? "#define GBL_TARGET_ESPBOY 1\n" : "") +
                                        "#define GBL_GEN_VERSION \"" + (esp ? GBRECOMP_ESPBOY_VERSION : GBRECOMP_META_VERSION) + "\"\n");
            write(src / "gbl_profile.h", "#pragma once\n#include \"gbl_board.h\"\n#ifndef META_OVERCLOCK\n#define META_OVERCLOCK 1\n#endif\n"
                                         "#define POKITTO_LCD_ASM 0\n#define POKITTO_RAM_BANKS 0\n#define POKITTO_AUDIO_ASM 0\n"
                                         "#ifndef META_AUDIO_DMA\n#define META_AUDIO_DMA 0\n#endif\n"
                                         "#ifndef META_AUDIO_RING\n#define META_AUDIO_RING 512u\n#endif\n");
            if (esp) {
                /* Arduino core start-up; the hardware adapter is the Sega frontends' (it finds
                 * its audio in sg_audio.h) with the picture's 128 columns taken as they are */
                write(src / "sg_audio.h", "#pragma once\n#include \"p8_audio.h\"\n#define sg_platform_audio_rate p8_audio_set_rate\n#define sg_platform_audio_render p8_audio_render\n");
                for (const char* name : { "espboy_hw.h" }) copy_template(platform / name, src / name);
                auto hw = read(platform / "espboy_hw.cpp");
                const std::string row = "line[x] = p[(x * 5u + 2u) / 4u];", frame = "uint8_t samples[192];";
                if (hw.find(row) == std::string::npos || hw.find(frame) == std::string::npos)
                    throw std::runtime_error("espboy_hw.cpp changed: the PICO-8 patch no longer applies");
                hw = replace(hw, row, "line[x] = p[x + 16]; /* the picture is 128 columns at 16 in a 160-wide row */");
                hw = replace(hw, frame, "uint8_t samples[400]; /* a 30 Hz frame at 11025 Hz */");
                write(src / "espboy_hw.cpp", hw);
                copy_template(o.meta_templates / "runtime" / "gbl_espboy.h", src / "gbl_espboy.h");
                for (const char* name : { "platformio.ini", "espboy.ld", "espboy_build.py", "espboy_size.py" })
                    write(o.output / name, replace(read(platform / (std::string(name) + ".in")), "@NAME@", o.name));
            } else {
                for (const char* name : { "meta_startup.c", "meta_clock.cpp", "meta_lcd_pack.h", "meta_lcd_queue.h",
                                          "meta_dma.h", "meta_dma.cpp", "meta_spi.c", "meta_hw.h", "meta_usb.c", "meta_usb.h" })
                    copy_template(platform / name, src / name);
                /* The picture is 128 pixels wide: the panel's column window is narrowed to it
                 * (meta_lcd_set_columns) so that a row is 256 bytes on the wire instead of 320. */
                {
                    auto lcd = read(platform / "meta_lcd.cpp");
                    const std::string row_bytes = "#define LCD_ROW_BYTES 320u";
                    const std::string caset = "spi_send(0); spi_send(0); spi_send(0); spi_send(159);";
                    if (lcd.find(caset) == std::string::npos || lcd.find("static void set_window(uint8_t row0, uint8_t row1) {") == std::string::npos)
                        throw std::runtime_error("meta_lcd.cpp changed: the PICO-8 column window patch no longer applies");
                    lcd = replace(lcd, caset, "spi_send(0); spi_send(lcd_x0); spi_send(0); spi_send(lcd_x1);");
                    const std::string anchor = "/* TXC only rises after a transmission";
                    if (lcd.find(anchor) == std::string::npos) throw std::runtime_error("meta_lcd.cpp changed: the PICO-8 column window patch no longer applies");
                    lcd = replace(lcd, anchor, "static uint8_t lcd_x0 = 0, lcd_x1 = 159; /* column window of the picture */\n"
                                               "static uint32_t lcd_row_bytes = LCD_ROW_BYTES;\n\n" + anchor);
                    lcd = replace(replace(lcd, "e->rows * LCD_ROW_BYTES", "e->rows * lcd_row_bytes"), "(uint32_t)rows * LCD_ROW_BYTES", "(uint32_t)rows * lcd_row_bytes");
                    lcd += "\n/* The picture's columns x0..x1 (the window is set again for the next strip). */\n"
                           "extern \"C\" void meta_lcd_set_columns(uint8_t x0, uint8_t x1) {\n"
                           "    lcd_x0 = x0;\n    lcd_x1 = x1;\n"
                           "    lcd_row_bytes = (uint32_t)(x1 - x0 + 1) * (LCD_ROW_BYTES / 160u);\n"
                           "    win_next = 0xFFFFu;\n}\n";
                    write(src / "meta_lcd.cpp", lcd);
                }
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
              "# PICO-8 project: " + o.name + "\n\nThe cartridge's Lua compiled to C (p8_gen.c) on the PICO-8 runtime.\n\n"
              "Host: cmake -G Ninja -S host -B host/build && ninja -C host/build\n"
              "Run: host/build/" + o.name + "_host --frames 300 [--input \"frame:mask:frames,...\"] [--shot image.ppm]\n\n" +
              (o.target == TargetId::meta ? "Firmware: pio run  (copy .pio/build/meta/firmware.bin to the SD card as " + o.name + ".bin)\n" : ""));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
#endif
}

} // namespace m0boy::pico8
