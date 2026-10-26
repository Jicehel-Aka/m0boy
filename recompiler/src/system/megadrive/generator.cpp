#include "m0boy/megadrive_generator.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifndef GBRECOMP_META_VERSION
#define GBRECOMP_META_VERSION "dev"
#endif
namespace m0boy::megadrive {
namespace fs = std::filesystem;
namespace {
struct Instruction { uint32_t next; std::string body; std::vector<uint32_t> edges; };
std::string hex(uint32_t n) {
    std::ostringstream o; o << "0x" << std::hex << n << "u"; return o.str();
}
void write(const fs::path &p, const std::string &s) {
    std::ofstream f(p);
    if (!f || !(f << s)) throw std::runtime_error("Cannot write " + p.string());
}
std::string read(const fs::path &p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("Cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
void copy_template(const fs::path &a, const fs::path &b) { fs::copy_file(a, b, fs::copy_options::overwrite_existing); }
std::string replace(std::string s, const std::string &a, const std::string &b) {
    size_t at = 0;
    while ((at = s.find(a, at)) != std::string::npos) { s.replace(at, a.size(), b); at += b.size(); }
    return s;
}
uint16_t be16(const std::vector<uint8_t> &rom, uint32_t at) {
    if (at + 1 >= rom.size()) throw std::runtime_error("Truncated M68k opcode/extension at " + hex(at));
    return uint16_t(rom[at] << 8 | rom[at + 1]);
}
uint32_t be32(const std::vector<uint8_t> &rom, uint32_t at) {
    return (uint32_t)be16(rom, at) << 16 | be16(rom, at + 2);
}
void generate_pc_native(const std::vector<uint8_t> &rom, const GenerateOptions &o) {
    const fs::path marker = o.output / "megadrive-project.txt";
    if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output))
        throw std::runtime_error("Output directory is not a Mega Drive project");
    if (fs::exists(marker) && read(marker) != "pc\n")
        throw std::runtime_error("Use a separate output directory for each destination");
    fs::create_directories(o.output / "src");
    const fs::path src = o.output / "src", tpl = o.targets_dir / "megadrive";
    for (const char *n : {"md.h", "md.c", "md_code.c", "md_host.c"})
        fs::remove(src / n);
    fs::remove(o.output / "rom.md");
    for (const char *n : {"md_full.h", "md_full_cpu.c", "md_full_bus.c", "md_full_video.c"})
        copy_template(tpl / "runtime" / n, src / n);
    copy_template(tpl / "platform/md_full_pc.c", src / "md_full_pc.c");
    std::set<uint32_t> covered;
    covered.insert(be32(rom, 4));
    for (const auto &path : o.coverage) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("Cannot read Mega Drive coverage: " + path.string());
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            size_t used = 0;
            const uint32_t pc = static_cast<uint32_t>(std::stoul(line, &used, 16));
            if (used != line.size() || (pc & 1u) || pc + 1u >= rom.size())
                throw std::runtime_error("Invalid Mega Drive coverage PC in " + path.string());
            covered.insert(pc);
        }
    }
    std::ostringstream gen;
    gen << "/* ROM 68000 instructions selected from execution coverage. The opcode is "
           "constant here; uncovered and RAM code use the reference decoder. */\n"
           "#include \"md_full.h\"\n"
           "void md_full_step_native(MdFull *m) {\n"
           "  uint32_t pc = m->pc;\n  switch (pc) {\n";
    for (uint32_t pc : covered) {
        gen << "    case " << hex(pc) << ":\n"
               "      m->fault_pc = pc; m->pc = pc + 2u;\n"
               "      if (m->coverage && pc < m->coverage_bytes * 8u) "
               "m->coverage[pc >> 3] |= (uint8_t)(1u << (pc & 7u));\n"
               "      ++m->native_hits; md_full_execute(m, " << hex(be16(rom, pc)) << "); return;\n";
    }
    gen << "    default: md_full_step_interp(m); return;\n  }\n}\n";
    write(src / "md_full_code.c", gen.str());
    std::ostringstream data;
    data << "#include <stdint.h>\nconst uint32_t md_rom_size = " << rom.size() << "u;\n"
            "const uint8_t md_rom[] = {\n";
    for (size_t at = 0; at < rom.size(); ++at) {
        char b[8]; std::snprintf(b, sizeof b, "0x%02x,", rom[at]); data << b;
        if ((at & 15u) == 15u) data << '\n';
    }
    data << "\n};\n";
    write(src / "md_rom.c", data.str());
    write(o.output / "CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\nproject(md_native C)\n"
        "set(CMAKE_C_STANDARD 11)\nfind_package(PkgConfig REQUIRED)\n"
        "pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)\n"
        "add_executable(md_pc src/md_full_pc.c src/md_full_code.c src/md_full_cpu.c "
        "src/md_full_bus.c src/md_full_video.c src/md_rom.c)\n"
        "target_compile_options(md_pc PRIVATE -O2)\n"
        "target_link_libraries(md_pc PRIVATE PkgConfig::SDL2)\n");
    write(o.output / "README.md", "# Mega Drive native PC project: " + o.name + "\n\n"
        "The ROM's 68000 opcodes recorded by coverage are emitted as C cases in "
        "`src/md_full_code.c`; uncovered instructions use the in-tree 68000 decoder. "
        "The VDP, RAM and controller are implemented in this project. SDL2 only opens "
        "the PC window and reads keys.\n\n"
        "Build: `cmake -G Ninja -S . -B build && ninja -C build`.\n"
        "Run: `build/md_pc` (arrows, X/Z/C, Enter; Esc quits).\n"
        "Profile: `build/md_pc --interp --frames 1200 --input start.input --coverage run.cov`, "
        "then regenerate with `m0recomp --coverage run.cov`.\n");
    write(marker, "pc\n");
    write(o.output / "native-status.txt", "native-c\n");
}
Instruction decode(const std::vector<uint8_t> &rom, uint32_t pc) {
    const uint16_t op = be16(rom, pc);
    Instruction i{pc + 2, {}, {}};
    if (op == 0x4e71) i.body = "";
    else if (op == 0x33fc || op == 0x23fc) {
        const unsigned width = op == 0x33fc ? 2 : 4;
        const uint32_t value = width == 2 ? be16(rom, pc + 2) : be32(rom, pc + 2);
        const uint32_t addr = be32(rom, pc + 2 + width);
        i.next = pc + 6 + width;
        i.body = std::string(width == 2 ? "md_write16" : "md_write32") + "(m, " + hex(addr) + ", " + hex(value) + ");";
    } else if ((op & 0xfff8u) == 0x33c0u) {
        const uint32_t addr = be32(rom, pc + 2);
        i.next = pc + 6;
        i.body = "md_write16(m, " + hex(addr) + ", (uint16_t)m->d[" + std::to_string(op & 7u) + "]);";
    } else if ((op & 0xf100u) == 0x7000u) {
        i.body = "md_set_long(m, " + std::to_string((op >> 9) & 7u) + ", " +
                 hex((uint32_t)(int32_t)(int8_t)op) + ");";
    } else if ((op & 0xfff8u) == 0x0640u) {
        const unsigned n = op & 7u;
        const uint16_t value = be16(rom, pc + 2);
        i.next = pc + 4;
        i.body = "md_set_word(m, " + std::to_string(n) + ", (uint16_t)(m->d[" +
                 std::to_string(n) + "] + " + hex(value) + "));";
    } else if ((op & 0xf1f8u) == 0x5040u) {
        const unsigned n = op & 7u, amount = (op >> 9) & 7u;
        i.body = "md_set_word(m, " + std::to_string(n) + ", (uint16_t)(m->d[" +
                 std::to_string(n) + "] + " + std::to_string(amount ? amount : 8) + "u));";
    } else if ((op & 0xfff8u) == 0x0c40u) {
        const uint16_t value = be16(rom, pc + 2);
        i.next = pc + 4;
        i.body = "m->z = (uint16_t)m->d[" + std::to_string(op & 7u) + "] == " + hex(value) + ";";
    } else if ((op & 0xf000u) == 0x6000u && (((op >> 8) & 15u) == 0u ||
                                               ((op >> 8) & 15u) == 6u || ((op >> 8) & 15u) == 7u)) {
        int32_t delta = (int8_t)op;
        if (!(op & 255u)) { delta = (int16_t)be16(rom, pc + 2); i.next = pc + 4; }
        const int64_t dest = int64_t(pc + 2) + delta;
        if (dest < 0 || dest >= int64_t(rom.size()) || (dest & 1))
            throw std::runtime_error("M68k branch out of ROM at " + hex(pc));
        const unsigned cc = (op >> 8) & 15u;
        i.body = cc == 0 ? "m->pc = " + hex(uint32_t(dest)) + ";" :
                 std::string("if (") + (cc == 6 ? "!" : "") + "m->z) m->pc = " + hex(uint32_t(dest)) + ";";
        i.edges.push_back(uint32_t(dest));
        if (cc == 0) return i;
    } else {
        throw std::runtime_error("Unsupported M68k opcode " + hex(op) + " at ROM address " + hex(pc) +
                                 "; this Mega Drive profile only translates immediate/register colour loops");
    }
    i.edges.push_back(i.next);
    return i;
}
}

bool generate(const fs::path &rom_path, const GenerateOptions &o, std::string &error) {
    try {
        if (o.target != TargetId::desktop && o.target != TargetId::meta && o.target != TargetId::pc)
            throw std::runtime_error("Mega Drive profile supports --target desktop, meta or pc");
        std::string inspect_error;
        if (!inspect(rom_path, inspect_error)) throw std::runtime_error(inspect_error);
        std::ifstream f(rom_path, std::ios::binary);
        const std::vector<uint8_t> rom{std::istreambuf_iterator<char>(f), {}};
        if (o.target == TargetId::pc) { generate_pc_native(rom, o); return true; }
        if (o.target == TargetId::meta && be32(rom, 0) < 0xfff000u)
            throw std::runtime_error("Mega Drive META profile needs a stack in the upper 4 KiB of work RAM");
        std::map<uint32_t, Instruction> code;
        std::map<uint32_t, uint32_t> byte_owner;
        std::set<uint32_t> pending{be32(rom, 4)};
        bool native_supported = true;
        std::string native_error;
        try {
        while (!pending.empty()) {
            const uint32_t pc = *pending.begin(); pending.erase(pending.begin());
            if (code.count(pc)) continue;
            if (byte_owner.count(pc) && byte_owner.at(pc) != pc)
                throw std::runtime_error("M68k branch enters the middle of an instruction at " + hex(pc));
            if (code.size() >= 4096) throw std::runtime_error("M68k static walk exceeded 4096 instructions");
            auto inst = decode(rom, pc);
            for (uint32_t at = pc; at < inst.next; ++at) {
                const auto [found, fresh] = byte_owner.emplace(at, pc);
                if (!fresh && found->second != pc)
                    throw std::runtime_error("Overlapping M68k instructions at " + hex(at));
            }
            for (uint32_t e : inst.edges) {
                if (e >= rom.size() || (e & 1u)) throw std::runtime_error("M68k control flow leaves ROM at " + hex(e));
                if (!code.count(e)) pending.insert(e);
            }
            code.emplace(pc, std::move(inst));
        }
        } catch (const std::exception &e) {
            if (o.target != TargetId::pc) throw;
            native_supported = false;
            native_error = e.what();
        }
        const fs::path marker = o.output / "megadrive-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output))
            throw std::runtime_error("Output directory is not a Mega Drive project");
        const std::string target = o.target == TargetId::meta ? "meta" : "desktop";
        if (fs::exists(marker) && read(marker) != target + "\n")
            throw std::runtime_error("Use a separate output directory for each destination");
        fs::create_directories(o.output / "src");
        const fs::path src = o.output / "src", tpl = o.targets_dir / "megadrive";
        if (!native_supported)
            for (const char *n : {"md.h", "md.c", "md_code.c", "md_rom.c", "md_host.c"}) fs::remove(src / n);
        if (native_supported) {
        copy_template(tpl / "runtime/md.h", src / "md.h");
        copy_template(tpl / "runtime/md.c", src / "md.c");
        std::ostringstream gen;
        gen << "/* Recompiled 68000 ROM instructions; no runtime opcode decode. */\n#include \"md.h\"\n"
               "void md_run_native(Md *m, uint32_t steps) {\n"
               "  while (steps-- && !m->fault) {\n    switch (m->pc) {\n";
        for (const auto &[pc, i] : code)
            gen << "    case " << hex(pc) << ": m->pc = " << hex(i.next) << "; "
                << i.body << " break;\n";
        gen << "    default: m->fault = 2; break;\n    }\n    m->instructions++;\n  }\n}\n";
        write(src / "md_code.c", gen.str());
        std::ostringstream data;
        data << "#include <stdint.h>\nconst uint32_t md_rom_size = " << rom.size() << "u;\nconst uint8_t md_rom[] = {\n";
        for (size_t at = 0; at < rom.size(); ++at) {
            char b[8]; std::snprintf(b, sizeof b, "0x%02x,", rom[at]); data << b;
            if ((at & 15u) == 15u) data << '\n';
        }
        data << "\n};\n";
        write(src / "md_rom.c", data.str());
        }
        if (o.target == TargetId::desktop) {
            if (native_supported) copy_template(tpl / "host/md_host.c", src / "md_host.c");
            std::string cmake = "cmake_minimum_required(VERSION 3.20)\nproject(md_profile C)\n"
                                "set(CMAKE_C_STANDARD 11)\n";
            if (native_supported)
                cmake += "add_executable(md_host src/md.c src/md_code.c src/md_rom.c src/md_host.c)\n"
                         "target_compile_definitions(md_host PRIVATE MD_FULL_VDP=1)\n";
            write(o.output / "CMakeLists.txt", cmake);
        } else {
            copy_template(tpl / "platform/md_main.cpp", src / "md_main.cpp");
            const fs::path common = o.targets_dir / "common/platform";
            const fs::path platform = o.targets_dir / "consoles/meta";
            for (const char *n : {"m0_target.h", "m0_lcd.h", "m0_clock.h", "m0_pad.h"}) copy_template(common / n, src / n);
            copy_template(o.targets_dir / "meta/runtime/gbl_board.h", src / "gbl_board.h");
            write(src / "gbl_config.h", std::string("#pragma once\n#define GBL_TARGET_POKITTO 0\n#define GBL_GEN_VERSION \"") + GBRECOMP_META_VERSION + "\"\n");
            write(src / "gbl_profile.h", "#pragma once\n"
                  "#define META_LCD_DMA 0\n#define META_LCD_12BPP 0\n#define META_OVERCLOCK 0\n"
                  "#define META_USB_MODE 0\n#include \"gbl_board.h\"\n");
            for (const char *n : {"meta_startup.c", "meta_clock.cpp", "meta_lcd.cpp", "meta_lcd_pack.h",
                                  "meta_spi.c", "meta_hw.h"}) copy_template(platform / n, src / n);
            write(o.output / "platformio.ini", replace(replace(read(platform / "platformio.ini.in"), "@NAME@", o.name), "@COMPILER@", o.compiler));
            write(o.output / "gamebuino_meta.ld", replace(read(platform / "gamebuino_meta.ld.in"), "@NAME@", o.name));
            fs::create_directories(o.output / "boards");
            copy_template(platform / "boards/gamebuino_meta.json", o.output / "boards/gamebuino_meta.json");
            fs::create_directories(o.output / "toolchain");
            for (const auto &e : fs::directory_iterator(o.targets_dir / "common/toolchain"))
                if (e.is_regular_file()) copy_template(e.path(), o.output / "toolchain" / e.path().filename());
        }
        write(o.output / "README.md", "# Mega Drive project: " + o.name + "\n\n" +
              (native_supported ? "Experimental native 68000 profile. On PC/desktop, "
                "64 KiB work RAM, VRAM, CRAM and a first Plane A tile renderer; on META, "
                "upper 4 KiB RAM and CRAM backdrop only. Unsupported opcodes or bus accesses "
                "are rejected/stopped. No scrolling, sprites, Z80 or sound.\n\n" :
                "The native 68000 profile cannot translate this ROM.\n\n") +
              (o.target == TargetId::meta ? "Build: `pio run` (META firmware in `.pio/build/meta/firmware.bin`).\n" :
                                          "Build: `cmake -G Ninja -S . -B build && ninja -C build`.\n") +
              (native_supported && o.target != TargetId::meta ? "Native reference: `build/md_host --frames 60`; add `--interp` to compare.\n" : "") +
              (!native_supported ? "\nNative translation stopped at: " + native_error + "\n" : ""));
        write(marker, target + "\n");
        write(o.output / "native-status.txt", native_supported ? "native\n" : "unsupported\n");
        return true;
    } catch (const std::exception &e) { error = e.what(); return false; }
}
}
