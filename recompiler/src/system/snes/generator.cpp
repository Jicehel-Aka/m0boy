/* SNES project generation, after the PCE generator's plan: the cartridge
 * image as C data, the runtime with the recompiled 65C816 code (snes_gen.c,
 * or the interpreter alone with --nes-interp) and the desktop harness. */
#include "m0boy/snes_generator.h"
#include "m0boy/snes_emitter.h"
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace m0boy::snes {
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

bool hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
/* a coverage line's context: K, DBR, the direct page and the M/X/E flags
 * packed K lowest, ten hex digits */
bool is_ctx(const std::string& t) {
    if (t.size() != 10) return false;
    for (char c : t)
        if (!hex_digit(c)) return false;
    return true;
}

} // namespace

/* the harness's coverage files: "addr [ctx...] hits [d]" lines — the
 * contexts are the K/DBR/D/flag bytes the address ran under.  Same-address
 * lines merge (hits summed, contexts kept, dynamic kept). */
std::vector<CoverageEntry> read_coverage(const std::vector<fs::path>& files) {
    std::map<uint64_t, CoverageEntry> sum; /* key: pc24 in the high bits, then the context */
    auto key_of = [&](uint32_t pc, bool ctxed, uint64_t ctx) {
        return ctxed ? (uint64_t(pc) << 24) ^ ctx : (uint64_t(pc) << 24) ^ (1ULL << 63);
    };
    for (const auto& path : files) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("Cannot read coverage file: " + path.string());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string addr;
            unsigned long pc;
            if (!(fields >> addr)) throw std::runtime_error("Bad coverage line in " + path.string() + ": " + line);
            pc = std::stoul(addr, nullptr, 16);
            std::vector<uint64_t> ctxs;
            uint64_t hits = 0;
            bool dynamic = false;
            std::string tok;
            while (fields >> tok) {
                if (tok == "d") dynamic = true;
                else if (is_ctx(tok)) ctxs.push_back(std::stoull(tok, nullptr, 16));
                else hits += std::stoull(tok, nullptr, 10);
            }
            if (ctxs.empty()) { /* an older file's address without a context */
                auto& e = sum[key_of(uint32_t(pc), false, 0)];
                e.pc = uint32_t(pc);
                e.hits = uint32_t(std::min<uint64_t>(uint64_t(e.hits) + hits, 0xFFFFFFFFu));
                e.dynamic = e.dynamic || dynamic;
            } else {
                for (uint64_t ctx : ctxs) {
                    auto& e = sum[key_of(uint32_t(pc), true, ctx)];
                    e.pc = uint32_t(pc);
                    e.hits = uint32_t(std::min<uint64_t>(uint64_t(e.hits) + hits, 0xFFFFFFFFu));
                    e.dynamic = e.dynamic || dynamic;
                    e.contexts.push_back(ctx);
                }
            }
        }
    }
    std::vector<CoverageEntry> out;
    for (const auto& [key, e] : sum) out.push_back(e);
    return out;
}

/* the header's map nibble: $20/$22/$23 LoROM, $21/$25 HiROM; the checksum
 * complement pairing tells a real header from noise */
static int header_map(const std::vector<uint8_t>& rom, size_t at) {
    if (at + 0x40 > rom.size()) return -1;
    const unsigned comp = rom[at + 0x3C] | rom[at + 0x3D] << 8;
    const unsigned cksum = rom[at + 0x3E] | rom[at + 0x3F] << 8;
    const unsigned map = rom[at + 0x15] & 0x0F;
    if ((comp ^ cksum) == 0xFFFF && (comp != 0 || cksum != 0xFFFF)) return (int)map;
    if (map == 0 || map == 1 || map == 2 || map == 3 || map == 5) return (int)map;
    return -1;
}
static bool detect_hirom(const std::vector<uint8_t>& rom) {
    const int hi = header_map(rom, 0xFFC0);
    const int lo = header_map(rom, 0x7FC0);
    if (hi == 1 || hi == 5) return true;
    if (lo == 0 || lo == 2 || lo == 3) return false;
    return hi >= 0 && lo < 0;
}

bool generate(const fs::path& cartridge, const GenerateOptions& o, GenerateStats& stats, std::string& error) {
    error.clear();
    stats = {};
    try {
        if (o.target != TargetId::desktop)
            throw std::runtime_error("SNES projects target the desktop harness (--target desktop); "
                                     "the picture's PPU/APU budget is the next step");
        const auto tpl = o.meta_templates.parent_path() / "snes";
        if (!fs::is_regular_file(tpl / "runtime" / "snes.c"))
            throw std::runtime_error("SNES templates missing next to targets/meta");

        /* the cartridge: the 512-byte header some collections carry, then
         * the image as the bus sees it */
        std::ifstream file(cartridge, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot read " + cartridge.string());
        file.seekg(0, std::ios::end);
        const size_t file_bytes = size_t(file.tellg());
        if (file_bytes < 0x8000) throw std::runtime_error("SNES image too small: " + cartridge.string());
        const size_t skip = file_bytes % 0x8000 == 512 ? 512 : 0;
        const size_t image_bytes = file_bytes - skip;
        std::vector<uint8_t> rom(image_bytes);
        file.seekg(std::streamoff(skip));
        file.read(reinterpret_cast<char*>(rom.data()), std::streamsize(image_bytes));
        if (size_t(file.gcount()) != image_bytes)
            throw std::runtime_error("Truncated image: " + cartridge.string());
        const bool hirom = detect_hirom(rom);

        const auto marker = o.output / "snes-project.txt";
        if (fs::exists(o.output) && !fs::exists(marker) && !fs::is_empty(o.output)) {
            bool foreign = false;
            for (const auto& e : fs::directory_iterator(o.output)) foreign = foreign || e.path().filename() != "compile.log";
            if (foreign) throw std::runtime_error("Output directory is not a SNES project; choose a fresh directory");
        }
        if (fs::exists(marker)) {
            if (read(marker) != "desktop\n")
                throw std::runtime_error("Use a separate output directory for each destination");
        }
        const auto src = o.output / "src";
        fs::create_directories(src);
        fs::create_directories(o.output / "host");

        write(src / "snes_cart.c",
              "/* The cartridge, generated by gbrecomp. */\n"
              "#include <stdint.h>\n\n"
              "const uint8_t snes_rom[] = {\n" + rom_data(rom.data(), image_bytes) + "};\n");
        std::string config = "#pragma once\n"
              "#define SNES_ROM_SIZE " + std::to_string(image_bytes) + "u\n"
              "#define SNES_HIROM " + std::to_string(hirom ? 1 : 0) + "\n";
        /* the recompiled code's dispatcher (snes_cpu.c) */
        if (!o.interpret) config += "#ifndef SNES_NATIVE\n#define SNES_NATIVE 1\n#endif\n";
        write(src / "snes_config.h", config);

        for (const char* name : { "snes_cpu.c", "snes.c", "snes.h", "snes_gen.h" })
            copy_template(tpl / "runtime" / name, src / name);
        /* the idle loops' count (snes_gen.h, the dispatcher) */
        copy_template(o.meta_templates.parent_path() / "common" / "runtime" / "m0_idle.h", src / "m0_idle.h");

        /* the recompiled 65C816 code: the reset vector, the profile, what
         * they reach under the contexts the walk can prove */
        const bool native = !o.interpret;
        if (native) {
            EmitOptions emit;
            emit.rom_bytes = uint32_t(image_bytes);
            emit.hirom = hirom;
            emit.precise = o.precise;
            const unsigned bpi = o.bytes_per_instruction ? o.bytes_per_instruction : 24u;
            emit.max_instructions = o.code_budget_bytes ? o.code_budget_bytes / bpi : 0;
            EmitStats es;
            write(src / "snes_gen.c", emit_code(rom, read_coverage(o.coverage), emit, es));
            stats.compiled = es.compiled;
            stats.covered = es.covered;
            stats.regions = es.regions;
            stats.entries = es.entries;
        }

        /* the harness sits next to src/ in a generated project, not next to
         * ../runtime as in the checkout */
        write(o.output / "host" / "snes_host.c",
              replace(replace(read(tpl / "host" / "snes_host.c"),
                              "#include \"../runtime/snes.h\"", "#include \"../src/snes.h\""),
                      "#ifdef SNES_EMBEDDED", "#include \"../src/snes_config.h\"\n#ifdef SNES_EMBEDDED"));
        const std::string sources = "${SRC}/snes.c ${SRC}/snes_cpu.c ${SRC}/snes_cart.c" +
                                    std::string(native ? " ${SRC}/snes_gen.c" : "");
        write(o.output / "host" / "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nproject(" + o.name + "_snes_host C)\nset(CMAKE_C_STANDARD 11)\n"
              "if(NOT CMAKE_BUILD_TYPE)\n    set(CMAKE_BUILD_TYPE Release)\nendif()\n"
              "set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)\n"
              "add_executable(" + o.name + "_host snes_host.c " + sources + ")\n"
              "target_include_directories(" + o.name + "_host PRIVATE ${SRC})\n"
              "target_compile_definitions(" + o.name + "_host PRIVATE SNES_EMBEDDED SNES_TRACE)\n"
              "target_compile_options(" + o.name + "_host PRIVATE -w)\ntarget_link_libraries(" + o.name + "_host PRIVATE m)\n");

        write(marker, "desktop\n");
        write(o.output / "README.md",
              "# SNES project: " + o.name + "\n\n" +
              (native ? std::string("The cartridge's 65C816 code recompiled to C (src/snes_gen.c) on the\n"
                                    "5A22 runtime, the interpreter for the rest.  A profile: host/build/" + o.name +
                                    "_host --interp --coverage cov.txt\n"
                                    "(then gbrecomp --system snes ... --coverage cov.txt).\n\n")
                      : std::string("The cartridge interpreted on the 5A22 runtime.\n\n")) +
              "Build: cmake -G Ninja -S host -B host/build && ninja -C host/build\n"
              "Run: host/build/" + o.name + "_host --frames N [--input script.txt] [--interp] [--coverage f]\n");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace m0boy::snes
