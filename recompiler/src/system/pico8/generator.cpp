/* PICO-8 project generation: p8c compiles the cartridge's Lua to C, the
 * runtime and, for the META, the console drivers shared with the other
 * frontends come along. */
#include "m0boy/pico8_generator.h"
#include <algorithm>
#include <cctype>
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
/* 8.3 base of the game's files on the card (the Sega generator's rule). */
std::string sd_base(const std::string& project) {
    std::string n;
    for (char c : project) {
        if (std::isalnum(static_cast<unsigned char>(c))) n += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (n.size() == 8) break;
    }
    return n.empty() ? "CART" : n;
}
std::string name83(std::string base, std::string ext) {
    base.resize(8, ' ');
    ext.resize(3, ' ');
    return base + ext;
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
        /* ESPboy: 6 KiB of continuation stack in .bss (the PICO-8 code needs about 1 KiB of C
         * stack, measured on the META) and 4 KiB left to the SDK's heap */
        const unsigned heap = o.heap_bytes ? o.heap_bytes : esp ? 22528u : 18432u;
        config << "#ifndef P8_HEAP_BYTES /* the host harness takes more: its values are twice as large */\n#define P8_HEAP_BYTES " << heap << "\n#endif\n";
        /* Save states (p8_state_io): the heap, the screen, the cartridge's globals (8 bytes each on the
         * consoles) and under 1 KiB of the rest; on the card next to the .BIN (NAME.ST8, made by
         * compile.sh), in the flash on the ESPboy (after nothing: no cartridge RAM). */
        if (o.target == TargetId::meta || esp) {
            const std::string gen = read(src / "p8_gen.c");
            size_t globals = 0;
            for (size_t at = 0; (at = gen.find("visit(&G", at)) != std::string::npos; at++) globals++;
            const size_t state_bytes = heap + 8192 + 8 * globals + 1024;
            const size_t sectors = 1 + (state_bytes + 511) / 512;
            const std::string sd = o.sd_name.empty() ? sd_base(o.name) : o.sd_name;
            if (esp) {
                const size_t slots = std::min<size_t>(10, (0x03B000 / 4096) / ((sectors + 7) / 8));
                config << "#define P8_STATE_SLOTS " << slots << " /* in flash */\n";
            } else {
                config << "#define P8_BIN_NAME83 \"" << name83(sd, "BIN") << "\"\n#define P8_STATE_NAME \"" << sd
                       << ".ST8\"\n#define P8_STATE_NAME83 \"" << name83(sd, "ST8") << "\"\n#define P8_STATE_SLOTS 10\n";
            }
            config << "#define P8_STATE_SLOT_SECTORS " << sectors << "\n";
        }
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
              "target_compile_options(" + o.name + "_host PRIVATE -w)\ntarget_link_libraries(" + o.name + "_host PRIVATE m)\n"
              "# --save-state / --load-state: the heap holds absolute pointers, so every run at the same addresses\n"
              "if(CMAKE_SYSTEM_NAME STREQUAL \"Linux\")\n"
              "    target_compile_options(" + o.name + "_host PRIVATE -fno-pie)\n"
              "    target_link_options(" + o.name + "_host PRIVATE -no-pie)\n"
              "endif()\n");

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
            /* save states: the card's state file in the game's folder on the META, the flash on the ESPboy */
            copy_template(o.meta_templates / "platform" / "src" / "meta_state.h", src / "meta_state.h");
            if (esp) {
                copy_template(platform / "espboy_flash.cpp", src / "espboy_flash.cpp");
                copy_template(common_dir / "runtime" / "m0_scale.h", src / "m0_scale.h"); /* espboy_hw.cpp */
            } else {
                for (const char* name : { "meta_state.cpp", "meta_sd.c", "meta_sd.h", "meta_fat.c", "meta_fat.h", "meta_card.h" })
                    copy_template(o.meta_templates / "platform" / "src" / name, src / name);
                copy_template(platform / "meta_sd_bus.cpp", src / "meta_sd_bus.cpp");
            }
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
                const std::string row = "line[x] = p[m0_nearest(x, 160, 128)];", frame = "uint8_t samples[192];";
                if (hw.find(row) == std::string::npos || hw.find(frame) == std::string::npos)
                    throw std::runtime_error("espboy_hw.cpp changed: the PICO-8 patch no longer applies");
                /* the picture is 128 columns at 16 in a 160-wide row; the option menu's rows are
                 * 160 wide, narrowed as on the Sega cores while it is open (p8_main.cpp) */
                hw = replace(hw, row, "line[x] = p8_menu_wide ? p[m0_nearest(x, 160, 128)] : p[x + 16];");
                hw = "extern \"C\" unsigned char p8_menu_wide;\n" + hw;
                hw = replace(hw, frame, "uint8_t samples[400]; /* a 30 Hz frame at 11025 Hz */");
                write(src / "espboy_hw.cpp", hw);
                copy_template(o.meta_templates / "runtime" / "gbl_espboy.h", src / "gbl_espboy.h");
                for (const char* name : { "platformio.ini", "espboy.ld", "espboy_build.py", "espboy_size.py" })
                    write(o.output / name, replace(replace(read(platform / (std::string(name) + ".in")), "@NAME@", o.name),
                                                   "-DCONT_STACKSIZE=12288", "-DCONT_STACKSIZE=6144"));
            } else {
                for (const char* name : { "meta_startup.c", "meta_clock.cpp", "meta_lcd_pack.h", "meta_lcd_queue.h",
                                          "meta_dma.h", "meta_dma.cpp", "meta_spi.c", "meta_hw.h", "meta_usb.c", "meta_usb.h",
                                          "meta_usb_msc.c", "meta_usb_msc.h" })
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
