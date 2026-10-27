#include "m0boy/m0plus_generator.h"
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace m0boy::m0plus {
namespace fs = std::filesystem;
namespace {
uint16_t half(const std::vector<uint8_t>& bytes, size_t at) {
    return uint16_t(bytes[at]) | uint16_t(bytes[at + 1]) << 8;
}
uint32_t word(const std::vector<uint8_t>& bytes, size_t at) {
    return uint32_t(half(bytes, at)) | uint32_t(half(bytes, at + 2)) << 16;
}
std::set<uint32_t> walk(const std::vector<uint8_t>& bytes, uint32_t base,
                        const std::vector<fs::path>& coverage, std::string& error) {
    std::set<uint32_t> found;
    std::vector<uint32_t> todo;
    auto enqueue = [&](uint32_t at) {
        if (!(at & 1u) && at >= base && uint64_t(at) + 2 <= uint64_t(base) + bytes.size())
            todo.push_back(at);
    };
    const size_t vectors = std::min<size_t>(48, bytes.size() / 4);
    for (size_t i = 1; i < vectors; ++i) {
        const uint32_t handler = word(bytes, i * 4);
        if (handler & 1u) enqueue(handler & ~1u);
    }
    for (const auto& path : coverage) {
        std::ifstream file(path);
        if (!file) { error = "Cannot read M0+ coverage: " + path.string(); return {}; }
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream row(line);
            std::string address;
            row >> address;
            try {
                size_t used = 0;
                const auto parsed = std::stoull(address, &used, 16);
                if (used != address.size() || parsed > 0xffffffffu) throw std::invalid_argument("address");
                enqueue(static_cast<uint32_t>(parsed));
            } catch (const std::exception&) {
                error = "Invalid M0+ coverage address in " + path.string(); return {};
            }
        }
    }
    while (!todo.empty()) {
        const uint32_t pc = todo.back(); todo.pop_back();
        if (!found.insert(pc).second) continue;
        const size_t at = pc - base;
        const uint16_t op = half(bytes, at);
        const bool wide = (op & 0xf800u) >= 0xe800u;
        if (wide && at + 3 >= bytes.size()) continue;
        const uint32_t next = pc + (wide ? 4u : 2u);
        if ((op & 0xf800u) == 0xe000u) {
            const int32_t off = (int32_t)((op & 0x7ffu) << 21) >> 20;
            enqueue(pc + 4u + uint32_t(off));
            continue;
        }
        if ((op & 0xf000u) == 0xd000u && ((op >> 8) & 15u) < 14u) {
            enqueue(pc + 4u + uint32_t(int32_t(int8_t(op)) * 2));
        } else if ((op & 0xf500u) == 0xb100u) {
            enqueue(pc + 4u + (((op >> 9) & 1u) << 6) + (((op >> 3) & 31u) << 1));
        } else if ((op & 0xf800u) == 0xf000u && (half(bytes, at + 2) & 0xd000u) == 0xd000u) {
            const uint16_t second = half(bytes, at + 2);
            const uint32_t s = (op >> 10) & 1u;
            const uint32_t i1 = !(((second >> 13) & 1u) ^ s);
            const uint32_t i2 = !(((second >> 11) & 1u) ^ s);
            const uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) |
                                 ((op & 0x3ffu) << 12) | ((second & 0x7ffu) << 1);
            enqueue(pc + 4u + uint32_t(int32_t(imm << 7) >> 7));
        }
        if ((op & 0xff00u) == 0x4700u || /* BX / BLX register */
            ((op & 0xfe00u) == 0xbc00u && (op & 0x100u)) || /* POP PC */
            ((op & 0xfc00u) == 0x4400u && ((op >> 8) & 3u) != 1u &&
             ((op & 7u) | ((op >> 4) & 8u)) == 15u)) /* ADD/MOV PC */
            continue;
        enqueue(next);
    }
    return found;
}
}

bool generate(const fs::path& firmware, SystemId system, const fs::path& output,
              std::string& error, const fs::path& runtime_dir,
              const std::vector<fs::path>& coverage) {
    if (system != SystemId::meta && system != SystemId::pokitto) {
        error = "M0+ project requires --system meta or pokitto";
        return false;
    }
    const auto inspector = system_descriptor(system).inspect;
    if (!inspector(firmware, error)) return false;
    std::error_code ec;
    if (fs::exists(output, ec) && !fs::is_directory(output, ec)) {
        error = "Output is not a directory: " + output.string();
        return false;
    }
    const fs::path marker = output / "m0plus-project.txt";
    if (fs::exists(output, ec) && !fs::is_empty(output, ec)) {
        std::ifstream previous(marker);
        std::string signature;
        std::getline(previous, signature);
        if (signature != (system == SystemId::meta ? "meta-pc" : "pokitto-pc")) {
            error = "Output directory belongs to another project";
            return false;
        }
    }
    fs::create_directories(output, ec);
    if (ec) { error = "Cannot create output directory: " + ec.message(); return false; }
    const fs::path destination = output / "firmware.bin";
    if (!fs::equivalent(firmware, destination, ec)) {
        ec.clear();
        fs::copy_file(firmware, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "Cannot copy firmware: " + ec.message(); return false; }
    }
    const char* board = system == SystemId::meta ? "meta" : "pokitto";
    if (!runtime_dir.empty()) {
        const fs::path source = output / "src";
        fs::create_directories(source, ec);
        if (ec) { error = "Cannot create native source directory: " + ec.message(); return false; }
        const fs::path host = runtime_dir.parent_path() / "host/m0_native_host.c";
        for (const fs::path& file : {runtime_dir / "m0_cpu.h", runtime_dir / "m0_cpu.c",
                                     runtime_dir / "m0_machine.h", runtime_dir / "m0_machine.c", host}) {
            ec.clear();
            fs::copy_file(file, source / file.filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) { error = "Cannot copy M0+ runtime " + file.string() + ": " + ec.message(); return false; }
        }
        const fs::path gm0 = runtime_dir.parent_path() / "gm0";
        for (const char* name : {"gm0.c", "aka.c", "aka.h", "esp8266.h",
                                 "esp8266_xt.h", "esp8266_rom_syms.h"}) {
            ec.clear();
            fs::copy_file(gm0 / name, source / name, fs::copy_options::overwrite_existing, ec);
            if (ec) { error = "Cannot copy integrated gm0 component: " + ec.message(); return false; }
        }
        ec.clear();
        fs::copy_file(gm0 / "LICENSE", output / "GM0-LICENSE", fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "Cannot copy integrated gm0 license: " + ec.message(); return false; }
        std::ifstream binary(destination, std::ios::binary);
        const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{binary}, {});
        if (bytes.empty()) { error = "Cannot read firmware for M0+ C translation"; return false; }
        const uint32_t base = system == SystemId::meta ? 0x4000u : 0u;
        const auto compiled = walk(bytes, base, coverage, error);
        if (!error.empty()) return false;
        {
            std::ofstream rom(source / "m0_rom.c");
            rom << "#include <stdint.h>\nconst uint32_t m0_rom_size = " << bytes.size()
                << "u;\nconst uint8_t m0_rom[] = {\n";
            for (size_t i = 0; i < bytes.size(); ++i) {
                rom << "0x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]) << ',';
                if ((i & 15u) == 15u) rom << '\n';
            }
            rom << "\n};\n";
            if (!rom) { error = "Cannot write embedded M0+ firmware"; return false; }
        }
        {
            std::ofstream code(source / "m0_native.c");
            code << "#include \"m0_cpu.h\"\n"
                    "/* Fixed flash addresses dispatch to constant Thumb opcodes. "
                    "RAM code and modified flash fall back to m0_cpu_step. */\n"
                    "int m0_native_step(M0Cpu *m) {\n"
                    "  if (m->fault) return 0;\n"
                    "  switch (m->r[15]) {\n";
            for (uint32_t pc : compiled) {
                const size_t i = pc - base;
                const uint16_t op = uint16_t(bytes[i]) | uint16_t(bytes[i + 1]) << 8;
                const bool wide = (op & 0xf800u) >= 0xe800u && i + 3 < bytes.size();
                const uint16_t op2 = wide ? uint16_t(bytes[i + 2]) | uint16_t(bytes[i + 3]) << 8 : 0;
                code << "  case 0x" << std::hex << pc << "u:\n"
                     << "    if (m->read(m->board, 0x" << pc << "u, 2) != 0x" << op << "u";
                if (wide) code << " || m->read(m->board, 0x" << pc + 2 << "u, 2) != 0x" << op2 << "u";
                code << ") break;\n";
                if ((op & 0xf800u) == 0x2000u) { // MOVS Rd, #imm8
                    const unsigned imm = op & 255u, dst = (op >> 8) & 7u;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; m->r[" << dst
                         << "] = 0x" << imm << "u; m->n = 0; m->z = " << (imm == 0 ? 1 : 0)
                         << "; m0_native_finish(m, 1); return 1;\n";
                } else if ((op & 0xe000u) == 0x2000u) { // CMP/ADD/SUB immediate
                    const unsigned dst = (op >> 8) & 7u, imm = op & 255u;
                    const unsigned kind = (op >> 11) & 3u;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; ";
                    if (kind == 1) code << "(void)m0_native_sub(m, m->r[" << dst << "], 0x" << imm << "u); ";
                    else code << "m->r[" << dst << "] = m0_native_" << (kind == 2 ? "add" : "sub")
                              << "(m, m->r[" << dst << "], " << (kind == 2 ? "0x" : "0x") << imm
                              << "u" << (kind == 2 ? ", 0" : "") << "); ";
                    code << "m0_native_finish(m, 1); return 1;\n";
                } else if ((op & 0xf800u) == 0x1800u) { // ADD/SUB low registers or imm3
                    const unsigned dst = op & 7u, lhs = (op >> 3) & 7u, rhs = (op >> 6) & 7u;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; m->r[" << dst
                         << "] = m0_native_" << (op & 0x200u ? "sub" : "add") << "(m, m->r["
                         << lhs << "], " << (op & 0x400u ? "0x" : "m->r[") << rhs
                         << (op & 0x400u ? "u" : "]") << (op & 0x200u ? "" : ", 0")
                         << "); m0_native_finish(m, 1); return 1;\n";
                } else if ((op & 0xf800u) == 0x4800u) { // literal load
                    const unsigned dst = (op >> 8) & 7u;
                    const uint32_t address = ((pc + 4u) & ~3u) + ((op & 255u) << 2);
                    code << "    m->r[15] = 0x" << pc + 2 << "u; m->r[" << dst
                         << "] = m->read(m->board, 0x" << address
                         << "u, 4); m0_native_finish(m, 2); return 1;\n";
                } else if ((op & 0xe000u) == 0x6000u || (op & 0xf000u) == 0x8000u) {
                    const unsigned dst = op & 7u, src = (op >> 3) & 7u;
                    const unsigned width = (op & 0xf000u) == 0x8000u ? 2u : (op & 0x1000u ? 1u : 4u);
                    const unsigned offset = ((op >> 6) & 31u) * width;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; ";
                    if (op & 0x0800u) code << "m->r[" << dst << "] = m->read(m->board, m->r["
                                           << src << "] + 0x" << offset << "u, " << width << "); ";
                    else code << "m->write(m->board, m->r[" << src << "] + 0x" << offset
                              << "u, m->r[" << dst << "], " << width << "); ";
                    code << "m0_native_finish(m, 2); return 1;\n";
                } else if ((op & 0xf000u) == 0x9000u) { // SP-relative load/store
                    const unsigned dst = (op >> 8) & 7u, offset = (op & 255u) << 2;
                    code << "    m->r[15] = 0x" << pc + 2 << "u; ";
                    if (op & 0x0800u) code << "m->r[" << dst << "] = m->read(m->board, m->r[13] + 0x"
                                           << offset << "u, 4); ";
                    else code << "m->write(m->board, m->r[13] + 0x" << offset << "u, m->r["
                              << dst << "], 4); ";
                    code << "m0_native_finish(m, 2); return 1;\n";
                } else if ((op & 0xf000u) == 0xd000u && ((op >> 8) & 15u) < 14u) {
                    const uint32_t target = pc + 4u + uint32_t(int32_t(int8_t(op)) * 2);
                    code << "    if (m0_native_cond(m, " << std::dec << ((op >> 8) & 15u) << std::hex << ")) { m->r[15] = 0x"
                         << target << "u; m0_native_finish(m, 2); } else { m->r[15] = 0x"
                         << pc + 2 << "u; m0_native_finish(m, 1); } return 1;\n";
                } else if ((op & 0xf800u) == 0xe000u) {
                    const int32_t offset = (int32_t)((op & 0x7ffu) << 21) >> 20;
                    code << "    m->r[15] = 0x" << pc + 4u + uint32_t(offset)
                         << "u; m0_native_finish(m, 2); return 1;\n";
                } else if (op == 0xbf00u) { // NOP
                    code << "    m->r[15] = 0x" << pc + 2
                         << "u; m0_native_finish(m, 1); return 1;\n";
                } else {
                    code << "    m0_cpu_execute(m, 0x" << pc << "u, 0x" << op
                         << "u, 0x" << op2 << "u); return 1;\n";
                }
            }
            code << "  default: break;\n  }\n  return 0;\n}\n";
            if (!code) { error = "Cannot write M0+ C translation"; return false; }
        }
        /* Keep each C translation unit bounded. A large ROM with a long
         * straight-line walk otherwise gives GCC one enormous switch and
         * minutes of compilation even for a 64 KiB synthetic image. */
        {
            std::ifstream generated(source / "m0_native.c");
            const std::string all(std::istreambuf_iterator<char>{generated}, {});
            std::map<uint32_t, std::string> pages;
            const size_t first = all.find("  case 0x");
            const size_t end = all.find("  default: break;", first);
            if (first == std::string::npos || end == std::string::npos) {
                error = "Cannot partition M0+ translation"; return false;
            }
            for (size_t at = first; at < end;) {
                const size_t next = all.find("  case 0x", at + 1);
                const size_t stop = next == std::string::npos || next > end ? end : next;
                const uint32_t pc = static_cast<uint32_t>(std::stoul(all.substr(at + 9), nullptr, 16));
                pages[pc & ~0xfffu] += all.substr(at, stop - at);
                at = stop;
            }
            for (const auto& entry : fs::directory_iterator(source)) {
                const auto name = entry.path().filename().string();
                if (name.starts_with("m0_native_page_") && entry.path().extension() == ".c")
                    fs::remove(entry.path(), ec);
            }
            std::ofstream dispatch(source / "m0_native.c");
            dispatch << "#include \"m0_cpu.h\"\n";
            for (const auto& [page, body] : pages) {
                std::ostringstream symbol;
                symbol << "m0_native_page_" << std::hex << page;
                std::ofstream unit(source / (symbol.str() + ".c"));
                unit << "#include \"m0_cpu.h\"\nint " << symbol.str()
                     << "(M0Cpu *m) {\n  switch (m->r[15]) {\n" << body
                     << "  default: break;\n  }\n  return 0;\n}\n";
                if (!unit) { error = "Cannot write M0+ native page"; return false; }
                dispatch << "int " << symbol.str() << "(M0Cpu *m);\n";
            }
            dispatch << "int m0_native_step(M0Cpu *m) {\n  if (m->fault) return 0;\n"
                        "  switch (m->r[15] & ~0xfffu) {\n";
            for (const auto& [page, body] : pages) {
                dispatch << "  case 0x" << std::hex << page << "u: return m0_native_page_"
                         << page << "(m);\n";
            }
            dispatch << "  default: return 0;\n  }\n}\n";
            if (!dispatch) { error = "Cannot write M0+ native dispatcher"; return false; }
        }
        {
            std::ofstream status(output / "native-status.txt");
            status << "compiled_flash_entries=" << compiled.size() << "\n"
                   << "dynamic_or_ram_entries=interpreter\n";
        }
        {
            std::ofstream cmake(output / "CMakeLists.txt");
            cmake << "cmake_minimum_required(VERSION 3.20)\nproject(m0_native C)\n"
                     "set(CMAKE_C_STANDARD 11)\n"
                     "file(GLOB M0_PAGES CONFIGURE_DEPENDS src/m0_native_page_*.c)\n"
                     "add_executable(m0_native_pc src/m0_native_host.c src/m0_native.c ${M0_PAGES} "
                     "src/m0_rom.c src/m0_cpu.c src/m0_machine.c)\n"
                     "set_property(GLOBAL PROPERTY JOB_POOLS m0_compile=4)\n"
                     "set_property(TARGET m0_native_pc PROPERTY JOB_POOL_COMPILE m0_compile)\n"
                     "target_compile_options(m0_native_pc PRIVATE -O2)\n"
                     "target_compile_definitions(m0_native_pc PRIVATE M0_FLASH_BASE="
                  << (system == SystemId::meta ? "0x4000u" : "0u") << " M0_RAM_BASE="
                  << (system == SystemId::meta ? "0x20000000u" : "0x10000000u") << ")\n"
                     "find_package(SDL2 QUIET)\nfind_package(ZLIB QUIET)\n"
                     "if(SDL2_FOUND AND ZLIB_FOUND)\n"
                     "  add_executable(m0_firmware_pc src/gm0.c src/aka.c)\n"
                     "  target_compile_options(m0_firmware_pc PRIVATE -O2)\n"
                     "  target_link_libraries(m0_firmware_pc PRIVATE SDL2::SDL2 ZLIB::ZLIB m)\n"
                     "endif()\n";
            if (!cmake) { error = "Cannot write M0+ native build"; return false; }
        }
    }
    {
        std::ofstream project(marker);
        project << board << "-pc\n";
        if (!project) { error = "Cannot write project marker"; return false; }
    }
    {
        std::ofstream launcher(output / "run.py");
        launcher << "#!/usr/bin/env python3\n"
                    "\"\"\"Run the embedded firmware in the integrated gm0 board emulator.\"\"\"\n"
                    "import os\nimport shutil\nimport subprocess\nimport sys\n"
                    "from pathlib import Path\n\n"
                    "root = Path(__file__).resolve().parent\n"
                    "local = root / 'build' / 'm0_firmware_pc'\n"
                    "gm0 = os.environ.get('GM0') or (str(local) if local.is_file() else shutil.which('gm0'))\n"
                    "if not gm0:\n"
                    "    sys.exit('build this project with CMake/Ninja (SDL2 and zlib required), or set GM0=/path/to/gm0')\n"
                    "sys.exit(subprocess.call([gm0, str(root / 'firmware.bin'), "
                 << "'--target', '" << board << "', *sys.argv[1:]], cwd=root))\n";
        if (!launcher) { error = "Cannot write PC launcher"; return false; }
    }
    {
        std::ofstream readme(output / "README.md");
        readme << "# " << board << " firmware on PC\n\n"
                  "This project contains the supplied firmware image and an integrated "
                  "gm0 board emulator (MIT, with third-party notices in `GM0-LICENSE`). "
                  "Build it with CMake/Ninja, SDL2 and zlib, then run:\n\n"
                  "```sh\ncmake -G Ninja -S . -B build && ninja -C build\n"
                  "python3 run.py [card.img or card/] "
                  "[gm0 options]\n```\n\n"
                  "gm0 supplies the Cortex-M0+ machine, " << board
               << " peripherals, display, input and SD card. The raw `.bin` has no "
                  "card contents; pass an image or folder when the application needs one. "
                  "For Pokitto loader `.pop` files, extract the firmware `.bin` first.\n\n"
                  "The separate `m0_native_pc` program builds with CMake/Ninja and "
                  "compares fixed-address C dispatch with the Thumb fallback "
                  "(`--interp --steps N`). It models the CPU, SRAM and NVIC only; "
                  "a real firmware requiring board peripherals must run through `run.py`.\n";
        if (!readme) { error = "Cannot write PC README"; return false; }
    }
    error.clear();
    return true;
}
}
