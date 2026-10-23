/* SG-1000 project generation: the recompiled code (emitter.cpp), the
 * cartridge image, the runtime and, for META/Pokitto, the console drivers
 * shared with the Game Boy frontend. */
#include <cctype>
#include "m0boy/sg1000_generator.h"
#include "m0_sega_mapper.h"
#include "m0boy/rom_storage.h"
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
/* 8.3 base of the game's files on the card: the first eight letters and
 * digits of the project name, upper case. */
std::string sd_base(const std::string& project) {
    std::string n;
    for (char c : project) {
        if (std::isalnum(static_cast<unsigned char>(c))) n += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (n.size() == 8) break;
    }
    return n.empty() ? "ROM" : n;
}

/* "NAME    EXT" as a FAT directory entry holds it. */
std::string name83(std::string base, std::string ext) {
    base.resize(8, ' ');
    ext.resize(3, ' ');
    return base + ext;
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
Profile read_coverage(const std::vector<fs::path>& files, bool gamegear, bool sms, unsigned bank_shift) {
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
        bool bank_format = bank_shift == 14;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (line.rfind("# bank-shift ", 0) == 0) {
                if (std::stoul(line.substr(13)) != bank_shift)
                    throw std::runtime_error("Coverage mapper differs: replay " + path.string());
                bank_format = true;
                continue;
            }
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
        if (!bank_format) throw std::runtime_error("Old coverage lacks the 8 KiB mapper: replay " + path.string());
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
constexpr size_t runtime_bytes = 30 * 1024; /* drivers, runtime and, since 0.10.28, the option
                                            * menu (meta_menu.cpp, meta_font.h, sg_menu.h: about 6 KiB — left
                                            * unbudgeted it pushed the hot instructions to the interpreter:
                                            * Ristar's first level at 16 fps on the device, 2026-10-20) */
constexpr size_t bytes_per_instruction = 32; /* measured 13-27 with GCC 7.2 -O2 */
constexpr size_t gg_bytes_per_instruction = 36; /* Game Gear: paged reads, measured 28-33 */
constexpr double gg_interpreted_cycles = 300;   /* an interpreted instruction costs about this much more than a compiled one (gm0 profiles) */
constexpr double gg_sector_miss_cycles = rom::sd_cycles; /* a ROM sector read from the SD card (gm0 estimate, rom_storage.h) */
constexpr size_t gg_pool_max = 254;             /* the pool's index is a byte, 255 meaning "not in flash" */
/* ESPboy (4 MiB flash, targets/meta/runtime/gbl_espboy.h): the application
 * is the 1 MiB the CPU maps; a Game Gear / Master System ROM above esp_embed
 * keeps page 0 there and the rest, raw, in rom.bin read with spi_flash_read. */
constexpr size_t esp_app = 0x100000, esp_runtime = 0x5C000, esp_embed = 0x40000, esp_rom_bytes = 0x2C0000;

} // namespace

bool generate(const fs::path& path, const GenerateOptions& o, GenerateStats& stats, std::string& error) {
    const std::string sd = o.sd_name.empty() ? sd_base(o.name) : o.sd_name;
    error.clear();
    const bool gg = o.gamegear;
    std::vector<uint8_t> rom;
    if (gg) { /* whole 16 KiB pages, padded with FF */
        std::ifstream file(path, std::ios::binary);
        if (!file) { error = "Cannot read Game Gear cartridge: " + path.string(); return false; }
        rom.assign(std::istreambuf_iterator<char>(file), {});
        /* copier header, same rule for Game Gear and Master System dumps
         * (m0boy/sg1000.h; the card copy compile.sh writes is stripped the
         * same way, so their pages stay aligned) */
        rom.erase(rom.begin(), rom.begin() + static_cast<std::ptrdiff_t>(copier_header_skip(rom.size())));        if (rom.size() < 0x400 || rom.size() > 0x400000) { error = "Game Gear cartridge must contain 1 KiB to 4 MiB"; return false; }
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
        /* Files both consoles share (CMake templates, host helpers, frontend
         * pieces, the generated-code interface sg_gen.h). */
        const auto common_dir = o.meta_templates.parent_path() / "common";
        if (!fs::is_regular_file(common_dir / "runtime" / "sg_gen.h"))
            throw std::runtime_error(system_name + " templates missing next to targets/meta");
        const unsigned mapper = gg ? m0_sega_mapper(rom.data(), rom.size()) : 0;
        const unsigned bank_shift = mapper == M0_MAPPER_KOREAN150 ? 13 : 14;
        const Profile profile = read_coverage(o.coverage, gg, o.mastersystem, bank_shift);
        const auto& hot = profile.hot;
        const auto& entries = profile.entries;
        const auto& reads = profile.reads;
        bool cart_ram = profile.cart_ram;
        const size_t rom_size = rom.size();
        /* consoles: no room for 8 KiB more, the RAM lives in a save file */
        const bool cart_on_card = gg && cart_ram && o.target != TargetId::desktop && o.target != TargetId::espboy;
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
        const bool esp = o.target == TargetId::espboy;
        const std::string target = o.target == TargetId::meta ? "meta" : o.target == TargetId::pokitto ? "pokitto" : esp ? "espboy" : "desktop";
        if (fs::exists(marker) && read(marker) != target + "\n")
            throw std::runtime_error("Use a separate output directory for each destination");
        fs::create_directories(o.output / "src");
        fs::create_directories(o.output / "host");
        const auto src = o.output / "src";

        EmitOptions eo;
        eo.precise = o.precise;
        eo.sync_cycles = o.sync_cycles;
        eo.cart_ram = cart_ram;
        eo.mastersystem = o.mastersystem;
        eo.aggressive_scan = o.aggressive_scan;
        eo.infer_data_zones = o.infer_data_zones;
        eo.entries.assign(entries.begin(), entries.end());
        /* Codemasters boards: checksum and its complement at 7FE6/7FE8 */
        if (gg) {
            eo.banked = true; eo.fixed_kib = mapper == M0_MAPPER_SEGA;
            eo.bank_shift = bank_shift; eo.io_out = "gg_out"; eo.io_in = "gg_in";
            /* A mapper write can move every slot, including the vectors. */
            if (mapper == M0_MAPPER_KOREAN150) eo.sync_stores = true;
        }
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
        std::vector<rom::Block> pool_blocks;
        std::vector<rom::Block> packed_rom;
        size_t fill_sectors = 0; /* pool sectors added by o.fill_bytes, outside the code budget */
        bool packed_all = false;
        const size_t pages = rom_size / 0x4000;
        const size_t per_instruction = o.bytes_per_instruction ? o.bytes_per_instruction
                                       : gg ? gg_bytes_per_instruction : bytes_per_instruction;
        if (esp) {
            const size_t free = esp_app > esp_runtime ? esp_app - esp_runtime : 0;
            if (gg && rom_size > esp_embed) {
                if (rom_size - 0x4000 > esp_rom_bytes) {
                    error = "ROM does not fit the ESPboy's 2.75 MiB ROM area";
                    return false;
                }
                page_index.assign(pages, -1);
                page_index[0] = 0;
                budget = free > 0x4000 ? free - 0x4000 : 0;
            } else budget = free > rom_size ? free - rom_size : 0;
        } else if (o.target != TargetId::desktop) {
            const size_t area = o.target == TargetId::pokitto ? 0x39000 : 240 * 1024;
            const size_t free = area > runtime_bytes ? area - runtime_bytes : 0;
            const size_t all_code = o.code_budget_bytes ? o.code_budget_bytes : hot.size() * per_instruction + 16 * 1024;
            const size_t code_need = std::min(all_code, free / 2);
            const bool raw_fits = pages == 1 || rom::fits_raw(area, runtime_bytes, rom_size, all_code);
            /* Like GB --meta-pack: try all cold ROM in flash before requiring
             * a card. Every sector keeps its independent raw/LZSS choice. */
            if (gg && !raw_fits) {
                size_t packed_bytes = 0;
                packed_rom.resize(pages * 32);
                for (size_t sector = 32; sector < pages * 32; ++sector) {
                    packed_rom[sector] = rom::encode(rom.data() + sector * 512, 512);
                    packed_bytes += packed_rom[sector].bytes.size();
                }
                const size_t metadata = pages * 32 * 2 + (pages * 32 - 32 + 1) * 4 + 2;
                if (rom::fits_raw(area, runtime_bytes + rom::lzss_decoder_bytes + metadata + 0x4000,
                                  packed_bytes, all_code)) {
                    std::vector<size_t> sizes(pages * 32, 512);
                    std::vector<double> weights(pages * 32, 0);
                    for (size_t s = 32; s < sizes.size(); ++s) sizes[s] = packed_rom[s].bytes.size();
                    for (const auto& [s, count] : reads) if (s < weights.size()) weights[s] = static_cast<double>(count);
                    const size_t extra = free - rom::lzss_decoder_bytes - metadata - 0x4000 - packed_bytes - all_code;
                    for (size_t s : rom::raw_units(sizes, weights, 1, 32, rom_size, extra)) {
                        auto& block = packed_rom[s];
                        packed_bytes += 512 - block.bytes.size();
                        block = {rom::Codec::raw, {rom.data() + s * 512, rom.data() + (s + 1) * 512}};
                    }
                    packed_all = true;
                    page_index.assign(pages, -1); page_index[0] = 0;
                    for (size_t sector = 32; sector < pages * 32; ++sector) {
                        pool.push_back(sector);
                        pool_blocks.push_back(std::move(packed_rom[sector]));
                    }
                    budget = free - 0x4000 - packed_bytes - metadata - rom::lzss_decoder_bytes;
                }
            }
            if (packed_all) { /* already planned above */ }
            else if (gg && !raw_fits && profile.rated) {
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
                    const uint32_t offset = eo.fixed_kib && pc < 0x400 && page == 0 ? pc
                        : (page << bank_shift) + (pc & ((1u << bank_shift) - 1u));
                    return offset / 512 < pages * 32 ? static_cast<int64_t>(offset / 512) : -1;
                };
                std::vector<double> data_rate(pages * 32, 0.0), code_rate(pages * 32, 0.0);
                /* --rom-plan: the SD reads per frame each sector saves when
                 * kept, from the card cache simulated on the sessions' ROM
                 * traces (m0romsim; a block's saving shared by its sectors,
                 * a smaller block's given to its sector) instead of the odds
                 * that a frame reads it: a sector read once a frame but
                 * still cached costs nothing, one read in bursts costs more. */
                const bool planned = !o.rom_plan.empty();
                if (planned) {
                    std::ifstream in(o.rom_plan);
                    if (!in) { error = "Cannot read ROM plan: " + o.rom_plan.string(); return false; }
                    for (std::string line; std::getline(in, line);) {
                        if (line.empty() || line[0] == '#') continue;
                        std::istringstream row(line);
                        uint32_t offset = 0, size = 0;
                        std::string codec;
                        size_t bytes = 0;
                        double saved = 0;
                        if (!(row >> std::hex >> offset >> std::dec >> size >> codec >> bytes >> saved) || !size) continue;
                        const size_t first = offset / 512, last = (offset + size - 1) / 512;
                        for (size_t sector = first; sector <= last && sector < data_rate.size(); sector++)
                            data_rate[sector] += saved / static_cast<double>(last - first + 1);
                    }
                }
                auto& packed = packed_rom;
                std::vector<bool> compressed(pages * 32, false);
                size_t pool_bytes = 0;
                if (!planned)
                    for (const auto& [sector, rate] : profile.sector_rate) if (sector < data_rate.size()) data_rate[sector] = std::min(rate, 1.0);
                size_t compiled = 0;
                for (int pass = 0; pass < 3; pass++) {
                    struct Candidate { double density; bool sector; size_t id; bool packed = false; };
                    std::vector<Candidate> candidates;
                    for (size_t k = 0; k < hot.size(); k++)
                        candidates.push_back({ profile.rate.at(hot[k]) * gg_interpreted_cycles / static_cast<double>(per_instruction), false, k });
                    for (size_t sector = 32; sector < data_rate.size(); sector++) {
                        const double rate = planned ? data_rate[sector] + code_rate[sector]
                                                    : 1.0 - (1.0 - data_rate[sector]) * (1.0 - code_rate[sector]);
                        if (rate > 0) {
                            candidates.push_back({ rom::density(rate, gg_sector_miss_cycles, rom::Codec::raw, 516), true, sector });
                            if (packed[sector].codec != rom::Codec::raw)
                                candidates.push_back({ rom::density(rate, gg_sector_miss_cycles, packed[sector].codec,
                                                                   packed[sector].bytes.size() + 4), true, sector, true });
                        }
                    }
                    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.density > b.density; });
                    size_t room = free > 0x4000 ? free - 0x4000 : 0;
                    pool.clear();
                    std::vector<bool> chosen(pages * 32, false);
                    std::fill(compressed.begin(), compressed.end(), false);
                    bool decoder = false;
                    compiled = 0;
                    for (const auto& c : candidates) {
                        if (c.density <= 0) break;
                        if (c.sector) {
                            if (chosen[c.id]) continue;
                            const size_t cost = (c.packed ? packed[c.id].bytes.size() : 512) + 4 +
                                                (pool.empty() ? index_bytes + 6 : 0) +
                                                (c.packed && !decoder ? rom::lzss_decoder_bytes : 0);
                            if (pool.size() < gg_pool_max && room >= cost + per_instruction) {
                                pool.push_back(c.id); room -= cost; chosen[c.id] = true;
                                compressed[c.id] = c.packed;
                                decoder = decoder || c.packed;
                            }
                        } else if (room >= per_instruction) { room -= per_instruction; compiled++; }
                    }
                    /* Packing is not an end in itself: spend any otherwise
                     * unused flash making the hottest selected blocks direct
                     * reads again, without evicting native code or ROM. */
                    std::vector<size_t> selected_sizes(packed.size(), 512);
                    std::vector<double> selected_weights(packed.size(), 0);
                    for (size_t sector : pool) if (compressed[sector]) {
                        selected_sizes[sector] = packed[sector].bytes.size();
                        selected_weights[sector] = data_rate[sector] + code_rate[sector];
                    }
                    for (size_t sector : rom::raw_units(selected_sizes, selected_weights, 1, 32, pool.size() * 512, room)) {
                        compressed[sector] = false;
                        room -= 512 - packed[sector].bytes.size();
                    }
                    /* the instructions not compiled run in the interpreter, which
                     * fetches their bytes: the odds that a frame touches a sector */
                    std::vector<double> miss(pages * 32, 1.0);
                    for (size_t k = compiled; k < hot.size(); k++) {
                        const int64_t sector = sector_of(hot[k]);
                        if (sector >= 32) miss[static_cast<size_t>(sector)] *= 1.0 - std::min(profile.rate.at(hot[k]), 1.0);
                    }
                    for (size_t sector = 0; sector < code_rate.size(); sector++) code_rate[sector] = 1.0 - miss[sector];
                    pool_bytes = free - 0x4000 - room - compiled * per_instruction;
                }
                /* Flash the measured build left over (--meta-fill-bytes): more
                 * ROM sectors, raw, the most read first then in ROM order. The
                 * code budget is untouched, so the compiled code is the same. */
                if (o.fill_bytes) {
                    std::vector<size_t> spare;
                    std::vector<bool> kept(pages * 32, false);
                    for (size_t s : pool) kept[s] = true;
                    for (size_t s = 32; s < pages * 32; s++) if (!kept[s]) spare.push_back(s);
                    std::stable_sort(spare.begin(), spare.end(), [&](size_t a, size_t b) {
                        return data_rate[a] + code_rate[a] > data_rate[b] + code_rate[b];
                    });
                    size_t left = o.fill_bytes;
                    for (size_t s : spare) {
                        if (pool.size() >= gg_pool_max || left < 512 + 4) break;
                        pool.push_back(s); left -= 512 + 4;
                        fill_sectors++;
                    }
                }
                page_index.assign(pages, -1);
                page_index[0] = 0;
                std::sort(pool.begin(), pool.end());
                for (size_t sector : pool) {
                    if (compressed[sector]) pool_blocks.push_back(std::move(packed[sector]));
                    else pool_blocks.push_back({rom::Codec::raw, {rom.data() + sector * 512, rom.data() + (sector + 1) * 512}});
                }
                budget = free - 0x4000 - pool_bytes;
            } else if (gg && !raw_fits) {
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
            if (!pool.empty() && std::none_of(pool_blocks.begin(), pool_blocks.end(), [](const auto& b) { return b.codec != rom::Codec::raw; }))
                budget = free - 0x4000 - (pool.size() - fill_sectors) * 512 - pages * 32;
        }
        const bool pool_packed = std::any_of(pool_blocks.begin(), pool_blocks.end(), [](const auto& b) { return b.codec != rom::Codec::raw; });
        if (o.code_budget_bytes) budget = o.target == TargetId::desktop ? o.code_budget_bytes : std::min(budget, o.code_budget_bytes);
        const bool streamed = !page_index.empty();
        if (budget) eo.max_instructions = budget / per_instruction;
        EmitStats es;
        std::vector<HotLoc> hot_counts;
        hot_counts.reserve(hot.size());
        for (Loc loc : hot) hot_counts.push_back({ loc, profile.hits.at(loc) });
        write(src / "sg_gen.c", emit_code(rom, hot_counts, eo, es));
        { /* the instructions the firmware runs natively: their fetches never
           * reach the ROM (tools/profile traces the others, --rom-trace-skip) */
            std::ostringstream list;
            list << "# m0boy compiled locations (page << 16 | address), hex\n" << std::hex;
            for (uint32_t loc : es.locations) list << loc << "\n";
            write(o.output / "compiled.txt", list.str());
        }
        stats.covered = es.covered;
        stats.compiled = es.compiled;
        stats.speculative = es.speculative;
        stats.swept = es.swept;
        stats.data_zones = es.data_zones;
        stats.data_bytes = es.data_bytes;
        stats.regions = es.regions;
        stats.entries = es.entries;
        stats.cart_ram = cart_ram;

        const bool pow2 = rom_size && !(rom_size & (rom_size - 1));
        std::ostringstream config;
        /* Renderer loops in SRAM (no flash wait states) on the Pokitto; in
         * flash on the META, where code run from SRAM faulted with the CPU
         * overclocked (device, 2026-10-17; the overclock is the default). */
        const char* code_in_sram = o.target == TargetId::pokitto ? "#define SG_CODE_IN_SRAM 1\n" : "#define SG_CODE_IN_SRAM 0\n";
        /* The ROM's identity in the ESPboy's save area. */
        uint32_t game_id = 2166136261u;
        for (uint8_t byte : rom) game_id = (game_id ^ byte) * 16777619u;
        const std::string esp_id = esp ? "#define SG_FLASH_GAME_ID " + std::to_string(game_id) + "u\n" : std::string();
        const bool swap_panel = o.target == TargetId::meta || esp; /* high byte first on the SPI */
        if (gg) config << "/* Generated by m0recomp --system gg. */\n#pragma once\n" << code_in_sram
               << "#define SG_SYSTEM_GG 1 /* sg_gen.h: paged ROM, ports through gg_write */\n"
               << "#define SG_ROM_SIZE " << rom_size << "u\n"
               << "#define SG_CART_RAM " << (cart_ram ? 1 : 0) << "\n"
               << "#define SG_PRECISE " << (eo.precise ? 1 : 0) << "\n"
               << "#define GG_PANEL_SWAP " << (swap_panel ? 1 : 0) << " /* META SPI: high byte first */\n"
               << esp_id << (esp ? "#define GG_CART_RAM_8K 1 /* ESPboy: 8 KiB in RAM, mirrored */\n" : "")
               << "#define SG_ROM_STREAM " << (streamed ? 1 : 0) << "\n"
               << "#define GG_MAPPER " << mapper << " /* 0 Sega, 1 Codemasters, 2 Korean 150-in-1 */\n"
               << "#define GG_CART_SD " << (cart_on_card ? 1 : 0) << " /* cartridge RAM in the save file */\n"
               << "#define GG_SMS " << (o.mastersystem ? 1 : 0) << " /* Master System */\n";
        /* The game's files on the card share the .BIN's 8.3 base; the
         * firmware looks for them in the folder of its own .BIN. */
        if (!esp) config << "#define GG_BIN_NAME83 \"" << name83(sd, "BIN") << "\"\n";
        if (cart_on_card) {
            config << "#define GG_SAVE_NAME \"" << sd << ".SAV\"\n#define GG_SAVE_NAME83 \"" << name83(sd, "SAV") << "\"\n";
        }
        /* Save states (meta_state.cpp): the file compile.sh creates holds
         * slots of the machine's bytes (gg_t or sg1000_t, the cartridge RAM
         * when the game has some in RAM).  meta_state_init refuses a machine
         * larger than a slot. */
        const bool states = o.target == TargetId::meta || o.target == TargetId::pokitto;
        if (states) {
            const size_t state_bytes = (gg ? 25536u : 18216u) + (cart_ram ? 8192u : 0u); /* sizeof(gg_t), sizeof(sg1000_t) */
            config << "#define GG_STATE_NAME \"" << sd << ".ST8\"\n#define GG_STATE_NAME83 \""
                   << name83(sd, "ST8") << "\"\n#define GG_STATE_SLOTS 10\n#define GG_STATE_SLOT_SECTORS "
                   << (state_bytes + 511) / 512 + 1 << "\n";
        }
        /* ESPboy: the slots that fit in the flash's save area (59 blocks of
         * 4 KiB, espboy_flash.cpp), after the two copies of the cartridge
         * RAM (8 KiB, and a header block each). */
        const bool esp_states = esp;
        if (esp_states) {
            const size_t state_bytes = (gg ? 25536u : 18216u) + (cart_ram ? 8192u : 0u);
            const size_t sectors = 1 + (state_bytes + 511) / 512, slot_blocks = (sectors + 7) / 8;
            const size_t ram_blocks = cart_ram ? 2 * (8192 / 4096 + 1) : 0;
            const size_t slots = std::min<size_t>(10, (0x03B000 / 4096 - ram_blocks) / slot_blocks);
            config << "#define GG_STATE_SLOTS " << slots << " /* in flash */\n#define GG_STATE_SLOT_SECTORS " << sectors << "\n";
        }
        /* Pokitto: the sector cache lives in the 2 KiB USB RAM bank, which
         * the Master System shares with its pair tables. */
        if (gg && (streamed || cart_on_card) && o.target == TargetId::pokitto && !o.mastersystem)
            config << "#define GG_STREAM_SECTORS 4 /* the whole USB RAM bank */\n";
        if (gg && streamed && esp)
            config << "#define GG_STREAM_SECTORS 6\n#define GG_CACHE_BLOCK 512\n";
        /* META: 3 sectors in 64-byte blocks - small blocks cover more
         * sectors at a time, which beats whole-sector blocks: the working
         * set spans more sectors than it fills (device: whole 512-byte
         * blocks froze Ristar, quarters did not; 64-byte blocks loaded
         * 4131 blocks in 150 s of its demo against 7573 with 128, 2026-10-21).
         * With GG_CACHE_DYN the frontend fills the cache at boot (gg.h);
         * without, compile.sh grows GG_STREAM_SECTORS from the RAM report. */
        if (gg && (streamed || cart_on_card) && o.target == TargetId::meta)
            config << "#ifndef GG_STREAM_SECTORS\n#define GG_STREAM_SECTORS 3\n#endif\n#ifndef GG_CACHE_BLOCK\n#define GG_CACHE_BLOCK 64\n#endif\n";
        if (gg) {
            if (streamed && !pool.empty()) config << "#define GG_POOL_SECTORS " << pool.size() << " /* raw/LZSS ROM sectors in flash */\n";
            if (pool_packed) config << "#define GG_POOL_PACKED 1\n";
            if (packed_all) config << "#define GG_ROM_PACKED 1 /* no ROM file required */\n#define GG_POOL_WIDE 1\n";
            if (streamed && !packed_all && !esp) {
                const std::string ext = o.mastersystem ? "SMS" : "GG";
                config << "#define GG_STREAM_NAME \"" << sd << "." << ext << "\"\n#define GG_STREAM_NAME83 \""
                       << name83(sd, ext) << "\"\n";
            }
        }
        else config << "/* Generated by m0recomp --system sg1000. */\n#pragma once\n" << code_in_sram
               << "#define SG_SYSTEM_GG 0 /* sg_gen.h: unbanked ROM, RAM at C000 */\n"
               << "#define SG_ROM_SIZE " << rom_size << "u\n"
               << "#define SG_ROM_MASK " << (pow2 ? rom_size - 1 : 0) << "u\n"
               << "#define SG_CART_RAM " << (cart_ram ? 1 : 0) << "\n"
               << "#define SG_LOW_RAM " << (cart_ram ? 1 : 0) << "\n"
               << "#define SG_PRECISE " << (o.precise ? 1 : 0) << "\n"
               << "#define SG_PANEL_SWAP " << (swap_panel ? 1 : 0) << " /* META SPI: high byte first */\n" << esp_id;
        write(src / "sg_config.h", config.str());
        std::ostringstream data;
        data << "#include <stdint.h>\n#include <stddef.h>\n"
             << (esp ? "#if defined(ESP8266)\n#include <pgmspace.h>\n#define SG_ROM_ATTR PROGMEM __attribute__((aligned(4))) /* read with aligned loads (m0_load_u8) */\n#else\n#define SG_ROM_ATTR\n#endif\nconst uint8_t sg_rom[] SG_ROM_ATTR = {"
                     : "const uint8_t sg_rom[] = {");
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
                std::vector<int> slot(pages * 32, packed_all ? 65535 : 255);
                for (size_t k = 0; k < pool.size(); k++) slot[pool[k]] = static_cast<int>(k);
                data << "const " << (packed_all ? "uint16_t" : "uint8_t") << " gg_pool_index[" << slot.size() << "] = {";
                for (size_t k = 0; k < slot.size(); k++) data << (k % 32 ? "" : "\n") << slot[k] << ",";
                std::vector<uint8_t> blob;
                std::vector<uint32_t> records;
                for (const auto& block : pool_blocks) {
                    records.push_back(static_cast<uint32_t>(blob.size()) | (static_cast<uint32_t>(block.codec) << 30));
                    blob.insert(blob.end(), block.bytes.begin(), block.bytes.end());
                }
                records.push_back(static_cast<uint32_t>(blob.size()));
                data << "\n};\n";
                if (pool_packed) {
                    blob.push_back(0); blob.push_back(0); /* LZSS decoder lookahead */
                    data << "const uint32_t gg_pool_records[" << records.size() << "] = {";
                    for (uint32_t record : records) data << record << "u,";
                    data << "\n};\n";
                }
                data << "const uint8_t gg_pool[" << blob.size() << "] = {";
                count = 0;
                for (uint8_t byte : blob) data << (count++ % 32 ? "" : "\n") << unsigned(byte) << ",";
                data << "\n};\n";
            }
            stats.streamed_pages = pages - kept;
            stats.pool_sectors = pool.size();
            stats.packed = packed_all;
            for (const auto& block : pool_blocks) {
                stats.pool_bytes += block.bytes.size();
                stats.packed_sectors += block.codec != rom::Codec::raw;
            }
        } else {
            for (auto byte : rom) data << (count++ % 32 ? "" : "\n") << unsigned(byte) << ",";
            data << "\n};\nconst size_t sg_rom_size = sizeof(sg_rom);\n";
        }
        write(src / "sg_rom.c", data.str());
        if (esp) { /* ROM pages 1 and up, raw, for spi_flash_read */
            const auto image = o.output / "rom.bin";
            if (streamed) {
                std::ofstream file(image, std::ios::binary);
                file.write(reinterpret_cast<const char*>(rom.data()) + 0x4000, static_cast<std::streamsize>(rom.size() - 0x4000));
                if (!file) throw std::runtime_error("Cannot write the ESPboy ROM image");
            } else fs::remove(image);
        }

        for (const auto& entry : fs::directory_iterator(tpl / "runtime"))
            if (entry.is_regular_file()) copy_template(entry.path(), src / entry.path().filename());
        /* the Z80 engine both consoles run (the memory map differs, see
         * sg_gen.h), and the PSG sample generation sg_audio.c / gg_psg.c
         * include (sg_psg.h) */
        for (const char* name : { "sg_gen.h", "sg_psg.h" })
            copy_template(common_dir / "runtime" / name, src / name);
        if (gg) copy_template(common_dir / "runtime/m0_sega_mapper.h", src / "m0_sega_mapper.h");
        copy_template(common_dir / "runtime/m0_rom_codec.h", src / "m0_rom_codec.h");
        copy_template(common_dir / "runtime/m0_rom_lzss.inc", src / "m0_rom_lzss.inc");
        if (gg) copy_template(common_dir / "runtime/m0_rom_lzss.S", src / "m0_rom_lzss.S");
        if (gg) /* the cache's and the pool's fixed-size copies */
            for (const char* name : { "m0_copy.h", "m0_copy.inc", "m0_copy.S" }) copy_template(common_dir / "runtime" / name, src / name);
        if (gg) { /* the card cache shared with the Game Boy */
            copy_template(common_dir / "runtime/m0_cache.h", src / "m0_cache.h");
            copy_template(common_dir / "runtime/m0_cache.inc", src / "m0_cache.inc");
            copy_template(common_dir / "runtime/m0_stack_pool.h", src / "m0_stack_pool.h");
        }
        if (gg) /* the Z80 core is the SG-1000 runtime's */
            for (const char* name : { "z80.c", "z80.h", "z80_ops.h", "z80_fast.h" }) copy_template(shared / "runtime" / name, src / name);
        const std::string host_c = gg ? "gg_host.c" : "sg_host.c", main_cpp = gg ? "gg_main.cpp" : "sg_main.cpp";
        copy_template(tpl / "host" / host_c, o.output / "host" / host_c);
        /* The CMake files are shared by both consoles (targets/common/cmake),
         * one for the firmware and one for the host harness: only the source
         * lists and the console's name change. */
        const auto cmake_tpl = common_dir / "cmake";
        const std::string console = gg ? "gg" : "sg1000";
        const std::string core_sources = gg ? "${SRC}/z80.c ${SRC}/gg.c ${SRC}/gg_psg.c ${SRC}/gg_vdp.c"
                                            : "${SRC}/z80.c ${SRC}/sg1000.c ${SRC}/sg_audio.c ${SRC}/tms9918.c";
        const std::string generated_sources = "${SRC}/sg_gen.c ${SRC}/sg_rom.c";
        const std::string render_asm = gg ? "gg_render_asm.S" : "sg_render_asm.S";
        const std::string firmware_sources = core_sources + " " + generated_sources + " ${SRC}/" + main_cpp +
                                              " ${SRC}/" + render_asm + (gg ? " ${SRC}/m0_rom_lzss.S ${SRC}/m0_copy.S" : "") +
                                              " ${SRC}/meta_menu.cpp" +
                                              (states ? " ${SRC}/meta_state.cpp" : "");
        std::string host_sources = host_c + " " + core_sources + " " + generated_sources;
        if (gg && (streamed || cart_on_card)) {
            /* A generated host project must exercise the same storage plan,
             * not silently replace it with a fully resident ROM. Only this
             * host translation unit contains the card's complete image. */
            std::ostringstream card;
            card << "#include <stdint.h>\n#include <string.h>\nstatic const uint8_t rom_card[] = {";
            for (size_t i = 0; i < rom.size(); ++i) card << (i % 32 ? "" : "\n") << unsigned(rom[i]) << ',';
            card << "\n};\nint gg_platform_rom_read(uint32_t sector, uint8_t *dst) {\n"
                    " if (sector >= sizeof(rom_card) / 512) return 0;\n"
                    " memcpy(dst, rom_card + (size_t)sector * 512, 512); return 1;\n}\n"
                    "static uint8_t save_card[32768];\n"
                    "int gg_platform_save_read(uint32_t sector, uint8_t *dst) {\n"
                    " if (sector >= 64) return 0;\n memcpy(dst, save_card + sector * 512, 512); return 1;\n}\n"
                    "int gg_platform_save_write(uint32_t sector, const uint8_t *src) {\n"
                    " if (sector >= 64) return 0;\n memcpy(save_card + sector * 512, src, 512); return 1;\n}\n";
            write(o.output / "host" / "rom_card.c", card.str());
            host_sources += " rom_card.c";
        }
        /* The Game Gear streams its ROM: its FAT reader and SD bus join the
         * Pokitto platform files in PLATFORM_C. */
        const std::string platform_extra = gg ? " ${SRC}/pokitto_*.S ${SRC}/gg_stream.c ${SRC}/meta_sd.c ${SRC}/meta_fat.c"
                                              : states ? " ${SRC}/meta_sd.c ${SRC}/meta_fat.c" : "";
        {
            std::string host_cmake = read(cmake_tpl / "host.txt.in");
            host_cmake = replace(host_cmake, "@NAME@", o.name);
            host_cmake = replace(host_cmake, "@SYSTEM@", console);
            host_cmake = replace(host_cmake, "@SOURCES@", host_sources);
            write(o.output / "host" / "CMakeLists.txt", host_cmake);
            /* shared host helpers (the copier-header rule, m0boy/sg1000.h
             * states it for the recompiler) */
            copy_template(common_dir / "host" / "m0_host_util.h", o.output / "host" / "m0_host_util.h");
            copy_template(common_dir / "host" / "m0_rom_trace.h", o.output / "host" / "m0_rom_trace.h");
            copy_template(common_dir / "host" / "m0_explore.h", o.output / "host" / "m0_explore.h"); /* -DM0_ROM_TRACE=ON */
        }

        if (o.target != TargetId::desktop) {
            copy_template(tpl / "platform" / main_cpp, src / main_cpp);
            /* frontend pieces both consoles share (setup, pad, pacing, stack
             * guard, option menu) */
            copy_template(common_dir / "platform" / "sg_frontend.h", src / "sg_frontend.h");
            copy_template(common_dir / "platform" / "sg_menu.h", src / "sg_menu.h");
            /* files of an earlier generation that this one does not use */
            for (const char* name : { "gg_stream.c", "meta_sd.c", "meta_sd.h", "meta_fat.c", "meta_fat.h", "meta_card.h", "meta_sd_bus.cpp", "pokitto_sd.c" })
                fs::remove(src / name);
            if ((streamed || cart_on_card || states) && !esp) { /* gg.h's GG_CARD; SG-1000: the states */
                if (gg) copy_template(tpl / "platform" / "gg_stream.c", src / "gg_stream.c");
                for (const char* name : { "meta_sd.c", "meta_sd.h", "meta_fat.c", "meta_fat.h", "meta_card.h" })
                    copy_template(o.meta_templates / "platform" / "src" / name, src / name);
                const auto bus = o.target == TargetId::pokitto ? fs::path("pokitto/pokitto_sd.c") : fs::path("meta/meta_sd_bus.cpp");
                copy_template(o.meta_templates / "platform" / bus, src / bus.filename());
            }
            auto platform = o.meta_templates / "platform" / (esp ? "espboy" : o.target == TargetId::pokitto ? "pokitto" : "meta");
            for (const char* name : { "meta_target.h", "meta_lcd.h", "meta_clock.h" })
                copy_template(o.meta_templates / "platform" / "src" / name, src / name);
            /* the option menu (sg_menu.h runs it; meta_menu.cpp draws it) */
            copy_template(o.meta_templates / "platform" / "src" / "meta_menu.h", src / "meta_menu.h");
            copy_template(o.meta_templates / "platform" / "src" / "meta_font.h", src / "meta_font.h");
            copy_template(o.meta_templates / "platform" / "src" / "meta_menu.cpp", src / "meta_menu.cpp");
            if (states || esp_states) /* ESPboy: espboy_flash.cpp implements it */
                copy_template(o.meta_templates / "platform" / "src" / "meta_state.h", src / "meta_state.h");
            if (states)
                copy_template(o.meta_templates / "platform" / "src" / "meta_state.cpp", src / "meta_state.cpp");
            /* The META's board settings (SPI rates, display pipeline): the
             * same file the Game Boy profile includes. */
            copy_template(o.meta_templates / "runtime" / "gbl_board.h", src / "gbl_board.h");
            copy_template(tpl / "platform" / "sg_audio.h", src / "sg_audio.h");
            auto audio_header = read(o.meta_templates / "platform/src/meta_audio.h");
            audio_header = replace(replace(audio_header, "70224u", "59736u"), "4194304u", "3579545u");
            write(src / "meta_audio.h", audio_header);
            if (!esp) {
                const std::string audio_name = o.target == TargetId::pokitto ? "pokitto_audio.c" : "meta_audio.cpp";
                auto audio = read(platform / audio_name);
                audio = replace(audio, "#include \"gbl.h\"", "#include \"sg_audio.h\"");
                audio = replace(replace(audio, "gbl_apu_render", "sg_platform_audio_render"), "gbl_apu_set_rate", "sg_platform_audio_rate");
                write(src / audio_name, audio);
            }
            /* One version stamp for all three frontends: the target console
             * runtime's version, compiled into gbrecomp from the VERSION
             * files (targets/meta/VERSION, platform/pokitto/VERSION) by
             * CMake.  gbrecomp --meta-version/--pokitto-version print it and
             * compile.sh reads it back from firmware.bin. */
            write(src / "gbl_config.h", "#pragma once\n#define GBL_TARGET_POKITTO " + std::to_string(o.target == TargetId::pokitto) +
                                        (esp ? "\n#define GBL_TARGET_ESPBOY 1" : "") +
                                        "\n#define GBL_GEN_VERSION \"" +
                                        (esp ? GBRECOMP_ESPBOY_VERSION : o.target == TargetId::pokitto ? GBRECOMP_POKITTO_VERSION : GBRECOMP_META_VERSION) + "\"\n");
            /* gbl_board.h carries the META's SPI rates and display pipeline
             * switches, shared with the Game Boy profile; only what this
             * build decides differently is written here. */
            /* META_OVERCLOCK: on by default on the META (70 MHz, as in the
             * Game Boy profile), off on the Pokitto unless POKITTO_HC. */
            const std::string overclock = o.target == TargetId::pokitto
                ? "#ifndef META_OVERCLOCK\n#if defined(POKITTO_HC) && POKITTO_HC\n#define META_OVERCLOCK 1\n#else\n#define META_OVERCLOCK 0\n#endif\n#endif\n"
                : "#ifndef META_OVERCLOCK\n#define META_OVERCLOCK 1\n#endif\n";
            /* META: one ring of META_AUDIO_RING samples of 2 bytes (512,
             * about 1.4 frames, lowered so the card cache can grow) feeds both
             * output paths, the TC4 interrupt by default and, at run time
             * (the menus' Audio line) or at build (META_AUDIO_DMA=1,
             * compile.sh AUDIO_DMA=1), a DMAC beat.  The Pokitto has its own
             * driver. */
            const std::string audio_dma = o.target == TargetId::meta
                ? std::string("#ifndef META_AUDIO_DMA\n#define META_AUDIO_DMA 0\n#endif\n#ifndef META_AUDIO_RING\n#define META_AUDIO_RING 512u\n#endif\n")
                : "#define META_AUDIO_DMA 0\n";
            write(src / "gbl_profile.h", "#pragma once\n#include \"gbl_board.h\"\n" + overclock +
                                         "#define POKITTO_LCD_ASM " + std::string(gg ? "1" : "0") + "\n#define POKITTO_RAM_BANKS " + std::string(gg ? "1" : "0") + "\n#define POKITTO_AUDIO_ASM " + std::string(gg ? "1" : "0") + "\n" + audio_dma);
            if (esp) {
                /* Arduino core start-up; the drivers and flash code the Game Boy
                 * build uses (they find the Sega frontend by its sg_config.h) */
                for (const char* name : { "espboy_hw.cpp", "espboy_hw.h", "espboy_flash.cpp" })
                    copy_template(platform / name, src / name);
                copy_template(o.meta_templates / "runtime" / "gbl_espboy.h", src / "gbl_espboy.h");
                copy_template(o.meta_templates / "platform" / "src" / "meta_menu.h", src / "meta_menu.h");
                for (const char* name : { "platformio.ini", "espboy.ld", "espboy_build.py", "espboy_size.py" })
                    write(o.output / name, replace(read(platform / (std::string(name) + ".in")), "@NAME@", o.name));
            } else if (o.target == TargetId::pokitto) {
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
                {
                    std::string firmware_cmake = read(cmake_tpl / "firmware.txt.in");
                    firmware_cmake = replace(firmware_cmake, "@NAME@", o.name);
                    firmware_cmake = replace(firmware_cmake, "@SYSTEM@", console);
                    firmware_cmake = replace(firmware_cmake, "@SOURCES@", firmware_sources);
                    firmware_cmake = replace(firmware_cmake, "@PLATFORM_EXTRA@", platform_extra);
                    firmware_cmake = replace(firmware_cmake, "@COMPILER@", o.compiler);
                    write(o.output / "CMakeLists.txt", firmware_cmake);
                }
            } else {
                for (const char* name : { "meta_startup.c", "meta_clock.cpp", "meta_lcd.cpp", "meta_lcd_pack.h", "meta_lcd_queue.h",
                                          "meta_dma.h", "meta_dma.cpp", "meta_spi.c", "meta_hw.h", "meta_usb.c", "meta_usb.h" })
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
              " (" + std::to_string(es.covered) + " played, " + std::to_string(es.swept) +
              " from the speculative sweep), regions: " + std::to_string(es.regions) + ".\n" +
              "Static sweep: " + std::to_string(es.data_zones) + " data zone(s), " +
              std::to_string(es.data_bytes) + " ROM byte(s) never decoded as code.\n\n" +
              "Host: cmake -G Ninja -S host -B host/build && ninja -C host/build\n" +
              "Run: host/build/" + o.name + "_host --frames 300 [--interp] [--shot image.ppm]\n\n" +
              (o.target == TargetId::pokitto ? "Firmware: cmake -G Ninja -S . -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake && ninja -C build\n"
               : o.target == TargetId::meta ? "Firmware: pio run\n" : "") +
              (gg ? std::string(mapper == M0_MAPPER_KOREAN150
                        ? "\nKorean 150-in-1 mapper (six 8 KiB windows), native code synced after stores, 8 KiB RAM, SN76489.\n"
                        : "\nSega/Codemasters mapper (16 KiB pages), 8 KiB RAM, VDP mode 4, SN76489.\n")
                  : std::string("\nBaseline NTSC, unbanked cartridge (optional 8 KiB cartridge RAM), TMS9918A, SN76489.\n")));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace m0boy::sg1000
