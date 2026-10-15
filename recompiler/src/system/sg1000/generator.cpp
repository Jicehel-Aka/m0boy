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

/* What the profiled runs saw.  "rated" when every run says how many frames
 * it lasted ("# frames N"): the rates are then executions and ROM sector
 * reads per frame, averaged over the runs. */
struct Profile {
    std::vector<Loc> hot;                 ///< hottest first
    std::map<Loc, uint32_t> hits;         ///< executions summed over sessions
    std::map<Loc, double> rate;           ///< executions per frame
    std::map<uint32_t, double> sector_rate; ///< fraction of the frames reading each ROM sector as data
    std::vector<Loc> entries;
    std::map<uint32_t, uint32_t> reads;   ///< frames reading each ROM sector, summed over the runs
    bool cart_ram = false;
    bool rated = false;
};

/* Hottest first, across files; "# cart-ram" when the profiled run wrote to
 * 8000-BFFF (enabled it, Game Gear); "# entry XXXX": a dynamic jump target.
 * Game Gear locations are page << 16 | pc.
 *
 * A line is "XXXX" (files ordered hottest first) or "XXXX count" (executions
 * in an interpreted run).  When every file has counts, each run weighs the
 * same: an instruction ranks by its executions per frame ("# frames N"),
 * else by its share of the run's executions, averaged over the runs, so the
 * code of a long game session outranks the logos and menus that every run
 * repeats.  Otherwise the first occurrence wins. */
Profile read_coverage(const std::vector<fs::path>& files, bool gamegear, bool sms) {
    Profile profile;
    std::set<Loc> seen;
    std::map<Loc, double> score;
    bool counted = true, rated = true;
    const unsigned long limit = gamegear ? 0xffffff : 0xffff;
    const std::string base = sms ? "sms" : gamegear ? "gg" : "sg1000";
    for (const auto& path : files) {
        std::ifstream file(path);
        std::string line;
        if (!file || !std::getline(file, line) ||
            (line != "# m0boy." + base + ".coverage.v1" && line != "# m0boy." + base + ".coverage.v2"))
            throw std::runtime_error("Expected m0boy." + base + ".coverage.v[12]: " + path.string());
        std::vector<std::pair<Loc, uint64_t>> run;
        std::vector<std::pair<uint32_t, uint32_t>> run_reads;
        uint64_t total = 0, frames = 0;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (line == "# cart-ram") { profile.cart_ram = true; continue; }
            if (line.rfind("# entry ", 0) == 0) { /* target of a RET or JP (HL) */
                size_t used = 0;
                const unsigned long value = std::stoul(line.substr(8), &used, 16);
                if (used != line.size() - 8 || value > limit) throw std::runtime_error("Invalid coverage entry");
                profile.entries.push_back(static_cast<Loc>(value));
                continue;
            }
            if (line.rfind("# frames ", 0) == 0) { /* length of the run */
                size_t used = 0;
                frames = std::stoull(line.substr(9), &used, 10);
                if (used != line.size() - 9) throw std::runtime_error("Invalid coverage frames");
                continue;
            }
            if (line.rfind("# read ", 0) == 0) { /* ROM sector read as data: frames */
                std::istringstream in(line.substr(7));
                uint32_t sector = 0, count = 0;
                if (in >> std::hex >> sector >> std::dec >> count) { profile.reads[sector] += count; run_reads.push_back({ sector, count }); }
                continue;
            }
            if (line[0] == '#') continue;
            size_t used = 0;
            unsigned long value = std::stoul(line, &used, 16);
            uint64_t count = 0;
            if (used != line.size()) { /* " count" */
                size_t used_count = 0;
                if (line[used] != ' ') throw std::runtime_error("Invalid coverage address");
                count = std::stoull(line.substr(used + 1), &used_count, 10);
                if (used + 1 + used_count != line.size() || !count) throw std::runtime_error("Invalid coverage count");
            }
            if (value > limit) throw std::runtime_error("Invalid coverage address");
            if (!count) counted = false;
            uint32_t& sum = profile.hits[static_cast<Loc>(value)];
            const uint64_t executions = count ? count : 1;
            sum = executions > 0xffffffffu - sum ? 0xffffffffu : sum + static_cast<uint32_t>(executions);
            total += count;
            run.push_back({ static_cast<Loc>(value), count });
            if (seen.insert(static_cast<Loc>(value)).second) profile.hot.push_back(static_cast<Loc>(value));
        }
        if (!file.eof()) throw std::runtime_error("Cannot read coverage");
        if (!frames) rated = false;
        if (counted && total) for (const auto& [loc, count] : run) {
            score[loc] += static_cast<double>(count) / static_cast<double>(total);
            if (frames) profile.rate[loc] += static_cast<double>(count) / static_cast<double>(frames);
        }
        if (frames) for (const auto& [sector, count] : run_reads) profile.sector_rate[sector] += static_cast<double>(count) / static_cast<double>(frames);
    }
    profile.rated = counted && rated && !files.empty();
    if (profile.rated) {
        for (auto& [loc, r] : profile.rate) r /= static_cast<double>(files.size());
        for (auto& [sector, r] : profile.sector_rate) r /= static_cast<double>(files.size());
        std::stable_sort(profile.hot.begin(), profile.hot.end(), [&](Loc a, Loc b) { return profile.rate[a] > profile.rate[b]; });
    } else if (counted && !score.empty())
        std::stable_sort(profile.hot.begin(), profile.hot.end(), [&](Loc a, Loc b) { return score[a] > score[b]; });
    return profile;
}

/* Flash for generated code: the application area, less the runtime and
 * drivers (measured at about 24 KiB) and the ROM image. */
constexpr size_t runtime_bytes = 24 * 1024;
constexpr size_t bytes_per_instruction = 32; /* measured 13-27 with GCC 7.2 -O2 */
constexpr size_t gg_bytes_per_instruction = 36; /* Game Gear: paged reads, measured 28-33 */
constexpr double gg_interpreted_cycles = 300;   /* an interpreted instruction costs about this much more than a compiled one (gm0 profiles) */
constexpr double gg_sector_miss_cycles = 50000; /* a ROM sector read from the SD card, assumed 0.7 ms at 72 MHz (not measured on a console) */
constexpr size_t gg_pool_max = 254;             /* the pool's index is a byte, 255 meaning "not in flash" */

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
        const Profile profile = read_coverage(o.coverage, gg, o.mastersystem);
        const auto& hot = profile.hot;
        const auto& entries = profile.entries;
        const auto& reads = profile.reads;
        bool cart_ram = profile.cart_ram;
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
        /* Game Gear ROMs larger than the flash leave some of it on the card.
         * With a rated profile, page 0 (the fixed KiB, always) stays and the
         * rest of the flash goes to the instructions compiled and the ROM
         * sectors kept (the pool, found by sector number without the SD
         * card) that save the most cycles per byte.  Without rates, the code
         * gets what its played instructions need (at most half) and the
         * pages most read as data fill the rest. */
        std::vector<int> page_index; /* flash slot of each page, -1: card */
        std::vector<size_t> pool;    /* ROM sectors kept in flash outside whole pages */
        const size_t pages = rom_size / 0x4000;
        const size_t per_instruction = o.bytes_per_instruction ? o.bytes_per_instruction
                                       : gg ? gg_bytes_per_instruction : bytes_per_instruction;
        if (o.target != TargetId::desktop) {
            const size_t area = o.target == TargetId::pokitto ? 0x39000 : 240 * 1024;
            const size_t free = area > runtime_bytes ? area - runtime_bytes : 0;
            const size_t code_need = std::min(hot.size() * per_instruction + 16 * 1024, free / 2);
            if (gg && rom_size + code_need > free && profile.rated) {
                /* Page 0 stays; the rest of the flash goes to what saves the most
                 * cycles per byte, from the profile's rates: an instruction
                 * compiled, or a ROM sector kept (the pool).  A sector is read
                 * as data, and also for the code of the instructions left to
                 * the interpreter, which depend on what is compiled: the
                 * choice is made again with the sectors those leave. */
                const size_t index_bytes = pages * 32; /* the pool's index, a byte per ROM sector */
                auto sector_of = [&](Loc l) -> int64_t { /* ROM sector of an instruction, -1: not in the ROM */
                    const uint32_t pc = l & 0xffff, page = l >> 16;
                    if (pc >= 0xc000) return -1;
                    const uint32_t offset = pc < 0x400 && page == 0 ? pc : page * 0x4000u + (pc & 0x3fffu);
                    return offset / 512 < pages * 32 ? static_cast<int64_t>(offset / 512) : -1;
                };
                std::vector<double> data_rate(pages * 32, 0.0), code_rate(pages * 32, 0.0);
                for (const auto& [sector, rate] : profile.sector_rate) if (sector < data_rate.size()) data_rate[sector] = std::min(rate, 1.0);
                size_t compiled = 0;
                for (int pass = 0; pass < 3; pass++) {
                    struct Candidate { double density; bool sector; size_t id; };
                    std::vector<Candidate> candidates;
                    for (size_t k = 0; k < hot.size(); k++)
                        candidates.push_back({ profile.rate.at(hot[k]) * gg_interpreted_cycles / static_cast<double>(per_instruction), false, k });
                    for (size_t sector = 32; sector < data_rate.size(); sector++) {
                        const double rate = 1.0 - (1.0 - data_rate[sector]) * (1.0 - code_rate[sector]);
                        if (rate > 0) candidates.push_back({ rate * gg_sector_miss_cycles / 512.0, true, sector });
                    }
                    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.density > b.density; });
                    size_t room = free > 0x4000 ? free - 0x4000 : 0;
                    pool.clear();
                    compiled = 0;
                    for (const auto& c : candidates) {
                        if (c.density <= 0) break;
                        if (c.sector) {
                            const size_t cost = 512 + (pool.empty() ? index_bytes : 0);
                            if (pool.size() < gg_pool_max && room >= cost + per_instruction) { pool.push_back(c.id); room -= cost; }
                        } else if (room >= per_instruction) { room -= per_instruction; compiled++; }
                    }
                    /* the instructions not compiled run in the interpreter, which
                     * fetches their bytes: the odds that a frame touches a sector */
                    std::vector<double> miss(pages * 32, 1.0);
                    for (size_t k = compiled; k < hot.size(); k++) {
                        const int64_t sector = sector_of(hot[k]);
                        if (sector >= 32) miss[static_cast<size_t>(sector)] *= 1.0 - std::min(profile.rate.at(hot[k]), 1.0);
                    }
                    for (size_t sector = 0; sector < code_rate.size(); sector++) code_rate[sector] = 1.0 - miss[sector];
                }
                page_index.assign(pages, -1);
                page_index[0] = 0;
                std::sort(pool.begin(), pool.end());
                size_t pool_bytes = pool.size() * 512 + (pool.empty() ? 0 : index_bytes);
                budget = free - 0x4000 - pool_bytes;
            } else if (gg && rom_size + code_need > free) {
                /* a profile without rates (older runs): the code takes at most
                 * half of the flash, the pages most read as data fill the rest */
                struct Page { uint64_t score; size_t page; };
                std::vector<Page> score;
                for (size_t p = 1; p < pages; p++) {
                    uint64_t sum = 0;
                    for (uint32_t s = static_cast<uint32_t>(p * 32); s < (p + 1) * 32; s++)
                        if (auto it = reads.find(s); it != reads.end()) sum += it->second;
                    score.push_back({ sum, p });
                }
                std::stable_sort(score.begin(), score.end(), [](auto& a, auto& b) { return a.score > b.score; });
                const size_t resident = std::max<size_t>(1, (free - code_need) / 0x4000);
                page_index.assign(pages, -1);
                page_index[0] = 0;
                int next = 1;
                for (size_t k = 0; k + 1 < resident && k < score.size(); k++) page_index[score[k].page] = next++;
                budget = free - static_cast<size_t>(next) * 0x4000;
            } else budget = free > rom_size ? free - rom_size : 0;
        }
        if (o.code_budget_bytes) budget = o.code_budget_bytes;
        const bool streamed = !page_index.empty();
        if (budget) eo.max_instructions = budget / per_instruction;
        EmitStats es;
        std::vector<HotLoc> hot_counts;
        hot_counts.reserve(hot.size());
        for (Loc loc : hot) hot_counts.push_back({ loc, profile.hits.at(loc) });
        write(src / "sg_gen.c", emit_code(rom, hot_counts, eo, es));
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
            config << "#define GG_SAVE_NAME \"ROM.SAV\"\n#define GG_SAVE_NAME83 \"ROM     SAV\"\n";
        }
        /* Pokitto: the sector cache lives in the 2 KiB USB RAM bank, which
         * the Master System shares with its pair tables. */
        if (gg && (streamed || cart_on_card) && o.target == TargetId::pokitto && !o.mastersystem)
            config << "#define GG_STREAM_SECTORS 4 /* the whole USB RAM bank */\n";
        if (gg && streamed && !pool.empty()) config << "#define GG_POOL_SECTORS " << pool.size() << " /* hot ROM sectors in flash */\n";
        if (gg && streamed) {
            const std::string ext = o.mastersystem ? "SMS" : "GG";
            config << "#define GG_STREAM_NAME \"ROM." << ext << "\"\n#define GG_STREAM_NAME83 \"ROM     "
                   << (o.mastersystem ? std::string("SMS") : std::string("GG ")) << "\"\n";
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
            if (!pool.empty()) {
                std::vector<int> slot(pages * 32, 255);
                for (size_t k = 0; k < pool.size(); k++) slot[pool[k]] = static_cast<int>(k);
                data << "const uint8_t gg_pool_index[" << slot.size() << "] = {";
                for (size_t k = 0; k < slot.size(); k++) data << (k % 32 ? "" : "\n") << slot[k] << ",";
                data << "\n};\nconst uint8_t gg_pool[" << pool.size() * 512 << "] = {";
                count = 0;
                for (size_t sector : pool)
                    for (size_t b = 0; b < 512; b++) data << (count++ % 32 ? "" : "\n") << unsigned(rom[sector * 512 + b]) << ",";
                data << "\n};\n";
            }
            stats.streamed_pages = pages - kept;
            stats.pool_sectors = pool.size();
        } else {
            for (auto byte : rom) data << (count++ % 32 ? "" : "\n") << unsigned(byte) << ",";
            data << "\n};\nconst size_t sg_rom_size = sizeof(sg_rom);\n";
        }
        write(src / "sg_rom.c", data.str());

        for (const auto& entry : fs::directory_iterator(tpl / "runtime"))
            if (entry.is_regular_file()) copy_template(entry.path(), src / entry.path().filename());
        if (gg) /* the Z80 core is the SG-1000 runtime's */
            for (const char* name : { "z80.c", "z80.h", "z80_ops.h", "z80_fast.h" }) copy_template(shared / "runtime" / name, src / name);
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
            /* The META's board settings (SPI rates, display pipeline): the
             * same file the Game Boy profile includes. */
            copy_template(o.meta_templates / "runtime" / "gbl_board.h", src / "gbl_board.h");
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
            /* gbl_board.h carries the META's SPI rates and display pipeline
             * switches, shared with the Game Boy profile; only what this
             * build decides differently is written here. */
            write(src / "gbl_profile.h", "#pragma once\n#include \"gbl_board.h\"\n"
                                         "#ifndef META_OVERCLOCK\n#if defined(POKITTO_HC) && POKITTO_HC\n#define META_OVERCLOCK 1\n#else\n#define META_OVERCLOCK 0\n#endif\n#endif\n"
                                         "#define POKITTO_LCD_ASM " + std::string(gg ? "1" : "0") + "\n#define POKITTO_RAM_BANKS " + std::string(gg ? "1" : "0") + "\n#define POKITTO_AUDIO_ASM " + std::string(gg ? "1" : "0") + "\n#define META_AUDIO_DMA 0\n");
            if (o.target == TargetId::pokitto) {
                for (const char* name : { "pokitto_startup.c", "pokitto_clock.c", "pokitto_buttons.c", "pokitto_lcd.c", "pokitto_hw.h", "lpc11u6x.h" })
                    copy_template(platform / name, src / name);
                if (gg) { /* rows pushed and the audio interrupt in Thumb-1 */
                    copy_template(platform / "pokitto_lcd_asm.S", src / "pokitto_lcd_asm.S");
                    copy_template(platform / "pokitto_audio_asm.S", src / "pokitto_audio_asm.S");
                }
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
