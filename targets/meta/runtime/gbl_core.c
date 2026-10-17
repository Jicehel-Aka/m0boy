/*
 * gbl core: bus, cartridge mapper, timer, joypad, interrupts and the
 * compiled/interpreted execution loop.
 */
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_internal.h"
#include "gbl_timing_layout.h"

#include <stddef.h>
#include <string.h>

gbl_cpu_t gbr;
uint8_t gbl_wram[0x2000];
uint8_t gbl_vram[0x2000];
uint8_t gbl_oam[0xA0] __attribute__((aligned(4))); /* word copies (OAM DMA) */
uint8_t gbl_hram[0x80];
uint8_t gbl_io[0x80];
uint8_t gbl_ie;
#if GBL_ERAM_SIZE > 0 && !GBL_ERAM_ON_SD
uint8_t gbl_eram[GBL_ERAM_SIZE];
#endif

const uint8_t *gbl_rmap[16];
uint8_t *gbl_wmap[16];

uint8_t gbl_interp_only;
uint32_t gbl_frame;
uint32_t gbl_interp_instrs;
uintptr_t gbl_stack_limit;

/* ---- cartridge ---------------------------------------------------------- */

/* Page pointers are biased by the page's CPU address so gbl_rmap[a >> 12][a]
 * addresses the byte directly. */
#define BIAS(p, cpu) ((uint8_t *)((uintptr_t)(p) - (uintptr_t)(cpu)))

static struct {
    uint8_t ram_enable;
    uint8_t lo, hi, mode;
} mbc;

#if GBL_ROM_STREAM
/* ---- ROM streaming ---------------------------------------------------------
 * Bank 0 (gbl_rom) and the resident 4 KiB chunks are in flash and mapped
 * directly; reads from other chunks go through an LRU cache of 512-byte
 * sectors filled by gbl_platform_rom_read. */
extern const uint8_t gbl_rom_resident[][4096];
extern const uint8_t gbl_rom_chunk_map[];

/* Buffers, tags and the last slot are global: the assembly gbl_rd reads a
 * byte of the last sector used without calling gbl_rd_slow. */
uint8_t gbl_stream_buf[GBL_STREAM_SECTORS][512] __attribute__((aligned(4)));
int32_t gbl_stream_tag[GBL_STREAM_SECTORS];
uint8_t gbl_stream_last;
#define stream_buf gbl_stream_buf
#define stream_tag gbl_stream_tag
#define stream_last gbl_stream_last
static uint32_t stream_age[GBL_STREAM_SECTORS], stream_clock;
uint32_t gbl_stream_misses;

uint8_t *gbl_stream_scratch(void) { return stream_buf[0]; }

static void stream_reset(void) {
    for (int i = 0; i < GBL_STREAM_SECTORS; i++) { stream_tag[i] = -1; stream_age[i] = 0; }
    stream_last = 0;
}

#if GBL_ROM_PACKED
#include "m0_rom_codec.h"
/* Compressed flash source: sector k of the ROM is gbl_rom_packed
 * [off[k], off[k + 1]), LZSS-coded, or stored as is when that is 512
 * bytes.  Only sectors outside bank 0 and the resident chunks are used. */
extern const uint8_t gbl_rom_packed[];
extern const uint32_t gbl_rom_packed_off[];

/* Decode one 512-byte sector.  Bitstream MSB first; per token a flag bit:
 * 1 = literal byte, 0 = back-reference (8-bit offset - 1, 4-bit length -
 * 2) into the bytes already decoded, so no window beyond dst is needed.
 * gbl_unpack512_asm (gbl_core_asm.S) is the same loop in Thumb-1. */
#if GBL_UNPACK_ASM
#if GBL_PPU_RAMFUNC
__attribute__((long_call)) /* in SRAM, out of BL range from flash */
#endif
void gbl_unpack512_asm(const uint8_t *src, uint8_t *dst);
#define unpack512 gbl_unpack512_asm
#else
static void unpack512(const uint8_t *src, uint8_t *dst) {
    m0_rom_unpack512_c(src, dst);
}
#endif

static int rom_unpack(uint32_t sector, uint8_t *dst) {
    uint32_t a = gbl_rom_packed_off[sector], n = gbl_rom_packed_off[sector + 1] - a;
    if (!n) return 0; /* a hybrid build leaves this sector on the card */
    if (n == 512) memcpy(dst, gbl_rom_packed + a, 512);
    else unpack512(gbl_rom_packed + a, dst);
    gbl_stream_unpacked++;
    return 1;
}
uint32_t gbl_stream_unpacked;
#endif

static uint8_t stream_read(uint32_t off) {
    int32_t sec = (int32_t)(off >> 9);
    if (stream_tag[stream_last] == sec) return stream_buf[stream_last][off & 511u];
    unsigned slot = 0;
    for (unsigned i = 0; i < GBL_STREAM_SECTORS; i++) {
        if (stream_tag[i] == sec) { slot = i; goto hit; }
        if (stream_age[i] < stream_age[slot]) slot = i;
    }
#if GBL_ROM_PACKED
    if (!rom_unpack((uint32_t)sec, stream_buf[slot]))
#endif
#if GBL_ROM_SD
    if (!gbl_platform_rom_read((uint32_t)sec, stream_buf[slot]))
        for (int i = 0; i < 512; i++) stream_buf[slot][i] = 0xFF;
#elif GBL_ROM_PACKED
        for (int i = 0; i < 512; i++) stream_buf[slot][i] = 0xFF;
#endif
    stream_tag[slot] = sec;
    gbl_stream_misses++;
hit:
    stream_age[slot] = ++stream_clock;
    stream_last = (uint8_t)slot;
    return stream_buf[slot][off & 511u];
}
#endif

/* Bank whose pages gbl_rmap[4..7] hold; the pages are only rebuilt when
 * the bank changes (games rewrite the bank register around every banked
 * call).  gbl_reset voids it. */
static uint16_t mapped_bank = 0xFFFF;

static void map_rom_bank(void) {
    uint32_t bank;
#if GBL_MBC == 1
    bank = (uint32_t)(mbc.lo & 0x1F);
    if (bank == 0) bank = 1;
    bank |= (uint32_t)(mbc.hi & 3) << 5;
#elif GBL_MBC == 2
    bank = mbc.lo & 0x0F;
    if (bank == 0) bank = 1;
#elif GBL_MBC == 3
    bank = mbc.lo & 0x7F;
    if (bank == 0) bank = 1;
#elif GBL_MBC == 5
    bank = (uint32_t)mbc.lo | ((uint32_t)(mbc.hi & 1) << 8);
#else
    bank = 1;
#endif
    bank &= (uint32_t)(GBL_ROM_BANKS - 1);
    gbr.rom_bank = (uint16_t)bank;
    if (bank == mapped_bank) return;
    mapped_bank = (uint16_t)bank;
#if GBL_ROM_STREAM
    /* Bank 0 is already resident at gbl_rom. MBC5 can also select it in
     * the upper slot: keep those reads out of the SD cache as well. */
    if (bank == 0) {
        const uint8_t *base = BIAS(gbl_rom, 0x4000u);
        gbl_rmap[4] = gbl_rmap[5] = gbl_rmap[6] = gbl_rmap[7] = base;
    } else {
        const uint8_t *m = gbl_rom_chunk_map + bank * 4u;
#define CHUNK(i) gbl_rmap[4 + i] = m[i] ? BIAS(gbl_rom_resident[m[i] - 1u], 0x4000u + i * 0x1000u) : 0
        CHUNK(0); CHUNK(1); CHUNK(2); CHUNK(3);
#undef CHUNK
    }
#else
    const uint8_t *base = BIAS(gbl_rom + bank * 0x4000u, 0x4000u);
    gbl_rmap[4] = gbl_rmap[5] = gbl_rmap[6] = gbl_rmap[7] = base;
#endif
#ifdef GBL_ROM_TRACE
    /* host study of ROM reads: every banked read is observed */
    for (int i = 4; i < 8; i++) gbl_rmap[i] = 0;
#endif
}

/* ---- cartridge RAM -------------------------------------------------------
 * Small RAM lives in gbl_eram.  With a battery (GBL_BATTERY) the platform
 * keeps it in a file: it is loaded at reset, writes go through the slow path
 * to mark 512-byte sectors dirty, and gbl_save_flush writes those back (when
 * the game disables cartridge RAM, as games do after saving, and whenever
 * the frontend decides).  RAM too large for the target (GBL_ERAM_ON_SD)
 * exists only in that file, behind a write-back cache of
 * GBL_ERAM_CACHE_SECTORS sectors (gbl_profile.h). */
#if GBL_ERAM_SIZE > 0 && GBL_MBC != 2
static uint32_t eram_bank(void) {
    uint32_t bank = 0;
#if GBL_MBC == 1
    if (mbc.mode) bank = mbc.hi & 3;
#elif GBL_MBC == 3 || GBL_MBC == 5
    bank = mbc.hi & 0x0F;
#endif
    return bank;
}
#endif

#if GBL_ERAM_SIZE > 0
static uint32_t eram_offset(uint16_t a) {
#if GBL_MBC == 2
    return a & 0x1FFu;
#else
    return (eram_bank() * 0x2000u + (uint32_t)(a - 0xA000u)) % GBL_ERAM_SIZE;
#endif
}

uint32_t gbl_save_last_change; /* gbl_frame of the last change not yet saved */
static uint8_t save_pending;

#if GBL_ERAM_ON_SD
#ifndef GBL_ERAM_CACHE_SECTORS
#define GBL_ERAM_CACHE_SECTORS 2
#endif
#define ERAM_WAYS GBL_ERAM_CACHE_SECTORS
#if ERAM_WAYS < 1 || ERAM_WAYS > 256
#error "GBL_ERAM_CACHE_SECTORS must be between 1 and 256"
#endif
static uint8_t eram_buf[ERAM_WAYS][512] __attribute__((aligned(4)));
static int32_t eram_tag[ERAM_WAYS];
/* Circular LRU list, newest at eram_mru, oldest at eram_newer[eram_mru].
 * Two bytes per way, as with the old timestamps, without clock wraparound.
 * Repeated accesses to the newest way do not touch the list. */
static uint8_t eram_older[ERAM_WAYS], eram_newer[ERAM_WAYS];
static uint8_t eram_dirty[ERAM_WAYS], eram_mru;

/* Window on the newest sector for gbl_rd / gbl_wr (C and assembly), so
 * that most accesses skip gbl_rd_slow and eram_sector: the Game Boy address
 * of its first byte with the current bank and RAM enable, and a pointer
 * indexed by that address.  GBL_ERAM_WIN_NONE (outside 16 bits) never
 * matches.  The write window opens only when that sector is already dirty,
 * so a write through it only has to note the change (gbl_save_last_change).
 * eram_window recomputes both after anything that moves them. */
uint32_t gbl_eram_rwin = GBL_ERAM_WIN_NONE, gbl_eram_wwin = GBL_ERAM_WIN_NONE;
uint8_t *gbl_eram_rptr, *gbl_eram_wptr;

static void eram_window(void) {
    gbl_eram_rwin = gbl_eram_wwin = GBL_ERAM_WIN_NONE;
#if GBL_ERAM_SIZE % 0x2000 == 0
    int32_t sec = eram_tag[eram_mru];
    uint32_t base = eram_bank() * 0x2000u, off = (uint32_t)sec * 512u;
    if (!mbc.ram_enable || sec < 0 || off < base || off >= base + 0x2000u) return;
    uint32_t a = 0xA000u + off - base;
    gbl_eram_rwin = a;
    gbl_eram_rptr = BIAS(eram_buf[eram_mru], a);
    if (eram_dirty[eram_mru]) {
        gbl_eram_wwin = a;
        gbl_eram_wptr = gbl_eram_rptr;
    }
#endif
}

static void eram_touch(unsigned i) {
    unsigned older = eram_older[i], newer = eram_newer[i];
    eram_older[newer] = (uint8_t)older;
    eram_newer[older] = (uint8_t)newer;
    unsigned tail = eram_newer[eram_mru];
    eram_older[tail] = (uint8_t)i;
    eram_newer[i] = (uint8_t)tail;
    eram_older[i] = eram_mru;
    eram_newer[eram_mru] = (uint8_t)i;
    eram_mru = (uint8_t)i;
}

static void eram_writeback(unsigned i) {
    if (eram_dirty[i]) {
        gbl_platform_save_write((uint32_t)eram_tag[i], eram_buf[i]);
        eram_dirty[i] = 0;
    }
}

/* The most recent sector is checked first (most accesses); the others when
 * the game moves to another sector.  A game using its cartridge RAM as work
 * RAM cycles through several sectors (Super Mario Land 2: about seven), and
 * every eviction of a dirty one is a card write. */
static uint8_t *eram_sector(uint32_t off) {
    int32_t sec = (int32_t)(off >> 9);
    unsigned i = eram_mru;
    if (eram_tag[i] != sec) {
        /* Games alternate between two sectors (a pointer table and the
         * data): the previous one before the scan. */
        i = eram_older[eram_mru];
        if (eram_tag[i] == sec) {
            eram_touch(i);
            eram_window();
            return eram_buf[i];
        }
        for (i = 0; i < ERAM_WAYS && eram_tag[i] != sec; i++) {}
        if (i == ERAM_WAYS) {
            i = eram_newer[eram_mru];
            eram_writeback(i);
            if (!gbl_platform_save_read((uint32_t)sec, eram_buf[i]))
                for (int k = 0; k < 512; k++) eram_buf[i][k] = 0xFF;
            eram_tag[i] = sec;
        }
        if (i != eram_mru) eram_touch(i);
        eram_window();
    }
    return eram_buf[i];
}

static uint8_t eram_load(uint32_t off) { return eram_sector(off)[off & 511u]; }

static void eram_store(uint32_t off, uint8_t v) {
    uint8_t *p = eram_sector(off);
    if (p[off & 511u] == v) return;
    p[off & 511u] = v;
    save_pending = 1;
    gbl_save_last_change = gbl_frame;
    if (!eram_dirty[eram_mru]) {
        eram_dirty[eram_mru] = 1;
        eram_window(); /* the write window opens */
    }
}

void gbl_save_flush(void) {
    for (unsigned i = 0; i < ERAM_WAYS; i++) eram_writeback(i);
    save_pending = 0;
    eram_window(); /* clean again: the write window closes */
}

uint8_t *gbl_save_scratch(void) { return eram_buf[0]; }

static void eram_reset(void) {
    for (unsigned i = 0; i < ERAM_WAYS; i++) {
        eram_tag[i] = -1;
        eram_dirty[i] = 0;
        eram_older[i] = (uint8_t)((i + 1u) % ERAM_WAYS);
        eram_newer[i] = (uint8_t)((i + ERAM_WAYS - 1u) % ERAM_WAYS);
    }
    eram_mru = 0;
    save_pending = 0;
    eram_window();
}
#else
static uint32_t eram_dirty_mask; /* bit per 512-byte sector */

static uint8_t eram_load(uint32_t off) { return gbl_eram[off]; }

static void eram_store(uint32_t off, uint8_t v) {
    if (gbl_eram[off] == v) return;
    gbl_eram[off] = v;
#if GBL_BATTERY
    eram_dirty_mask |= 1u << (off >> 9);
    save_pending = 1;
    gbl_save_last_change = gbl_frame;
#endif
}

void gbl_save_flush(void) {
#if GBL_BATTERY
    for (uint32_t sec = 0; eram_dirty_mask; sec++) {
        if (eram_dirty_mask & (1u << sec)) {
            gbl_platform_save_write(sec, gbl_eram + sec * 512u);
            eram_dirty_mask &= ~(1u << sec);
        }
    }
#endif
    save_pending = 0;
}

uint8_t *gbl_save_scratch(void) { return gbl_eram; }

static void eram_reset(void) {
    eram_dirty_mask = 0;
    save_pending = 0;
#if GBL_BATTERY
    /* The saved RAM, or 0xFF (an erased cartridge) where there is none. */
    for (uint32_t sec = 0; sec * 512u < GBL_ERAM_SIZE; sec++)
        if (!gbl_platform_save_read(sec, gbl_eram + sec * 512u))
            memset(gbl_eram + sec * 512u, 0xFF, 512);
#endif
}
#endif

int gbl_save_pending(void) { return save_pending; }

/* RAM enable register: a game that disables cartridge RAM has finished
 * writing it; that is the moment to save. */
static __attribute__((unused)) void set_ram_enable(uint8_t v) {
    uint8_t on = (v & 0x0F) == 0x0A;
    if (mbc.ram_enable && !on && save_pending) gbl_save_flush();
    mbc.ram_enable = on;
}
#else
void gbl_save_flush(void) {}
int gbl_save_pending(void) { return 0; }
uint8_t *gbl_save_scratch(void) { return 0; }
uint32_t gbl_save_last_change;
static __attribute__((unused)) void set_ram_enable(uint8_t v) { mbc.ram_enable = (v & 0x0F) == 0x0A; }
#endif

static void map_eram(void) {
#if GBL_ERAM_ON_SD
    eram_window(); /* bank or RAM enable */
#endif
#if GBL_ERAM_SIZE >= 0x2000 && GBL_MBC != 2 && !GBL_ERAM_ON_SD
    /* Whole 8 KiB banks map directly (reads always; writes only without a
     * battery, whose saves need the dirty tracking of the slow path).
     * Smaller RAM mirrors through the slow path. */
    uint32_t off = (eram_bank() * 0x2000u) % GBL_ERAM_SIZE;
    uint8_t *base = mbc.ram_enable ? BIAS(gbl_eram + off, 0xA000u) : 0;
    gbl_rmap[0xA] = gbl_rmap[0xB] = base;
    gbl_wmap[0xA] = gbl_wmap[0xB] = GBL_BATTERY ? 0 : base;
#endif
}

/* The assembly gbl_wr calls it directly for 0000-7FFF. */
#if GBL_CORE_ASM && defined(__ARM_ARCH_6M__)
void gbl_mbc_write(uint16_t a, uint8_t v);
#define mbc_write gbl_mbc_write
#else
static
#endif
void mbc_write(uint16_t a, uint8_t v) {
#if GBL_MBC == 1
    switch (a >> 13) {
    case 0: set_ram_enable(v); map_eram(); break;
    case 1: mbc.lo = v; map_rom_bank(); break;
    case 2: mbc.hi = v & 3; map_rom_bank(); map_eram(); break;
    case 3: mbc.mode = v & 1; map_eram(); break;
    }
#elif GBL_MBC == 2
    if (a < 0x4000) {
        if (a & 0x100) { mbc.lo = v; map_rom_bank(); }
        else set_ram_enable(v);
    }
#elif GBL_MBC == 3
    switch (a >> 13) {
    case 0: set_ram_enable(v); map_eram(); break;
    case 1: mbc.lo = v; map_rom_bank(); break;
    case 2: mbc.hi = v; map_eram(); break;
    default: break; /* RTC latch: no RTC on this target */
    }
#elif GBL_MBC == 5
    if (a < 0x2000) { set_ram_enable(v); map_eram(); }
    else if (a < 0x3000) { mbc.lo = v; map_rom_bank(); }
    else if (a < 0x4000) { mbc.hi = (mbc.hi & 0xF0) | (v & 1); map_rom_bank(); }
    else if (a < 0x6000) { mbc.hi = (uint8_t)((v & 0x0F) | (mbc.hi & 1)); map_eram(); }
#else
    (void)a; (void)v;
#endif
}

#if GBL_CORE_ASM && defined(__ARM_ARCH_6M__)
/* gbl_core_asm.S: copy 160 bytes into the (word-aligned) OAM, return
 * nonzero when any byte changed. */
uint32_t gbl_oam_dma_asm(uint8_t *oam, const uint8_t *src);
#endif

/* ---- timer ---------------------------------------------------------------- */

/* Timer and serial state in one structure, for the assembly gbl_sync
 * (gbl_timing_asm.S, offsets in gbl_timing_layout.h). */
typedef struct {
    uint32_t div_base;    /* cycle at which the 16-bit divider was zero */
    uint32_t tima_sync;   /* cycle the TIMA counter was last advanced to */
    uint32_t serial_done; /* end of the transfer in progress */
    uint8_t serial_busy;
    uint8_t core_has_next, core_dirty; /* gbl_core_next_event cache */
    uint32_t core_next;
} gbl_timer_state_t;
gbl_timer_state_t gbl_timer_state = {.core_dirty = 1};
_Static_assert(offsetof(gbl_timer_state_t, div_base) == GBL_T_DIV_BASE, "div_base");
_Static_assert(offsetof(gbl_timer_state_t, tima_sync) == GBL_T_TIMA_SYNC, "tima_sync");
_Static_assert(offsetof(gbl_timer_state_t, serial_busy) == GBL_T_SERIAL_BUSY, "serial_busy");
_Static_assert(offsetof(gbl_timer_state_t, core_has_next) == GBL_T_CORE_HAS_NEXT, "core_has_next");
_Static_assert(offsetof(gbl_timer_state_t, core_dirty) == GBL_T_CORE_DIRTY, "core_dirty");
_Static_assert(offsetof(gbl_timer_state_t, core_next) == GBL_T_CORE_NEXT, "core_next");
#define div_base gbl_timer_state.div_base
#define tima_sync gbl_timer_state.tima_sync
#define serial_done gbl_timer_state.serial_done
#define serial_busy gbl_timer_state.serial_busy
const uint8_t gbl_tac_shift[4] = {10, 4, 6, 8};
#define tac_shift gbl_tac_shift

#define TIMA gbl_io[0x05]
#define TMA gbl_io[0x06]
#define TAC gbl_io[0x07]
#define IF gbl_io[0x0F]

static void core_invalidate(void);

/* Also the slow path of the assembly gbl_sync (a TIMA overflow). */
#if GBL_TIMING_ASM
void gbl_timer_sync(uint32_t now);
#define timer_sync gbl_timer_sync
#else
static
#endif
void timer_sync(uint32_t now) {
    if (TAC & 4) {
        uint32_t s = tac_shift[TAC & 3];
        uint32_t ticks = ((now - div_base) >> s) - ((tima_sync - div_base) >> s);
        while (ticks) {
            uint32_t room = 256u - TIMA;
            if (ticks < room) { TIMA = (uint8_t)(TIMA + ticks); break; }
            ticks -= room;
            TIMA = TMA;
            core_invalidate(); /* without a reload the next overflow stays put */
            IF |= 0x04;
        }
    }
    tima_sync = now;
}

static uint32_t timer_next_event(void) {
    uint32_t s = tac_shift[TAC & 3];
    uint32_t done = (tima_sync - div_base) >> s;
    return div_base + ((done + (256u - TIMA)) << s);
}

/* ---- serial: internal-clock transfers complete with no partner ---------- */
/* (state in gbl_timer_state) */

/* ---- events --------------------------------------------------------------- */

/* Earliest timer or serial event, or best if that comes first (the PPU part
 * and the event cache live in gbl_ppu.c: gbl_update_evt). */
/* The timer/serial part is cached on its own: PPU transitions invalidate
 * the combined event far more often than the timer changes. */
/* (cache in gbl_timer_state) */
#define core_next gbl_timer_state.core_next
#define core_has_next gbl_timer_state.core_has_next
#define core_dirty gbl_timer_state.core_dirty

static void core_invalidate(void) {
    core_dirty = 1;
    gbl_evt_invalidate();
}

uint32_t gbl_core_next_event(uint32_t best) {
    if (core_dirty) {
        uint32_t t = 0;
        uint8_t has = 0;
        if (TAC & 4) { t = timer_next_event(); has = 1; }
        if (serial_busy && (!has || (int32_t)(serial_done - t) < 0)) { t = serial_done; has = 1; }
        core_next = t;
        core_has_next = has;
        core_dirty = 0;
    }
    if (core_has_next && (int32_t)(core_next - best) < 0) best = core_next;
    return best;
}

/* The serial part of gbl_sync, then the next event (the assembly gbl_sync
 * jumps here while a transfer is in progress). */
#if GBL_TIMING_ASM
void gbl_sync_serial(uint32_t now);
#else
static inline __attribute__((always_inline))
#endif
void gbl_sync_serial(uint32_t now) {
    if (serial_busy && (int32_t)(now - serial_done) >= 0) {
        serial_busy = 0;
        core_invalidate();
        gbl_io[0x01] = 0xFF;
        gbl_io[0x02] &= 0x7F;
        IF |= 0x08;
    }
    gbl_update_evt();
}

#if !GBL_TIMING_ASM /* else gbl_timing_asm.S */
void gbl_sync(void) {
    uint32_t now = gbr.cyc;
    gbl_ppu_sync(now);
    timer_sync(now);
    gbl_sync_serial(now);
}
#endif

/* ---- wait loops ------------------------------------------------------------ */

/* Iterations to skip so the first one not skipped is the first that can see
 * a change `room` cycles from now (ceil(room / period)). */
static uint32_t spin_iters(uint32_t period, int32_t room) {
    if (room <= 0) return 0;
    uint32_t r = (uint32_t)room;
    if (r > 8u * period) return (r + period - 1u) / period;
    /* Usually a few periods (until the next PPU mode change): subtracting
     * is cheaper than the M0+'s software division. */
    uint32_t k = 1;
    while (r > period) {
        r -= period;
        k++;
    }
    return k;
}

void gbl_spin(uint32_t period, uint32_t read_ofs, uint32_t kind) {
#if GBL_SPIN
    if (kind > 2) return;
    uint32_t now = gbr.cyc;
    /* Skipped iterations must not reach the loop head's interrupt poll at or
     * after next_evt, nor read a device value at or after it. */
    int32_t room = (int32_t)(gbr.next_evt - now);
    if (kind) room -= (int32_t)read_ofs;
    if (kind == 2) {
        int32_t c = (int32_t)(gbl_ppu_stable_until() - now) - (int32_t)read_ofs;
        if (c < room) room = c;
    }
    gbr.cyc = now + spin_iters(period, room) * period;
#else
    (void)period; (void)read_ofs; (void)kind;
#endif
}

uint32_t gbl_spin_n(uint32_t period, uint32_t max) {
#if GBL_SPIN
    uint32_t k = spin_iters(period, (int32_t)(gbr.next_evt - gbr.cyc));
    if (k > max) k = max;
    gbr.cyc += k * period;
    return k;
#else
    (void)period; (void)max;
    return 0;
#endif
}

uint32_t gbl_spin_kind(uint16_t a) {
    if (a < 0xFF00 || a >= 0xFF80) return 0;
    switch (a & 0xFF) {
    case 0x00: case 0x0F: return 1;
    case 0x41: case 0x44: return 2;
    case 0x40: case 0x42: case 0x43: case 0x45: case 0x47: case 0x48:
    case 0x49: case 0x4A: case 0x4B: return 0;
    default: return 3;
    }
}

/* ---- joypad ------------------------------------------------------------- */

static uint8_t buttons;
/* P1 as read, kept current by P1 writes and gbl_set_buttons: generated code
 * reads it directly (games read P1 several times in a row to let it
 * settle, Pinball Dreams some 700 times a frame). */
uint8_t gbl_joyp;

static void joypad_update(void) {
    uint8_t sel = gbl_io[0x00];
    uint8_t v = 0x0F;
    if (!(sel & 0x10)) v &= (uint8_t)~(buttons >> 4);
    if (!(sel & 0x20)) v &= (uint8_t)~(buttons & 0x0F);
    gbl_joyp = (uint8_t)(0xC0 | (sel & 0x30) | (v & 0x0F));
}

void gbl_set_buttons(uint8_t pressed) {
    if (pressed & ~buttons) IF |= 0x10;
    buttons = pressed;
    joypad_update();
}

/* ---- IO ----------------------------------------------------------------- */

/* LY and STAT (0x44, 0x41), polled in wait loops, are read by the
 * assembly gbl_io_rd (gbl_timing_asm.S); it jumps here for the others. */
#if GBL_TIMING_ASM
uint8_t gbl_io_rd_c(uint8_t n);
#define gbl_io_rd gbl_io_rd_c
#endif
uint8_t gbl_io_rd(uint8_t n) {
    switch (n) {
    case 0x00: return gbl_joyp;
    case 0x02: return gbl_io[0x02] | 0x7E;
    case 0x04: return (uint8_t)((gbr.cyc - div_base) >> 8);
    case 0x05: timer_sync(gbr.cyc); return TIMA;
    case 0x07: return TAC | 0xF8;
    case 0x0F: gbl_sync(); return IF | 0xE0;
    default:
        if (n >= 0x10 && n < 0x40) return gbl_apu_read(n);
        if (n >= 0x40 && n < 0x4C) return gbl_ppu_read(n);
        return gbl_io[n];
    }
}
#if GBL_TIMING_ASM
#undef gbl_io_rd
#endif

/* Scroll, palette and window writes are handled by the assembly gbl_io_wr
 * (gbl_timing_asm.S); it jumps here for the others. */
#if GBL_TIMING_ASM
void gbl_io_wr_c(uint8_t n, uint8_t v);
#define gbl_io_wr gbl_io_wr_c
#endif
void gbl_io_wr(uint8_t n, uint8_t v) {
    switch (n) {
    case 0x00: gbl_io[0x00] = v & 0x30; joypad_update(); return;
    case 0x02:
        gbl_io[0x02] = v;
        if ((v & 0x81) == 0x81) {
            serial_busy = 1;
            core_invalidate();
            serial_done = gbr.cyc + 8u * 512u;
            gbl_update_evt();
        }
        return;
    case 0x04:
        timer_sync(gbr.cyc);
        div_base = gbr.cyc;
        core_invalidate();
        tima_sync = gbr.cyc;
        gbl_update_evt();
        return;
    case 0x05: timer_sync(gbr.cyc); TIMA = v; core_invalidate(); gbl_update_evt(); return;
    case 0x06: timer_sync(gbr.cyc); TMA = v; return;
    case 0x07: timer_sync(gbr.cyc); TAC = v & 7; core_invalidate(); gbl_update_evt(); return;
    case 0x0F: gbl_sync(); IF = v & 0x1F; gbl_update_evt(); return;
    case 0x46: {
        /* OAM DMA completes instantly; games wait it out in HRAM anyway. */
        uint16_t src = (uint16_t)(v << 8);
        uint32_t changed = 0;
        gbl_ppu_sync(gbr.cyc);
        /* XX00-XX9F never leaves its 4 KiB page: a mapped page is copied
         * straight, without a gbl_rd call per byte (about 30 cycles each
         * on the M0+, 4,800 per DMA). */
        const uint8_t *page = gbl_rmap[src >> 12];
        if (page) {
#if GBL_CORE_ASM && defined(__ARM_ARCH_6M__)
            changed = gbl_oam_dma_asm(gbl_oam, page + src);
#else
            const uint8_t *p = page + src;
            for (int i = 0; i < 0xA0; i++) {
                changed |= (uint32_t)(p[i] ^ gbl_oam[i]);
                gbl_oam[i] = p[i];
            }
#endif
        } else {
            for (int i = 0; i < 0xA0; i++) {
                uint8_t b = gbl_rd((uint16_t)(src + i));
                changed |= (uint32_t)(b ^ gbl_oam[i]);
                gbl_oam[i] = b;
            }
        }
        if (changed) {
            gbl_oam_dirty = 1;
            gbl_ppu_content_changed();
        }
        gbl_io[0x46] = v;
        return;
    }
    default:
        if (n >= 0x10 && n < 0x40) { gbl_apu_write(n, v); return; }
        if (n >= 0x40 && n < 0x4C) { gbl_ppu_write(n, v); return; }
        gbl_io[n] = v;
        return;
    }
}
#if GBL_TIMING_ASM
#undef gbl_io_wr
#endif

/* ---- bus ---------------------------------------------------------------- */

#ifdef GBL_ROM_TRACE
uint8_t gbl_rom_trace_read(uint32_t bank, uint16_t a); /* host harness */
uint8_t gbl_rom_fetch; /* set by the interpreter while fetching opcodes */
#endif

/* Unmapped pages (IO, OAM, HRAM, MBC registers, streamed ROM...); also the
 * tail of the assembly gbl_rd. */
uint8_t gbl_rd_slow(uint16_t a) {
#ifdef GBL_ROM_TRACE
    if (a >= 0x4000 && a < 0x8000) return gbl_rom_trace_read(gbr.rom_bank, a);
#endif
#if GBL_ROM_STREAM
    if (a >= 0x4000 && a < 0x8000) return stream_read(gbr.rom_bank * 0x4000u + (a - 0x4000u));
#endif
    if (a >= 0xFF80) return a == 0xFFFF ? gbl_ie : gbl_hram[a - 0xFF80];
    if (a >= 0xFF00) return gbl_io_rd((uint8_t)a);
    if (a >= 0xFE00) return a < 0xFEA0 ? gbl_oam[a - 0xFE00] : 0xFF;
    if (a >= 0xE000) return gbl_wram[a - 0xE000];
#if GBL_MBC == 2
    if (a >= 0xA000 && a < 0xC000 && mbc.ram_enable)
        return eram_load(eram_offset(a)) | 0xF0;
#elif GBL_ERAM_SIZE > 0
    if (a >= 0xA000 && a < 0xC000 && mbc.ram_enable)
        return eram_load(eram_offset(a));
#endif
    return 0xFF;
}

#if !(GBL_CORE_ASM && defined(__ARM_ARCH_6M__)) /* else gbl_core_asm.S */
uint8_t gbl_rd(uint16_t a) {
    const uint8_t *p = gbl_rmap[a >> 12];
    if (p) return p[a];
#if GBL_ERAM_ON_SD
    if (a - gbl_eram_rwin < 512u) return gbl_eram_rptr[a];
#endif
    return gbl_rd_slow(a);
}
#endif

void gbl_vram_wr(uint16_t off, uint8_t v) {
    if (gbl_vram[off] == v) return;
    /* Lines already past mode 3 must be drawn with the old contents. */
    gbl_ppu_sync(gbr.cyc);
    gbl_vram[off] = v;
    gbl_ppu_vram_changed(off);
}

#if !(GBL_CORE_ASM && defined(__ARM_ARCH_6M__)) /* else gbl_core_asm.S */
void gbl_wr(uint16_t a, uint8_t v) {
    uint8_t *p = gbl_wmap[a >> 12];
    if (p) { p[a] = v; return; }
#if GBL_ERAM_ON_SD
    if (a - gbl_eram_wwin < 512u) {
        if (gbl_eram_wptr[a] != v) {
            gbl_eram_wptr[a] = v;
            gbl_save_last_change = gbl_frame;
        }
        return;
    }
#endif
    gbl_wr_slow(a, v);
}
#endif

/* Unmapped pages; also the tail of the assembly gbl_wr. */
void gbl_wr_slow(uint16_t a, uint8_t v) {
    if (a >= 0xFF80) {
        if (a == 0xFFFF) { gbl_ie = v; gbl_update_evt(); }
        else gbl_hram[a - 0xFF80] = v;
        return;
    }
    if (a >= 0xFF00) { gbl_io_wr((uint8_t)a, v); return; }
    if (a >= 0xFE00) {
        if (a < 0xFEA0 && gbl_oam[a - 0xFE00] != v) {
            gbl_ppu_sync(gbr.cyc);
            gbl_oam[a - 0xFE00] = v;
            gbl_oam_dirty = 1;
            gbl_ppu_content_changed();
        }
        return;
    }
    if (a >= 0x8000 && a < 0xA000) { gbl_vram_wr((uint16_t)(a - 0x8000), v); return; }
    if (a >= 0xE000) { gbl_wram[a - 0xE000] = v; return; }
    if (a < 0x8000) { mbc_write(a, v); return; }
#if GBL_MBC == 2
    if (a >= 0xA000 && a < 0xC000 && mbc.ram_enable) eram_store(eram_offset(a), v & 0x0F);
#elif GBL_ERAM_SIZE > 0
    if (a >= 0xA000 && a < 0xC000 && mbc.ram_enable) eram_store(eram_offset(a), v);
#endif
}

#if !(GBL_CORE_ASM && defined(__ARM_ARCH_6M__)) /* else gbl_core_asm.S */
void gbl_push(uint16_t v) {
    gbr.sp = (uint16_t)(gbr.sp - 2);
    uint16_t s = gbr.sp;
    if (s >= 0xC000 && s < 0xDFFF) {
        gbl_wram[s - 0xC000] = (uint8_t)v;
        gbl_wram[s - 0xC000 + 1] = (uint8_t)(v >> 8);
        return;
    }
    gbl_wr((uint16_t)(s + 1), (uint8_t)(v >> 8));
    gbl_wr(s, (uint8_t)v);
}

uint16_t gbl_pop(void) {
    uint16_t s = gbr.sp;
    gbr.sp = (uint16_t)(s + 2);
    if (s >= 0xC000 && s < 0xDFFF)
        return (uint16_t)(gbl_wram[s - 0xC000] | (gbl_wram[s - 0xC000 + 1] << 8));
    uint8_t lo = gbl_rd(s);
    return (uint16_t)(lo | (gbl_rd((uint16_t)(s + 1)) << 8));
}
#endif

/* ---- interrupts and execution ------------------------------------------- */

uint8_t gbl_safe_request;
__attribute__((weak)) void gbl_platform_safe_point(void) {}

int gbl_irq_enter(void) {
    /* Every caller is between two instructions, gbr.pc exact. */
    if (gbl_safe_request) {
        gbl_safe_request = 0;
        gbl_platform_safe_point();
    }
    uint8_t pending = gbl_ie & IF & 0x1F;
    if (!gbr.ime || !pending) return 0;
    int bit = 0;
    while (!(pending & (1u << bit))) bit++;
    IF &= (uint8_t)~(1u << bit);
    gbr.ime = 0;
    gbr.cyc += 20;
    gbl_push(gbr.pc);
    gbr.pc = (uint16_t)(0x40 + bit * 8);
    gbl_update_evt();
    return 1;
}

int gbl_event(uint16_t resume_pc) {
    for (;;) {
        gbl_sync();
        gbr.pc = resume_pc;
        if (!gbl_irq_enter()) return 0;
        gbl_exec();
        if (gbr.pc != resume_pc) return 1;
    }
}

/* Wait in HALT until an interrupt is pending, then service it when IME is
 * set.  Returns nonzero when control did not come back to resume_pc. */
void gbl_halt_wait(void) {
    gbl_sync();
    while (!(gbl_ie & IF & 0x1F)) {
        /* next_evt is bounded to one frame and the PPU keeps a virtual frame
         * clock while the LCD is off, so this always makes progress. */
        gbr.cyc = gbr.next_evt;
        gbl_sync();
    }
}

int gbl_halt(uint16_t resume_pc) {
    gbl_halt_wait();
    return gbl_event(resume_pc);
}

/* Dispatch: binary search of the bank's sorted entry table, behind a small
 * direct-mapped cache (interrupt returns and unwinds resume at the same few
 * addresses).  Misses, i.e. addresses left to the interpreter, are cached
 * too. */
#if GBL_LOOKUP_CACHE
/* Global for the assembly gbl_lookup (gbl_timing_asm.S), which handles
 * hits and jumps to gbl_lookup_miss otherwise. */
typedef struct {
    uint16_t pc, bank;
    gbl_fn fn;
} gbl_lookup_entry_t;
gbl_lookup_entry_t gbl_lookup_cache[GBL_LOOKUP_CACHE];
#define lookup_cache gbl_lookup_cache
#if GBL_TIMING_ASM /* 32-bit pointers */
_Static_assert(sizeof(gbl_lookup_entry_t) == 8 && offsetof(gbl_lookup_entry_t, bank) == 2 &&
               offsetof(gbl_lookup_entry_t, fn) == 4, "gbl_lookup_entry_t layout");
#endif
_Static_assert(offsetof(gbl_cpu_t, rom_bank) == GBL_GBR_ROM_BANK, "rom_bank");
#endif

/* The dispatch table of pc's bank, or NULL when nothing is compiled in the
 * page of pc (RAM, a bank without code, or an empty 128-byte page). */
static const gbl_bank_table *lookup_page(uint16_t pc) {
    const gbl_bank_table *t;
    if (pc < 0x4000) t = &gbl_tables[0];
    else if (pc < 0x8000 && gbr.rom_bank < gbl_table_count) t = &gbl_tables[gbr.rom_bank];
    else return 0;
    if (!t->count) return 0;
    uint32_t page = (pc & 0x3FFFu) >> 7;
    return t->idx[17 + (page >> 4)] >> (page & 15) & 1 ? t : 0;
}

/* Binary search of the entries in pc's 1 KiB block. */
static gbl_fn lookup_table(const gbl_bank_table *t, uint16_t pc) {
    uint32_t block = (pc & 0x3FFFu) >> 10;
    uint32_t lo = t->idx[block], hi = t->idx[block + 1];
    const uint16_t *addr = t->addr;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        uint16_t v = addr[mid];
        if (v == pc) return t->fn[mid];
        if (v < pc) lo = mid + 1; else hi = mid;
    }
    return 0;
}

#if GBL_TIMING_ASM && GBL_LOOKUP_CACHE
gbl_fn gbl_lookup_miss(uint16_t pc);
#define gbl_lookup gbl_lookup_miss
#endif
gbl_fn gbl_lookup(uint16_t pc) {
    /* Pages with nothing compiled are answered here and never cached: the
     * interpreter's transfers inside uncompiled code would evict entries. */
    const gbl_bank_table *t = lookup_page(pc);
    if (!t) return 0;
#if GBL_LOOKUP_CACHE
    uint16_t bank = pc < 0x4000 ? 0 : gbr.rom_bank;
    unsigned i = (pc ^ (pc >> 5)) & (GBL_LOOKUP_CACHE - 1);
#if !GBL_TIMING_ASM /* the assembly gbl_lookup checked the entry already */
    if (lookup_cache[i].pc == pc && lookup_cache[i].bank == bank) return lookup_cache[i].fn;
#endif
    gbl_fn fn = lookup_table(t, pc);
    lookup_cache[i].pc = pc;
    lookup_cache[i].bank = bank;
    lookup_cache[i].fn = fn;
    return fn;
#else
    return lookup_table(t, pc);
#endif
}
#if GBL_TIMING_ASM && GBL_LOOKUP_CACHE
#undef gbl_lookup
#endif

/* The assembly interpreter's register offsets (B C D E H L - A) and the
 * innermost gbl_exec's bound (see gbl_exec), in RAM next to each other. */
gbl_interp_ctx_t gbl_interp_ctx = {{1, 0, 3, 2, 5, 4, 0xFF, 10}, 0, 0};
_Static_assert(offsetof(gbl_interp_ctx_t, sp0) == 8 && offsetof(gbl_interp_ctx_t, bounded) == 10,
               "gbl_interp_ctx_t layout");

/* gbl_exec only runs with this much stack above gbl_stack_limit, so the
 * region it calls always passes its own ENTER() check: otherwise a region
 * refused at entry would be called again forever without progress (seen
 * with deep call chains in R.C. Pro-Am under the META's 3 KiB budget). */
#ifndef GBL_EXEC_HEADROOM
#define GBL_EXEC_HEADROOM 512u
#endif

/* The innermost gbl_exec's entry SP, or no bound under gbl_run: the
 * assembly interpreter continues across a transfer only while this loop
 * would call it again (gbl_interp_asm.S). */
#define gbl_exec_sp0 gbl_interp_ctx.sp0
#define gbl_exec_bounded gbl_interp_ctx.bounded

void gbl_exec(void) {
    char probe;
    if ((uintptr_t)&probe < gbl_stack_limit + GBL_EXEC_HEADROOM) return; /* unwind: callers resume */
    uint16_t sp0 = gbr.sp, outer_sp0 = gbl_exec_sp0;
    uint8_t outer_bounded = gbl_exec_bounded;
    gbl_exec_sp0 = sp0;
    gbl_exec_bounded = 1;
    do {
        gbl_fn fn = gbl_interp_only ? 0 : gbl_lookup(gbr.pc);
        if (fn) fn(); else gbl_interp_block();
    } while ((int16_t)(gbr.sp - sp0) <= 0);
    gbl_exec_sp0 = outer_sp0;
    gbl_exec_bounded = outer_bounded;
}

void gbl_run(void) {
    char probe;
    /* Same margin as gbl_exec: a region called here must pass its guard,
     * otherwise it returns without progress and this loop never ends. */
    int room = (uintptr_t)&probe >= gbl_stack_limit + GBL_EXEC_HEADROOM;
    gbl_exec_bounded = 0;
    for (;;) {
        gbl_fn fn = (gbl_interp_only || !room) ? 0 : gbl_lookup(gbr.pc);
        if (fn) fn(); else gbl_interp_block();
    }
}

/* ---- reset ---------------------------------------------------------------- */

/* Every page pointer, from the mapper registers. */
static void map_pages(void) {
    for (int i = 0; i < 16; i++) { gbl_rmap[i] = 0; gbl_wmap[i] = 0; }
    for (int i = 0; i < 4; i++) gbl_rmap[i] = gbl_rom;
    mapped_bank = 0xFFFF;
    map_rom_bank();
    gbl_rmap[0x8] = gbl_rmap[0x9] = BIAS(gbl_vram, 0x8000u);
#if !GBL_VRAM_HOOK
    gbl_wmap[0x8] = gbl_wmap[0x9] = BIAS(gbl_vram, 0x8000u);
#endif
    gbl_rmap[0xC] = gbl_rmap[0xD] = BIAS(gbl_wram, 0xC000u);
    gbl_wmap[0xC] = gbl_wmap[0xD] = BIAS(gbl_wram, 0xC000u);
    /* E000-EFFF echoes C000-CFFF (F000-FDFF shares a page with OAM, IO
     * and HRAM and stays on the slow path). */
    gbl_rmap[0xE] = gbl_wmap[0xE] = BIAS(gbl_wram, 0xE000u);
    map_eram();
}

void gbl_reset(void) {
    memset(&gbr, 0, sizeof(gbr));
    memset(gbl_wram, 0, sizeof(gbl_wram));
    memset(gbl_vram, 0, sizeof(gbl_vram));
    memset(gbl_oam, 0, sizeof(gbl_oam));
    gbl_oam_dirty = 1;
    memset(gbl_hram, 0, sizeof(gbl_hram));
    memset(gbl_io, 0, sizeof(gbl_io));
    memset(&mbc, 0, sizeof(mbc));
#if GBL_ERAM_SIZE > 0
    eram_reset();
#endif
#if GBL_LOOKUP_CACHE
    /* pc 0xFFFF is never looked up (>= 0x8000): an empty slot. */
    for (int i = 0; i < GBL_LOOKUP_CACHE; i++) lookup_cache[i].pc = 0xFFFF;
#endif
#if GBL_ROM_STREAM
    stream_reset();
#endif

    /* DMG register state after the boot ROM. */
    gbr.a = 0x01; gbr.zr = 0; gbr.nf = 0; gbr.hf = 1; gbr.cf = 1;
    gbr.bc = 0x0013; gbr.de = 0x00D8; gbr.hl = 0x014D;
    gbr.sp = 0xFFFE; gbr.pc = 0x0100;
    gbl_ie = 0;
    gbl_io[0x00] = 0x30;
    joypad_update();
    IF = 0x01;
    TAC = 0; TIMA = 0; TMA = 0;
    div_base = (uint32_t)0 - 0xABCCu;
    tima_sync = 0;
    serial_busy = 0;
    core_invalidate();

    map_pages();

    gbl_apu_reset();
    gbl_ppu_reset();
    gbl_update_evt();
}

/* ---- save states ------------------------------------------------------------ */

static uint8_t io_counting; /* gbl_state_bytes: no card access */

static void state_mem(gbl_state_fn io, void *ctx, int load) {
    io(&gbr, sizeof(gbr), ctx);
    io(gbl_wram, sizeof(gbl_wram), ctx);
    io(gbl_vram, sizeof(gbl_vram), ctx);
    io(gbl_oam, sizeof(gbl_oam), ctx);
    io(gbl_hram, sizeof(gbl_hram), ctx);
    io(gbl_io, sizeof(gbl_io), ctx);
    io(&gbl_ie, sizeof(gbl_ie), ctx);
    io(&mbc, sizeof(mbc), ctx);
    io(&gbl_timer_state, sizeof(gbl_timer_state), ctx);
    io(&buttons, sizeof(buttons), ctx);
    io(&gbl_frame, sizeof(gbl_frame), ctx);
#if GBL_ERAM_SIZE > 0 && GBL_ERAM_ON_SD
    /* The cartridge RAM is the save file: copied through the sector cache's
     * first buffer, whose contents were written back first. */
    if (!io_counting) gbl_save_flush();
    for (uint32_t sec = 0; sec * 512u < GBL_ERAM_SIZE; sec++) {
        if (io_counting) {
            io(eram_buf[0], 512, ctx);
        } else if (load) {
            io(eram_buf[0], 512, ctx);
            gbl_platform_save_write(sec, eram_buf[0]);
        } else {
            if (!gbl_platform_save_read(sec, eram_buf[0])) memset(eram_buf[0], 0xFF, 512);
            io(eram_buf[0], 512, ctx);
        }
    }
    if (!io_counting) eram_reset();
#elif GBL_ERAM_SIZE > 0
    io(gbl_eram, GBL_ERAM_SIZE, ctx);
#if GBL_BATTERY
    if (load) { /* the save file follows the loaded RAM (meta_save_tick) */
        eram_dirty_mask = 0xFFFFFFFFu >> (32u - (GBL_ERAM_SIZE + 511u) / 512u);
        save_pending = 1;
        gbl_save_last_change = gbl_frame;
    }
#endif
#endif
    (void)load;
}

void gbl_state_io(gbl_state_fn io, void *ctx, int load) {
    state_mem(io, ctx, load);
    gbl_ppu_state_io(io, ctx, load);
    gbl_apu_state_io(io, ctx, load);
    if (!load) return;
    map_pages();
#if GBL_LOOKUP_CACHE
    for (int i = 0; i < GBL_LOOKUP_CACHE; i++) lookup_cache[i].pc = 0xFFFF;
#endif
    joypad_update();
    core_invalidate();
    gbl_update_evt();
}

static void count_bytes(void *data, uint32_t bytes, void *ctx) {
    (void)data;
    *(uint32_t *)ctx += bytes;
}

/* Size of a state (nothing is read or written: counting only). */
uint32_t gbl_state_bytes(void) {
    uint32_t n = 0;
    io_counting = 1;
    gbl_state_io(count_bytes, &n, 0);
    io_counting = 0;
    return n;
}
