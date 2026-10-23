/* m0romsim: the card cache of the META/Pokitto firmwares (m0_cache.inc),
 * replayed on ROM access traces of host runs (--rom-trace-out), what each
 * access costs the firmware, and the choice of the ROM blocks kept in flash
 * that saves the most cycles.
 *
 *   m0romsim [options] ROM TRACE.gz...
 *     --ways N           cache ways (default 12: GG_CACHE_FIXED_BYTES / 64)
 *     --block N          cache block bytes (16 to 512; default 64)
 *     --split C,D[,R]    caches of their own: C ways for interpreted
 *                        instruction bytes, D for data reads, R for
 *                        cartridge RAM (0 or absent: it shares D's); one
 *                        unified cache of --ways otherwise (the firmware's)
 *     --cart-on-card 0|1 the cartridge RAM lives in the card cache (save
 *                        file, GG_CART_SD); default 0: in RAM, not simulated
 *     --budget BYTES     flash for ROM data beyond page 0 (default 0: none)
 *     --sizes a,b,...    candidate block sizes (default 32,...,16384)
 *     --raw-sizes a,b    the sizes a block may be kept raw at (default: all;
 *                        the Game Boy keeps 4 KiB chunks raw, 512-byte
 *                        sectors packed)
 *     --runtime NAME     access costs: "current" (0.10.35's gg_stream_read,
 *                        measured in gm0) or "lean" (fast path apart from the
 *                        miss path, unrolled copies; see Costs)
 *     --costs FILE       cost overrides, "name cycles" per line (the names
 *                        of Costs; m0profile.py calibrate writes them)
 *     --sd-cycles N      a 512-byte SD read (overrides the runtime's)
 *     --lzss-cycles N    an LZSS decode of 512 bytes, likewise
 *     --record-bytes N   flash per kept block, its index entry (default 4)
 *     --fetch 0|1        count interpreted instruction fetches (default 1)
 *     --deflate 0|1      DEFLATE candidates besides LZSS (default 0: the
 *                        Game Gear pool decodes LZSS only)
 *     --sub 0|1          raw blocks under 512 bytes kept in flash, found
 *                        through a second index level (default 0: the
 *                        runtime keeps whole sectors)
 *     --small-lzss 0|1   LZSS blocks under 512 bytes, decoded alone straight
 *                        into a cache block (default 0)
 *     --zero 0|1         32/64-byte blocks without their zero bytes (a mask,
 *                        read in place, no cache; default 0)
 *     --page-direct 0|1  a whole 16 KiB page kept raw is mapped directly
 *                        (no gg_stream_read; default 0: the generator keeps
 *                        sectors in its pool)
 *     --max-frames N     only the first N frames of each trace
 *     --legacy-mru       the cache of firmwares before 0.10.35, which never
 *                        evicted way 0 on a game without cartridge RAM
 *     --plan FILE        write the chosen blocks, in the order chosen:
 *                        "offset size codec flash_bytes saved_per_frame",
 *                        the saving in SD reads' worth of cycles
 *     --ways-curve a,b   the final plan simulated again with these ways
 *     --budget-curve a,b plans for these budgets too (statistics only)
 *     --simulate-plan F  also simulate the blocks of plan F (same format as
 *                        --plan: a generator's choice, to compare with)
 *   JSON statistics on stdout.
 *
 * Every figure is a simulation of the firmware's cache on host traces:
 * accesses, SD reads, decodes and their cycles are estimates, never device
 * measurements.  Costs are extra cycles over a read from a page mapped in
 * flash. */
#include "m0boy/rom_storage.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace rom = m0boy::rom;

constexpr uint32_t granule_bytes = 16; /* trace v2's grain (v1: 32, read as two) */
constexpr uint32_t page_bytes = 0x4000;

/* Where a granule of the ROM is read from. */
enum Residence : uint8_t {
    on_card,      /* the card, through the cache */
    flash_direct, /* a page mapped in flash: no gg_stream_read */
    flash_raw,    /* a raw sector of the pool */
    flash_sub,    /* a raw block under 512 bytes (second index level) */
    flash_zero,   /* a block without its zero bytes, read in place */
    flash_packed, /* packed in the pool, decoded into the cache on a miss */
};
enum class Kind : uint8_t { raw, lzss, deflate, zero };

/* Extra cycles over a read from a page mapped in flash.  "current":
 * gg_stream_read of 0.10.35, instruction by instruction in gm0's EMU_PROF
 * (Ristar, META, 6 000 gm0 frames): every call saves r4-r11 because the
 * miss path is inlined, blocks are copied by newlib-nano's byte memcpy
 * (10.2 cycles a byte).  "lean": the fast path (pool record, block read
 * last) alone in a function that saves nothing, the rest out of line,
 * unrolled ldm/stm copies of each block size (m0_copy.S).  Costs marked
 * "estimate" were not measured: they come from the instructions of the
 * code that would run them. */
struct Costs {
    double call = 8;         /* gg_slot_read's branch to gg_stream_read (page, call): estimate */
    double entry = 62.2;     /* prologue, pool index, epilogue */
    double raw = 11.0;       /* a raw pool sector's byte */
    double sub = 10;         /* second index level, a sub-sector raw block: estimate */
    double zero = 24;        /* a byte of a zero-elided block (mask, popcount table): estimate */
    double last = 32.9;      /* the block read last */
    double staged = 39.7;    /* the staged sector */
    double scan = 81.5;      /* a block found by scanning, plus per_way for each way looked at */
    double per_way = 9.4;
    double miss = 297;       /* victim, tags, counters (not the copy nor the source) */
    double copy_fixed = 14;  /* a block's copy from the staged sector: call */
    double copy_byte = 10.2; /* and per byte */
    double sd = 17254;       /* a 512-byte SD read (card, SPI, FAT; gm0, boot and per-frame SPI set apart) */
    double sd_write = 34508; /* a 512-byte SD write: estimate, twice a read */
    double lzss = 11397;     /* an LZSS decode of 512 bytes */
    double lzss_fixed = 60;  /* a decode's call: estimate */
    double cart = 45;        /* a cartridge RAM byte in its newest block: estimate */
    double cart_scan = 70;   /* found by scanning, plus per_way: estimate */
};

Costs costs(const std::string& runtime) {
    Costs c;
    if (runtime == "lean") {
        /* 0.10.36 (gg_stream_read in assembly, cache_rom_fill and
         * cache_rom_miss apart, m0_copy), per instruction in gm0's EMU_PROF:
         * Ristar, META, the same 5 800 game frames */
        c.entry = 11.3;
        c.raw = 23.4;
        c.last = 29.4;
        c.staged = 59.4;
        c.scan = 80;
        c.per_way = 8.3;
        c.miss = 330;
        c.copy_fixed = 7;
        c.copy_byte = 0.75;
    } else if (runtime != "current") throw std::runtime_error("--runtime: current or lean");
    return c;
}

void load_costs(Costs& c, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) throw std::runtime_error("cannot read " + path);
    const std::map<std::string, double*> names = {
        {"call", &c.call}, {"entry", &c.entry}, {"raw", &c.raw}, {"sub", &c.sub}, {"zero", &c.zero},
        {"last", &c.last}, {"staged", &c.staged}, {"scan", &c.scan}, {"per_way", &c.per_way},
        {"miss", &c.miss}, {"copy_fixed", &c.copy_fixed}, {"copy_byte", &c.copy_byte}, {"sd", &c.sd},
        {"sd_write", &c.sd_write}, {"lzss", &c.lzss}, {"lzss_fixed", &c.lzss_fixed}, {"cart", &c.cart},
        {"cart_scan", &c.cart_scan}};
    char line[256], name[64];
    double v;
    while (std::fgets(line, sizeof line, f))
        if (line[0] != '#' && std::sscanf(line, "%63s %lf", name, &v) == 2) {
            auto it = names.find(name);
            if (it == names.end()) throw std::runtime_error(path + ": unknown cost " + name);
            *it->second = v;
        }
    std::fclose(f);
}

struct Config {
    unsigned ways = 12, block = 64;
    unsigned split[3] = {0, 0, 0}; /* code, data, cartridge RAM ways; code 0: unified */
    size_t record_bytes = 4;
    uint64_t max_frames = 0; /* 0: every frame of each trace */
    std::vector<uint32_t> raw_sizes; /* empty: any */
    bool fetch = true, deflate = false, legacy_mru = false, sub = false, small_lzss = false, zero = false,
         page_direct = false, cart_on_card = false;
    Costs cost;
};

/* One ROM area kept in flash. */
struct Kept {
    uint32_t offset = 0, size = 0;
    Kind kind = Kind::raw;
    size_t bytes = 0; /* flash taken, record included */
    double saved = 0; /* cycles it saved when chosen, per frame, in SD reads */
};

const char* kind_name(Kind k) { return k == Kind::lzss ? "lzss" : k == Kind::deflate ? "deflate" : k == Kind::zero ? "zero" : "raw"; }
Kind kind_of(rom::Codec c) { return c == rom::Codec::lzss ? Kind::lzss : c == rom::Codec::deflate ? Kind::deflate : Kind::raw; }

/* What the simulation counts.  Classes: where each access was served. */
enum Class { c_direct, c_raw, c_sub, c_zero, c_last, c_staged, c_scan, c_miss, c_cart, c_cart_scan, c_cart_miss, classes };
const char* const class_names[classes] = {"flash_direct", "pool_raw", "pool_sub", "pool_zero", "cache_last", "staged",
                                          "cache_scan", "cache_miss", "cart_newest", "cart_scan", "cart_miss"};

struct Stats {
    uint64_t accesses = 0, frames = 0, sd_reads = 0, sd_writes = 0, decodes = 0, cache_misses = 0;
    uint64_t count[classes] = {};
    double access_cycles = 0, miss_cycles = 0, copy_cycles = 0, sd_cycles = 0, decode_cycles = 0, cart_cycles = 0;
    std::vector<uint32_t> per_frame; /* SD reads of each frame */
    std::vector<float> frame_cycles; /* estimated cycles of each frame */
    /* Every SD read stages a sector; while it stays staged the cache serves
     * other bytes of it straight from there.  An episode: the sector and
     * the granules it served (the read's cache block, then the staged
     * hits), sector << 32 | mask -> count.  Keeping a block in flash saves
     * the episodes whose granules it holds all. */
    std::map<uint64_t, uint64_t> episodes;
    /* per granule, for the plan: accesses that did not read a mapped page,
     * the cycles of their path, and of the misses they caused (bookkeeping
     * and copy, not the source) */
    std::vector<uint32_t> g_count;
    std::vector<float> g_path, g_miss;
    double total() const { return access_cycles + miss_cycles + copy_cycles + sd_cycles + decode_cycles + cart_cycles; }
};

/* ---- traces --------------------------------------------------------------- */

class Trace {
public:
    explicit Trace(const std::string& path) : path_(path) {
        file_ = gzopen(path.c_str(), "rb");
        if (!file_) throw std::runtime_error("cannot open trace " + path);
        gzbuffer(file_, 1 << 20);
        char header[96] = {};
        if (!gzgets(file_, header, sizeof header)) throw std::runtime_error(path + ": not an m0boy ROM trace");
        if (!std::strncmp(header, "m0boy.romtrace.v1 ", 18)) rom_bytes_ = std::strtoul(header + 18, nullptr, 10);
        else if (!std::strncmp(header, "m0boy.romtrace.v2 ", 18)) {
            char* end = nullptr;
            rom_bytes_ = std::strtoul(header + 18, &end, 10);
            cart_bytes_ = std::strtoul(end, nullptr, 10);
            v2_ = true;
        } else throw std::runtime_error(path + ": not an m0boy ROM trace");
    }
    ~Trace() { if (file_) gzclose(file_); }
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;
    uint32_t rom_bytes() const { return rom_bytes_; }
    /* Next event: 1 a ROM access (granule, kind 0 data / 1 fetch), 2 a
     * repeat of the access before (n more), 3 a cartridge RAM access
     * (granule, kind 1 a write that changed it), 0 a frame end, -1 the end. */
    int next(uint32_t& granule, unsigned& kind, uint64_t& n) {
        uint64_t v = 0;
        for (unsigned shift = 0;; shift += 7) {
            const int c = byte();
            if (c < 0) return -1;
            v |= static_cast<uint64_t>(c & 127) << shift;
            if (!(c & 128)) break;
            if (shift > 56) throw std::runtime_error(path_ + ": corrupt token");
        }
        if (!v) return 0;
        v -= 1;
        if (!v2_) { /* 32-byte granules: the first of their two */
            kind = static_cast<unsigned>(v & 1);
            last_ += unzigzag(v >> 1);
            granule = static_cast<uint32_t>(last_) * 2;
            return 1;
        }
        const unsigned type = static_cast<unsigned>(v & 3);
        v >>= 2;
        if (type == 2) { n = v; return 2; }
        if (type == 3) {
            kind = static_cast<unsigned>(v & 1);
            cart_last_ += unzigzag(v >> 1);
            granule = static_cast<uint32_t>(cart_last_);
            return 3;
        }
        kind = type;
        last_ += unzigzag(v);
        granule = static_cast<uint32_t>(last_);
        return 1;
    }

private:
    static int64_t unzigzag(uint64_t z) { return (z & 1) ? -static_cast<int64_t>((z + 1) >> 1) : static_cast<int64_t>(z >> 1); }
    int byte() {
        if (pos_ == len_) {
            const int n = gzread(file_, buf_, sizeof buf_);
            if (n <= 0) return -1;
            len_ = static_cast<size_t>(n);
            pos_ = 0;
        }
        return buf_[pos_++];
    }
    std::string path_;
    gzFile file_ = nullptr;
    uint32_t rom_bytes_ = 0, cart_bytes_ = 0;
    bool v2_ = false;
    int64_t last_ = 0, cart_last_ = 0;
    unsigned char buf_[1 << 16];
    size_t pos_ = 0, len_ = 0;
};

/* ---- the firmware's cache (m0_cache.inc) ------------------------------------ */

/* What the one sector buffer holds: a ROM sector (>= 0), a packed unit
 * (-2 - id) or a save file sector (save_base + sector). */
constexpr int64_t save_base = int64_t(1) << 40;

struct Lookup {
    Class cls = c_last;
    unsigned way = 0;    /* scan: the way found (ways looked at: way + 1); miss: all */
    bool source = false; /* a miss that read its source (SD, decode), not the staged sector */
    unsigned writebacks = 0, writeback_reads = 0; /* save sectors written back, read for it */
};

class Cache {
public:
    Cache(unsigned ways, unsigned block, bool legacy_mru, int64_t& staged)
        : ways_(ways), shift_(static_cast<unsigned>(__builtin_ctz(block))), tag_(ways), ref_(ways), dirty_(ways),
          legacy_mru_(legacy_mru), staged_(&staged) { reset(); }
    unsigned shift() const { return shift_; }
    void reset() {
        std::fill(tag_.begin(), tag_.end(), -1);
        std::fill(ref_.begin(), ref_.end(), 0);
        std::fill(dirty_.begin(), dirty_.end(), 0);
        hand_ = 0;
        mru_ = rom_last_ = 0;
        *staged_ = -1;
    }
    /* cache_rom_byte: ROM offset off; unit: what a miss stages (the sector,
     * or a packed unit); alone: decoded straight into the block, no staging */
    Lookup rom(uint32_t off, int64_t unit, bool alone) {
        Lookup r;
        const int64_t b = off >> shift_;
        if (tag_[rom_last_] == b) return r;
        if (shift_ < 9 && !alone && *staged_ == unit) { r.cls = c_staged; return r; }
        unsigned i = 0;
        while (i < ways_ && tag_[i] != b) i++;
        if (i < ways_) { r.cls = c_scan; r.way = i; }
        else {
            r.cls = c_miss;
            r.way = ways_;
            i = victim(r);
            if (alone) r.source = true;
            else if (shift_ >= 9 || *staged_ != unit) { r.source = true; if (shift_ < 9) *staged_ = unit; }
            tag_[i] = b;
        }
        ref_[i] = 1;
        rom_last_ = i;
        return r;
    }
    /* cache_ram_block, then a store when write (the byte changed) */
    Lookup cart(uint32_t off, bool write) {
        Lookup r;
        r.cls = c_cart;
        const int64_t tag = save_base | (off >> shift_);
        unsigned i = mru_;
        if (tag_[i] != tag) {
            for (i = 0; i < ways_ && tag_[i] != tag; i++) {}
            if (i < ways_) { r.cls = c_cart_scan; r.way = i; }
            else {
                r.cls = c_cart_miss;
                r.way = ways_;
                i = victim(r);
                const int64_t sec = save_base + (off >> 9);
                if (shift_ >= 9 || *staged_ != sec) { r.source = true; if (shift_ < 9) *staged_ = sec; }
                tag_[i] = tag;
            }
        }
        ref_[i] = 1;
        mru_ = i;
        if (write) dirty_[i] = 1;
        return r;
    }
    /* gg_save_tick's flush: every dirty block written back */
    void flush(Lookup& r) {
        for (unsigned i = 0; i < ways_; i++) writeback(i, r);
    }

private:
    int64_t save_sector(int64_t tag) const { return save_base + ((tag - save_base) >> (9 - std::min(shift_, 9u))); }
    void writeback(unsigned i, Lookup& r) {
        if (tag_[i] < save_base || !dirty_[i]) return;
        const int64_t sec = save_sector(tag_[i]);
        if (shift_ < 9 && *staged_ != sec) { r.writeback_reads++; *staged_ = sec; }
        r.writebacks++;
        for (unsigned j = 0; j < ways_; j++) /* the sector's other dirty blocks go with it */
            if (tag_[j] >= save_base && save_sector(tag_[j]) == sec) dirty_[j] = 0;
    }
    unsigned victim(Lookup& r) {
        for (;;) {
            const unsigned i = hand_;
            hand_ = i + 1 == ways_ ? 0 : i + 1;
            if (ways_ > 2 && (i == rom_last_ || (i == mru_ && (legacy_mru_ || tag_[i] >= save_base)))) continue;
            if (!ref_[i]) {
                writeback(i, r);
                tag_[i] = -1;
                dirty_[i] = 0;
                return i;
            }
            ref_[i] = 0;
        }
    }
    unsigned ways_, shift_;
    std::vector<int64_t> tag_;
    std::vector<uint8_t> ref_, dirty_;
    unsigned hand_ = 0, mru_ = 0, rom_last_ = 0;
    bool legacy_mru_ = false;
    int64_t* staged_;
};

/* ---- residency -------------------------------------------------------------- */

struct Residency {
    std::vector<uint8_t> where; /* per granule */
    std::vector<int32_t> unit;  /* index of its Kept */
    std::vector<Kept> kept;
    explicit Residency(uint32_t granules) : where(granules, on_card), unit(granules, -1) {}
    void keep(const Kept& k, const Config& cfg) {
        const int32_t id = static_cast<int32_t>(kept.size());
        kept.push_back(k);
        const uint8_t w = k.kind == Kind::zero ? flash_zero
                        : k.kind != Kind::raw ? flash_packed
                        : k.offset < page_bytes || (cfg.page_direct && k.size == page_bytes) ? flash_direct
                        : k.size < 512 ? flash_sub : flash_raw;
        for (uint32_t g = k.offset / granule_bytes; g < (k.offset + k.size) / granule_bytes && g < where.size(); g++) {
            where[g] = w;
            unit[g] = id;
        }
    }
};

Residency page0(uint32_t granules, uint32_t rom_size, const Config& cfg) {
    Residency r(granules);
    r.keep({0, std::min<uint32_t>(page_bytes, rom_size), Kind::raw, 0}, cfg);
    return r;
}

double decode_cycles(const Config& cfg, Kind kind, uint32_t size) {
    const double per512 = kind == Kind::deflate ? rom::deflate_cycles * cfg.cost.lzss / rom::lzss_cycles
                        : kind == Kind::lzss ? cfg.cost.lzss : 0;
    return per512 * size / 512.0 + (size < 512 ? cfg.cost.lzss_fixed : 0);
}

/* ---- simulation ------------------------------------------------------------ */

class Simulator {
public:
    Simulator(const Config& cfg, uint32_t granules, const Residency& r) : cfg_(cfg), granules_(granules), r_(r) {
        caches_.reserve(3);
        if (cfg.split[0]) {
            caches_.emplace_back(cfg.split[0], cfg.block, cfg.legacy_mru, staged_);
            caches_.emplace_back(cfg.split[1], cfg.block, cfg.legacy_mru, staged_);
            if (cfg.split[2]) caches_.emplace_back(cfg.split[2], cfg.block, cfg.legacy_mru, staged_);
        } else caches_.emplace_back(cfg.ways, cfg.block, cfg.legacy_mru, staged_);
        st_.g_count.assign(granules, 0);
        st_.g_path.assign(granules, 0);
        st_.g_miss.assign(granules, 0);
    }
    Stats run(const std::vector<std::string>& traces) {
        for (const auto& path : traces) one(path);
        close_episode();
        return std::move(st_);
    }

private:
    Cache& code() { return caches_[0]; }
    Cache& data() { return caches_.size() > 1 ? caches_[1] : caches_[0]; }
    Cache& cart() { return caches_.size() > 2 ? caches_[2] : data(); }
    double copy(unsigned block) const { return cfg_.cost.copy_fixed + cfg_.cost.copy_byte * block; }
    void close_episode() {
        if (ep_sector_ >= 0) st_.episodes[static_cast<uint64_t>(ep_sector_) << 32 | ep_mask_]++;
        ep_sector_ = -1;
        ep_mask_ = 0;
    }
    void end_frame() {
        /* gg_save_tick: dirty cartridge RAM written back 120 frames after its last change */
        if (pending_ && frame_ - changed_ >= 120) {
            Lookup wb;
            for (Cache& c : caches_) c.flush(wb);
            writebacks(wb);
            pending_ = false;
        }
        st_.per_frame.push_back(frame_reads_);
        st_.frame_cycles.push_back(static_cast<float>(st_.total() - frame_start_));
        frame_start_ = st_.total();
        frame_reads_ = 0;
        st_.frames++;
        frame_++;
    }
    void writebacks(const Lookup& wb) {
        st_.sd_reads += wb.writeback_reads;
        frame_reads_ += wb.writeback_reads;
        st_.sd_writes += wb.writebacks;
        st_.sd_cycles += cfg_.cost.sd * wb.writeback_reads + cfg_.cost.sd_write * wb.writebacks;
    }
    void path(uint32_t g, Class cls, uint64_t times, double cycles) {
        st_.count[cls] += times;
        st_.access_cycles += cycles;
        st_.g_count[g] += static_cast<uint32_t>(times);
        st_.g_path[g] += static_cast<float>(cycles);
    }
    /* one ROM access (granule g, fetch or data), then times - 1 repeats */
    void rom(uint32_t g, unsigned kind, uint64_t times) {
        const Costs& c = cfg_.cost;
        const uint8_t w = r_.where[g];
        st_.accesses += times;
        if (w == flash_direct) { st_.count[c_direct] += times; last_cls_ = c_direct; return; }
        const double enter = c.call + c.entry;
        if (w == flash_raw || w == flash_sub || w == flash_zero) {
            const Class cls = w == flash_raw ? c_raw : w == flash_sub ? c_sub : c_zero;
            const double one = enter + c.raw + (w == flash_sub ? c.sub : w == flash_zero ? c.zero : 0);
            path(g, cls, times, one * static_cast<double>(times));
            last_cls_ = cls;
            return;
        }
        const uint32_t off = g * granule_bytes;
        const bool packed = w == flash_packed;
        const Kept* k = packed ? &r_.kept[static_cast<size_t>(r_.unit[g])] : nullptr;
        const bool alone = packed && k->size < 512;
        const int64_t unit = packed ? -2 - r_.unit[g] : off >> 9;
        Cache& cache = kind && cfg_.split[0] ? code() : data();
        const Lookup l = cache.rom(off, unit, alone);
        writebacks(l);
        double cycles = enter;
        switch (l.cls) {
        case c_last: cycles += c.last; break;
        case c_staged: cycles += c.staged; break;
        case c_scan: cycles += c.scan + c.per_way * (l.way + 1); break;
        default: cycles += c.scan - 35 + c.per_way * l.way; break; /* the scan found nothing */
        }
        path(g, l.cls, 1, cycles);
        last_cls_ = l.cls == c_staged ? c_staged : c_last;
        if (times > 1) repeat_cached(g, times - 1);
        if (l.cls == c_miss) {
            st_.cache_misses++;
            const unsigned block = 1u << cache.shift();
            const double cp = alone || block >= 512 ? 0 : copy(block);
            st_.miss_cycles += c.miss;
            st_.copy_cycles += cp;
            st_.g_miss[g] += static_cast<float>(c.miss + cp);
            if (l.source) {
                close_episode();
                if (packed) {
                    st_.decodes++;
                    st_.decode_cycles += decode_cycles(cfg_, k->kind, alone ? k->size : 512);
                } else {
                    ep_sector_ = unit;
                    const uint32_t first = off & ~(std::min(block, 512u) - 1);
                    for (uint32_t x = first; x < first + std::min(block, 512u); x += granule_bytes)
                        ep_mask_ |= 1u << ((x & 511) / granule_bytes);
                    st_.sd_reads++;
                    st_.sd_cycles += c.sd;
                    frame_reads_++;
                }
            }
        } else if (l.cls == c_staged && unit == ep_sector_) ep_mask_ |= 1u << ((off & 511) / granule_bytes);
    }
    /* repeats of a cached access: the block read last, or the staged sector again */
    void repeat_cached(uint32_t g, uint64_t n) {
        const Costs& c = cfg_.cost;
        const Class cls = last_cls_ == c_staged ? c_staged : c_last;
        path(g, cls, n, (c.call + c.entry + (cls == c_staged ? c.staged : c.last)) * static_cast<double>(n));
        st_.accesses += n;
    }
    void cart_access(uint32_t g, bool write, uint64_t times) {
        const Costs& c = cfg_.cost;
        st_.accesses += times;
        if (!cfg_.cart_on_card) return;
        const Lookup l = cart().cart(g * granule_bytes, write);
        double cyc = l.cls == c_cart ? c.cart : c.cart_scan + c.per_way * (l.way + (l.cls == c_cart_scan));
        st_.count[l.cls]++;
        if (times > 1) { st_.count[c_cart] += times - 1; cyc += c.cart * static_cast<double>(times - 1); }
        if (l.cls == c_cart_miss) {
            const unsigned block = 1u << cart().shift();
            cyc += c.miss + (block >= 512 ? 0 : copy(block));
            if (l.source) {
                close_episode();
                st_.sd_reads++;
                st_.sd_cycles += c.sd;
                frame_reads_++;
            }
        }
        writebacks(l);
        st_.cart_cycles += cyc;
        if (write) { pending_ = true; changed_ = frame_; }
    }
    void one(const std::string& path) {
        Trace t(path);
        for (Cache& c : caches_) c.reset();
        close_episode();
        pending_ = false;
        frame_ = changed_ = 0;
        uint32_t g = 0, last_g = 0;
        unsigned kind = 0, last_kind = 0;
        uint64_t n = 0;
        int last_event = 0;
        bool last_skipped = true;
        for (;;) {
            const int e = t.next(g, kind, n);
            if (e < 0) break;
            if (e == 0) {
                end_frame();
                if (cfg_.max_frames && frame_ >= cfg_.max_frames) break;
                continue;
            }
            if (e == 2) { /* repeats of the access before */
                if (last_skipped) continue;
                if (last_event == 1) {
                    if (last_cls_ == c_direct || last_cls_ == c_raw || last_cls_ == c_sub || last_cls_ == c_zero) rom(last_g, last_kind, n);
                    else repeat_cached(last_g, n);
                } else if (last_event == 3) {
                    cart_access(last_g, last_kind != 0, n); /* a repeated write keeps the block dirty and its last change current */
                }
                continue;
            }
            last_event = e;
            last_g = g;
            last_kind = kind;
            if (e == 3) { last_skipped = false; cart_access(g, kind != 0, 1); continue; }
            last_skipped = (kind && !cfg_.fetch) || g >= granules_;
            if (!last_skipped) rom(g, kind, 1);
        }
    }

    const Config& cfg_;
    uint32_t granules_;
    const Residency& r_;
    int64_t staged_ = -1;
    std::vector<Cache> caches_;
    Stats st_;
    int64_t ep_sector_ = -1;
    uint32_t ep_mask_ = 0;
    uint32_t frame_reads_ = 0;
    double frame_start_ = 0;
    uint64_t frame_ = 0, changed_ = 0;
    bool pending_ = false;
    Class last_cls_ = c_last;
};

Stats simulate(const std::vector<std::string>& traces, uint32_t granules, const Residency& r, const Config& cfg) {
    Simulator s(cfg, granules, r);
    return s.run(traces);
}

/* ---- the plan ---------------------------------------------------------------- */

/* Packed sizes are cached: every round looks at the same blocks. */
class Packer {
public:
    Packer(const std::vector<uint8_t>& rom, bool deflate) : rom_(rom), deflate_(deflate) {}
    const rom::Block& get(uint32_t offset, uint32_t size) {
        const uint64_t key = static_cast<uint64_t>(offset) << 20 | size;
        auto it = memo_.find(key);
        if (it == memo_.end()) it = memo_.emplace(key, rom::encode(rom_.data() + offset, size, deflate_)).first;
        return it->second;
    }
    /* a zero-elided block: its mask, then its other bytes */
    size_t zero_bytes(uint32_t offset, uint32_t size) const {
        size_t n = size / 8;
        for (uint32_t i = 0; i < size; i++) n += rom_[offset + i] != 0;
        return n;
    }

private:
    const std::vector<uint8_t>& rom_;
    bool deflate_;
    std::map<uint64_t, rom::Block> memo_;
};

struct Plan {
    Residency residency;
    size_t used = 0;
    std::vector<std::string> rounds; /* JSON objects */
};

std::string stats_json(const Stats& st);

struct Candidate {
    double density, gain;
    Kept k;
};

Plan choose(const std::vector<std::string>& traces, const std::vector<uint8_t>& rom, uint32_t granules,
            const std::vector<uint32_t>& sizes, size_t budget, const Config& cfg, Packer& packer) {
    const Costs& c = cfg.cost;
    Plan plan{page0(granules, static_cast<uint32_t>(rom.size()), cfg)};
    std::vector<uint8_t> taken(granules, 0);
    std::map<std::pair<uint32_t, uint32_t>, size_t> packed_at; /* packed blocks, an upgrade to raw may follow */
    for (uint32_t g = 0; g < page_bytes / granule_bytes && g < granules; g++) taken[g] = 1;
    /* Rounds: each spends part of what is left, then the cache is simulated
     * again (blocks kept in flash free cache room for the others). */
    for (int round = 0; round < 8 && plan.used < budget; round++) {
        const Stats st = simulate(traces, granules, plan.residency, cfg);
        plan.rounds.push_back(stats_json(st));
        const double frames = static_cast<double>(st.frames ? st.frames : 1);
        /* per sector: its episodes (mask, count), and their total */
        const size_t sectors = (rom.size() + 511) / 512;
        std::vector<std::vector<std::pair<uint32_t, uint64_t>>> by_sector(sectors);
        std::vector<uint64_t> prefix(sectors + 1, 0);
        for (const auto& [key, n] : st.episodes) {
            const size_t sec = static_cast<size_t>(key >> 32);
            if (sec < sectors) by_sector[sec].push_back({static_cast<uint32_t>(key), n});
        }
        for (size_t sec = 0; sec < sectors; sec++) {
            uint64_t sum = 0;
            for (const auto& e : by_sector[sec]) sum += e.second;
            prefix[sec + 1] = prefix[sec] + sum;
        }
        std::vector<double> path_prefix(granules + 1, 0), count_prefix(granules + 1, 0);
        for (uint32_t g = 0; g < granules; g++) {
            path_prefix[g + 1] = path_prefix[g] + st.g_path[g] + st.g_miss[g];
            count_prefix[g + 1] = count_prefix[g] + st.g_count[g];
        }
        /* SD reads a block kept in flash saves: whole sectors' episodes, or
         * within a sector those that touched nothing outside the block */
        auto saved_reads = [&](uint32_t off, uint32_t size) -> uint64_t {
            if (size >= 512) return prefix[(off + size) / 512] - prefix[off / 512];
            const uint32_t mask = static_cast<uint32_t>(((uint64_t(1) << (size / granule_bytes)) - 1) << ((off & 511) / granule_bytes));
            uint64_t n = 0;
            for (const auto& [m, count] : by_sector[off / 512]) if (!(m & ~mask)) n += count;
            return n;
        };
        std::vector<Candidate> cands;
        auto per_frame = [&](double gain) { return gain / frames / c.sd; };
        auto add = [&](double gain, Kept k) {
            if (gain <= 0) return;
            k.saved = per_frame(gain);
            cands.push_back({gain / static_cast<double>(k.bytes), gain, k});
        };
        for (uint32_t size : sizes) {
            if (size > rom.size()) continue;
            for (uint32_t off = 0; off + size <= rom.size(); off += size) {
                const uint32_t g0 = off / granule_bytes, g1 = (off + size) / granule_bytes;
                bool overlap = false;
                for (uint32_t g = g0; g < g1 && !overlap; g++) overlap = taken[g];
                const auto was = packed_at.find({off, size});
                if (overlap && was == packed_at.end()) continue;
                const double reads = static_cast<double>(saved_reads(off, size));
                const double path = path_prefix[g1] - path_prefix[g0], n = count_prefix[g1] - count_prefix[g0];
                if (!reads && !n) continue;
                const double sd = c.sd * reads, enter = c.call + c.entry;
                /* raw: every access reads it in place, mapped or through the pool */
                if ((size >= 512 || cfg.sub) &&
                    (cfg.raw_sizes.empty() || std::find(cfg.raw_sizes.begin(), cfg.raw_sizes.end(), size) != cfg.raw_sizes.end())) {
                    const bool direct = cfg.page_direct && size == page_bytes;
                    const double each = direct ? 0 : enter + c.raw + (size < 512 ? c.sub : 0);
                    const size_t bytes = size + (direct ? 0 : size < 512 ? 2 : cfg.record_bytes * (size / 512));
                    const double gain = sd + path - n * each;
                    if (was != packed_at.end()) { /* packed already: upgraded for the bytes it adds */
                        const Kept& p = plan.residency.kept[was->second];
                        const double before = p.saved * frames * c.sd;
                        if (bytes > p.bytes && gain > before)
                            cands.push_back({(gain - before) / static_cast<double>(bytes - p.bytes), gain,
                                             {off, size, Kind::raw, bytes, per_frame(gain)}});
                        continue;
                    }
                    add(gain, {off, size, Kind::raw, bytes});
                }
                /* without its zero bytes, read in place */
                if (cfg.zero && (size == 32 || size == 64)) {
                    const size_t bytes = packer.zero_bytes(off, size) + 2;
                    if (bytes < size) add(sd + path - n * (enter + c.raw + c.zero), {off, size, Kind::zero, bytes});
                }
                /* packed: still through the cache, a decode where the SD read was */
                const bool alone = size < 512;
                if (size <= 512 && size >= cfg.block && (!alone || cfg.small_lzss)) {
                    const rom::Block& p = packer.get(off, size);
                    if (p.codec != rom::Codec::raw)
                        add(sd - decode_cycles(cfg, kind_of(p.codec), size) * reads,
                            {off, size, kind_of(p.codec), p.bytes.size() + cfg.record_bytes});
                }
            }
        }
        if (cands.empty()) break;
        std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.density > b.density; });
        const size_t left = budget - plan.used;
        const size_t spend = round == 7 ? left : std::max<size_t>(left * 2 / 5, 4096);
        size_t spent = 0;
        for (const Candidate& cand : cands) {
            if (spent >= spend) break;
            const auto was = packed_at.find({cand.k.offset, cand.k.size});
            if (was != packed_at.end() && cand.k.kind == Kind::raw) { /* raw upgrade of a packed block */
                const size_t old = was->second, before = plan.residency.kept[old].bytes;
                if (plan.used + cand.k.bytes - before > budget) continue;
                plan.used += cand.k.bytes - before;
                spent += cand.k.bytes - before;
                plan.residency.keep(cand.k, cfg); /* overrides the packed entry's granules */
                plan.residency.kept[old].size = 0; /* dropped below */
                packed_at.erase(was);
                continue;
            }
            if (was != packed_at.end() || plan.used + cand.k.bytes > budget) continue;
            const uint32_t g0 = cand.k.offset / granule_bytes, g1 = (cand.k.offset + cand.k.size) / granule_bytes;
            bool overlap = false;
            for (uint32_t g = g0; g < g1 && !overlap; g++) overlap = taken[g];
            if (overlap) continue;
            for (uint32_t g = g0; g < g1; g++) taken[g] = 1;
            if (cand.k.kind == Kind::lzss || cand.k.kind == Kind::deflate)
                packed_at[{cand.k.offset, cand.k.size}] = plan.residency.kept.size();
            plan.residency.keep(cand.k, cfg);
            plan.used += cand.k.bytes;
            spent += cand.k.bytes;
        }
        if (!spent) break;
    }
    /* What the profile never asked for still ships: packed 512-byte blocks,
     * the ROM bytes per flash byte first (no SD read for what the sessions
     * did not reach). */
    std::vector<Kept> fill;
    for (uint32_t off = 0; off + 512 <= rom.size(); off += 512) {
        bool free_block = true;
        for (uint32_t g = off / granule_bytes; g < (off + 512) / granule_bytes && free_block; g++) free_block = !taken[g];
        if (!free_block) continue;
        const rom::Block& p = packer.get(off, 512);
        fill.push_back({off, 512, kind_of(p.codec), p.bytes.size() + cfg.record_bytes});
    }
    std::sort(fill.begin(), fill.end(), [](const Kept& a, const Kept& b) { return a.bytes < b.bytes; });
    for (const Kept& k : fill) {
        if (plan.used + k.bytes > budget) break;
        plan.residency.keep(k, cfg);
        plan.used += k.bytes;
        for (uint32_t g = k.offset / granule_bytes; g < (k.offset + k.size) / granule_bytes; g++) taken[g] = 1;
    }
    /* entries an upgrade replaced */
    auto& kept = plan.residency.kept;
    std::vector<int32_t> remap(kept.size(), -1);
    std::vector<Kept> live;
    for (size_t i = 0; i < kept.size(); i++)
        if (kept[i].size) { remap[i] = static_cast<int32_t>(live.size()); live.push_back(kept[i]); }
    for (int32_t& u : plan.residency.unit) if (u >= 0) u = remap[static_cast<size_t>(u)];
    kept = std::move(live);
    return plan;
}

/* ---- output ---------------------------------------------------------------- */

std::string stats_json(const Stats& st) {
    std::vector<uint32_t> f = st.per_frame;
    std::sort(f.begin(), f.end());
    std::vector<float> fc = st.frame_cycles;
    std::sort(fc.begin(), fc.end());
    auto pct = [&](double p) { return f.empty() ? 0u : f[std::min(f.size() - 1, static_cast<size_t>(p * static_cast<double>(f.size())))]; };
    auto pctc = [&](double p) { return fc.empty() ? 0.0 : static_cast<double>(fc[std::min(fc.size() - 1, static_cast<size_t>(p * static_cast<double>(fc.size())))]); };
    uint64_t over[3] = {};
    for (uint32_t n : f) { over[0] += n >= 1; over[1] += n >= 2; over[2] += n >= 4; }
    const double frames = st.frames ? static_cast<double>(st.frames) : 1.0;
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(4);
    o << "{\"frames\": " << st.frames << ", \"accesses\": " << st.accesses << ", \"flash_raw_hits\": "
      << st.count[c_raw] + st.count[c_sub] + st.count[c_zero] + st.count[c_direct]
      << ", \"cache_misses\": " << st.cache_misses << ", \"sd_reads\": " << st.sd_reads << ", \"sd_writes\": " << st.sd_writes
      << ", \"decodes\": " << st.decodes << ", \"classes\": {";
    for (int i = 0; i < classes; i++) o << (i ? ", " : "") << "\"" << class_names[i] << "\": " << st.count[i];
    o << "}, \"sd_reads_per_frame\": {\"mean\": " << static_cast<double>(st.sd_reads) / frames << ", \"p50\": " << pct(.5)
      << ", \"p95\": " << pct(.95) << ", \"p99\": " << pct(.99) << ", \"max\": " << (f.empty() ? 0 : f.back())
      << ", \"frames_ge_1\": " << over[0] << ", \"frames_ge_2\": " << over[1] << ", \"frames_ge_4\": " << over[2] << "}";
    o.precision(1);
    o << ", \"cycles_per_frame\": {\"access\": " << st.access_cycles / frames << ", \"miss\": " << st.miss_cycles / frames
      << ", \"copy\": " << st.copy_cycles / frames << ", \"sd\": " << st.sd_cycles / frames << ", \"decode\": "
      << st.decode_cycles / frames << ", \"cart\": " << st.cart_cycles / frames << ", \"p50\": " << pctc(.5)
      << ", \"p95\": " << pctc(.95) << ", \"p99\": " << pctc(.99) << ", \"max\": " << (fc.empty() ? 0.0 : static_cast<double>(fc.back()))
      << "}, \"estimated_cycles_per_frame\": " << st.total() / frames << "}";
    return o.str();
}

std::vector<uint32_t> list(const char* s) {
    std::vector<uint32_t> v;
    for (std::stringstream in(s); in.good();) {
        std::string item;
        std::getline(in, item, ',');
        if (!item.empty()) v.push_back(static_cast<uint32_t>(std::stoul(item)));
    }
    return v;
}

/* The plan's blocks by size and kind, and the hottest kept ones (accesses
 * per byte): where they live, the hot data must stay raw. */
std::string plan_json(const Plan& plan, size_t budget, const Stats* st) {
    struct Row { uint64_t blocks = 0, rom_bytes = 0, flash_bytes = 0; };
    std::map<std::pair<uint32_t, std::string>, Row> by;
    uint64_t rom_bytes = 0, flash_bytes = 0;
    const auto& kept = plan.residency.kept;
    for (size_t i = 1; i < kept.size(); i++) {
        const Kept& k = kept[i];
        Row& r = by[{k.size, kind_name(k.kind)}];
        r.blocks++; r.rom_bytes += k.size; r.flash_bytes += k.bytes;
        rom_bytes += k.size; flash_bytes += k.bytes;
    }
    std::ostringstream o;
    o << "{\"budget\": " << budget << ", \"flash_bytes\": " << flash_bytes << ", \"rom_bytes_shipped\": " << rom_bytes
      << ", \"blocks\": [";
    bool first = true;
    for (const auto& [key, r] : by) {
        o << (first ? "" : ", ") << "{\"size\": " << key.first << ", \"codec\": \"" << key.second << "\", \"count\": " << r.blocks
          << ", \"rom_bytes\": " << r.rom_bytes << ", \"flash_bytes\": " << r.flash_bytes << "}";
        first = false;
    }
    o << "]";
    if (st && st->frames) {
        struct Hot { double heat; size_t i; double per_frame; };
        std::vector<Hot> hot;
        for (size_t i = 1; i < kept.size(); i++) {
            double n = 0;
            for (uint32_t g = kept[i].offset / granule_bytes; g < (kept[i].offset + kept[i].size) / granule_bytes && g < st->g_count.size(); g++)
                n += st->g_count[g];
            if (n > 0) hot.push_back({n / kept[i].size, i, n / static_cast<double>(st->frames)});
        }
        std::sort(hot.begin(), hot.end(), [](const Hot& a, const Hot& b) { return a.heat > b.heat; });
        size_t packed_top = 0;
        const size_t top = std::max<size_t>(1, hot.size() / 10);
        for (size_t j = 0; j < top && j < hot.size(); j++) packed_top += kept[hot[j].i].kind == Kind::lzss || kept[hot[j].i].kind == Kind::deflate;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << ", \"hottest\": [";
        for (size_t j = 0; j < hot.size() && j < 12; j++) {
            const Kept& k = kept[hot[j].i];
            char off[16];
            std::snprintf(off, sizeof off, "%06x", k.offset);
            o << (j ? ", " : "") << "{\"offset\": \"" << off << "\", \"size\": " << k.size << ", \"codec\": \"" << kind_name(k.kind)
              << "\", \"accesses_per_frame\": " << hot[j].per_frame << "}";
        }
        o << "], \"hottest_tenth_packed\": " << packed_top << ", \"hottest_tenth\": " << top;
    }
    o << "}";
    return o.str();
}

Residency read_plan(const std::string& path, uint32_t granules, uint32_t rom_size, const Config& cfg, size_t& total) {
    Residency given = page0(granules, rom_size, cfg);
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) throw std::runtime_error("cannot read " + path);
    char line[256], codec[16];
    unsigned off, size;
    size_t bytes;
    total = 0;
    while (std::fgets(line, sizeof line, f))
        if (line[0] != '#' && std::sscanf(line, "%x %u %15s %zu", &off, &size, codec, &bytes) == 4) {
            const std::string c = codec;
            given.keep({off, size, c == "lzss" ? Kind::lzss : c == "deflate" ? Kind::deflate : c == "zero" ? Kind::zero : Kind::raw, bytes}, cfg);
            total += bytes;
        }
    std::fclose(f);
    return given;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Config cfg;
        size_t budget = 0;
        std::vector<uint32_t> sizes = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384}, ways_curve, budget_curve;
        std::string plan_path, given_path, runtime = "current", costs_path;
        double sd_cycles = 0, lzss_cycles = 0;
        std::vector<std::string> args;
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            auto value = [&]() -> const char* {
                if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
                return argv[++i];
            };
            auto flag = [&]() { return std::stoul(value()) != 0; };
            if (a == "--ways") cfg.ways = static_cast<unsigned>(std::stoul(value()));
            else if (a == "--block") cfg.block = static_cast<unsigned>(std::stoul(value()));
            else if (a == "--split") {
                const auto v = list(value());
                for (size_t k = 0; k < 3; k++) cfg.split[k] = k < v.size() ? v[k] : 0;
            }
            else if (a == "--cart-on-card") cfg.cart_on_card = flag();
            else if (a == "--budget") budget = std::stoul(value());
            else if (a == "--sizes") sizes = list(value());
            else if (a == "--raw-sizes") cfg.raw_sizes = list(value());
            else if (a == "--runtime") runtime = value();
            else if (a == "--costs") costs_path = value();
            else if (a == "--sd-cycles") sd_cycles = std::stod(value());
            else if (a == "--lzss-cycles") lzss_cycles = std::stod(value());
            else if (a == "--record-bytes") cfg.record_bytes = std::stoul(value());
            else if (a == "--fetch") cfg.fetch = flag();
            else if (a == "--deflate") cfg.deflate = flag();
            else if (a == "--sub") cfg.sub = flag();
            else if (a == "--small-lzss") cfg.small_lzss = flag();
            else if (a == "--zero") cfg.zero = flag();
            else if (a == "--page-direct") cfg.page_direct = flag();
            else if (a == "--legacy-mru") cfg.legacy_mru = true;
            else if (a == "--max-frames") cfg.max_frames = std::stoull(value());
            else if (a == "--plan") plan_path = value();
            else if (a == "--ways-curve") ways_curve = list(value());
            else if (a == "--budget-curve") budget_curve = list(value());
            else if (a == "--simulate-plan") given_path = value();
            else args.push_back(a);
        }
        cfg.cost = costs(runtime);
        if (!costs_path.empty()) load_costs(cfg.cost, costs_path);
        if (sd_cycles > 0) cfg.cost.sd = sd_cycles;
        if (lzss_cycles > 0) cfg.cost.lzss = lzss_cycles;
        auto ways_ok = [](unsigned w) { return w >= 1 && w <= 255; };
        if (args.size() < 2 || !ways_ok(cfg.ways) || (cfg.split[0] && !ways_ok(cfg.split[1])) ||
            cfg.block < 16 || cfg.block > 512 || (cfg.block & (cfg.block - 1)))
            throw std::runtime_error("usage: m0romsim [options] ROM TRACE.gz... (see the source header)");
        for (uint32_t s : sizes)
            if (s < granule_bytes || (s & (s - 1))) throw std::runtime_error("block sizes: powers of two from 16");
        std::vector<uint8_t> rom;
        {
            FILE* f = std::fopen(args[0].c_str(), "rb");
            if (!f) throw std::runtime_error("cannot read " + args[0]);
            for (int c; (c = std::fgetc(f)) != EOF;) rom.push_back(static_cast<uint8_t>(c));
            std::fclose(f);
        }
        if (rom.size() % page_bytes == 512) rom.erase(rom.begin(), rom.begin() + 512); /* copier header */
        const std::vector<std::string> traces(args.begin() + 1, args.end());
        const uint32_t granules = static_cast<uint32_t>((rom.size() + granule_bytes - 1) / granule_bytes);
        const uint32_t rom_size = static_cast<uint32_t>(rom.size());
        Packer packer(rom, cfg.deflate);

        const Residency base_r = page0(granules, rom_size, cfg);
        const Stats base = simulate(traces, granules, base_r, cfg);
        Plan plan = choose(traces, rom, granules, sizes, budget, cfg, packer);
        const Stats fin = simulate(traces, granules, plan.residency, cfg);

        auto b = [](bool v) { return v ? "true" : "false"; };
        std::printf("{\"format\": \"m0boy.romsim.v2\", \"note\": \"simulated card cache on host traces: estimates, not device measurements\",\n");
        std::printf(" \"config\": {\"ways\": %u, \"block\": %u, \"split\": [%u, %u, %u], \"cart_on_card\": %s, \"runtime\": \"%s\", "
                    "\"sd_cycles\": %.0f, \"lzss_cycles_per_512\": %.0f, \"record_bytes\": %zu, \"fetch\": %s, \"deflate\": %s, "
                    "\"sub\": %s, \"small_lzss\": %s, \"zero\": %s, \"page_direct\": %s, \"rom_bytes\": %zu, \"traces\": %zu},\n",
                    cfg.ways, cfg.block, cfg.split[0], cfg.split[1], cfg.split[2], b(cfg.cart_on_card), runtime.c_str(),
                    cfg.cost.sd, cfg.cost.lzss, cfg.record_bytes, b(cfg.fetch), b(cfg.deflate), b(cfg.sub), b(cfg.small_lzss),
                    b(cfg.zero), b(cfg.page_direct), rom.size(), traces.size());
        std::printf(" \"page0_only\": %s,\n \"rounds\": [", stats_json(base).c_str());
        for (size_t i = 0; i < plan.rounds.size(); i++) std::printf("%s%s", i ? ", " : "", plan.rounds[i].c_str());
        std::printf("],\n \"plan\": %s,\n \"result\": %s", plan_json(plan, budget, &fin).c_str(), stats_json(fin).c_str());
        if (!given_path.empty()) {
            size_t total = 0;
            Plan p{read_plan(given_path, granules, rom_size, cfg, total)};
            const Stats gs = simulate(traces, granules, p.residency, cfg);
            std::printf(",\n \"given_plan\": {\"plan\": %s, \"result\": %s", plan_json(p, total, &gs).c_str(), stats_json(gs).c_str());
            if (!ways_curve.empty()) {
                std::printf(", \"ways_curve\": [");
                for (size_t i = 0; i < ways_curve.size(); i++) {
                    Config c = cfg;
                    c.ways = ways_curve[i];
                    std::printf("%s{\"ways\": %u, \"result\": %s}", i ? ", " : "", c.ways,
                                stats_json(simulate(traces, granules, p.residency, c)).c_str());
                }
                std::printf("]");
            }
            std::printf("}");
        }
        if (!ways_curve.empty()) {
            std::printf(",\n \"ways_curve\": [");
            for (size_t i = 0; i < ways_curve.size(); i++) {
                Config c = cfg;
                c.ways = ways_curve[i];
                std::printf("%s{\"ways\": %u, \"page0_only\": %s, \"plan\": %s}", i ? ", " : "", c.ways,
                            stats_json(simulate(traces, granules, base_r, c)).c_str(),
                            stats_json(simulate(traces, granules, plan.residency, c)).c_str());
            }
            std::printf("]");
        }
        if (!budget_curve.empty()) {
            std::printf(",\n \"budget_curve\": [");
            for (size_t i = 0; i < budget_curve.size(); i++) {
                const Plan p = choose(traces, rom, granules, sizes, budget_curve[i], cfg, packer);
                std::printf("%s{\"plan\": %s, \"result\": %s}", i ? ", " : "", plan_json(p, budget_curve[i], nullptr).c_str(),
                            stats_json(simulate(traces, granules, p.residency, cfg)).c_str());
            }
            std::printf("]");
        }
        std::printf("}\n");
        if (!plan_path.empty()) {
            FILE* f = std::fopen(plan_path.c_str(), "w");
            if (!f) throw std::runtime_error("cannot write " + plan_path);
            std::fprintf(f, "# m0boy.romplan.v1: offset size codec flash_bytes saved_per_frame (SD reads' worth of cycles; page 0 implied)\n");
            for (size_t i = 1; i < plan.residency.kept.size(); i++) {
                const Kept& k = plan.residency.kept[i];
                std::fprintf(f, "%06x %u %s %zu %.6f\n", k.offset, k.size, kind_name(k.kind), k.bytes, k.saved);
            }
            std::fclose(f);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "m0romsim: %s\n", e.what());
        return 1;
    }
    return 0;
}
