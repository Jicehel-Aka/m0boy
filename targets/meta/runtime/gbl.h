/*
 * gbl - compact DMG runtime for memory-constrained targets (Gamebuino META).
 *
 * The desktop runtime (runtime/) favours instrumentation and accuracy
 * tooling; this one favours flash and RAM footprint.  Devices are advanced
 * lazily from a single T-cycle counter and only publish events when an
 * interrupt or a rendered scanline can become observable.
 *
 * Flag representation used by both the interpreter and generated code:
 *   Z is set      <=> gbr.zr == 0
 *   C, H, N       are stored as 0/1 bytes in cf, hf, nf
 */
#ifndef GBL_H
#define GBL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte fields stay below offset 32 and words below 128 so Thumb-1 can reach
 * every register with a single immediate-offset load/store. */
typedef struct {
    union { struct { uint8_t c, b; }; uint16_t bc; };
    union { struct { uint8_t e, d; }; uint16_t de; };
    union { struct { uint8_t l, h; }; uint16_t hl; };
    uint16_t sp;
    uint16_t pc;
    uint8_t a;
    uint8_t zr;
    uint8_t cf;
    uint8_t hf;
    uint8_t nf;
    uint8_t ime;
    uint16_t rom_bank;
    uint32_t cyc;
    uint32_t next_evt;
} gbl_cpu_t;

extern gbl_cpu_t gbr;

extern uint8_t gbl_wram[0x2000];
extern uint8_t gbl_vram[0x2000];
extern uint8_t gbl_oam[0xA0];
extern uint8_t gbl_hram[0x80];
extern uint8_t gbl_io[0x80];
extern uint8_t gbl_ie;

/* Biased page pointers: gbl_rmap[a >> 12][a] is the byte at a, NULL pages go
 * through the slow path (IO, OAM, MBC registers, disabled cart RAM). */
extern const uint8_t *gbl_rmap[16];
extern uint8_t *gbl_wmap[16];

/* Provided by the generated code (empty tables for interpreter-only
 * projects). */
typedef void (*gbl_fn)(void);
typedef struct {
    uint16_t count;
    const uint16_t *addr; /* sorted */
    const gbl_fn *fn;
    /* Within the bank's 16 KiB: idx[i] (i <= 16) is the first entry at or
     * above offset 0x400 * i, and bit p % 16 of idx[17 + p / 16] is set when
     * 128-byte page p holds an entry. */
    const uint16_t *idx;
} gbl_bank_table;
extern const gbl_bank_table gbl_tables[];
extern const uint16_t gbl_table_count;
extern const uint8_t gbl_rom[];
extern const uint32_t gbl_rom_size;

/* ---- lifecycle ---------------------------------------------------------- */
void gbl_reset(void);
void gbl_run(void);                 /* never returns */

/* ---- platform hooks (implemented by the host / Meta frontend) ---------- */
/* Destination for DMG scanline ly (160 pixels, 4-byte aligned), or NULL to
 * skip rendering it.  Pixels are gbl_lcd_palette[shade], shade 0 = white ..
 * 3 = black.  Call gbl_ppu_palette_changed() after editing the palette. */
uint16_t *gbl_platform_line_begin(uint8_t ly);
void gbl_platform_line_end(uint8_t ly);
/* With GBL_LINE_SIGNATURE: called before line_begin with a signature of all
 * inputs of line ly (not including gbl_lcd_palette); return 0 to skip it. */
int gbl_platform_line_check(uint8_t ly, uint32_t sig);
/* Nonzero: line_begin buffers receive 20 groups of eight BGP/OBP-mapped
 * 2-bit shades (uint16, leftmost pixel in bits 15:14) instead of 160
 * colours; gbl_lcd_palette is then not used for that line. */
extern uint8_t gbl_ppu_packed;
/* With GBL_PPU_PAL32: set to 0 while gbl_lcd_palette holds a temporary
 * palette so the 2 KiB table is not rebuilt for it. */
extern uint8_t gbl_lcd_palette_primary;
extern uint16_t gbl_lcd_palette[4];
void gbl_ppu_palette_changed(void);
/* Signature of the frame whose visible lines just completed (valid in
 * gbl_platform_vblank); equal signatures mean identical pictures.  Returns
 * 0 when GBL_FRAME_SIGNATURE is disabled. */
uint32_t gbl_ppu_frame_signature(void);
/* Static-frame policy, called once per VBlank: pass whether the frame that
 * just ended was rendered and displayed; returns nonzero when the next frame
 * is expected to look exactly like the picture on screen (signature stable
 * for two frames).  A change is then shown one frame late.  Call
 * gbl_frame_invalidate() when the frontend's palette or layout changes. */
int gbl_frame_unchanged_next(int shown);
/* Order-dependent 32-bit combination of two signatures (full avalanche). */
uint32_t gbl_sig_combine(uint32_t a, uint32_t b);
void gbl_frame_invalidate(void);
/* Entered VBlank.  The frontend presents, paces, and polls input here. */
void gbl_platform_vblank(void);
/* Set by the frontend: nonzero skips scanline rasterization this frame. */
extern uint8_t gbl_skip_render;
/* Joypad state, 1 = pressed.  bit0 A, 1 B, 2 Select, 3 Start,
 * 4 Right, 5 Left, 6 Up, 7 Down. */
void gbl_set_buttons(uint8_t pressed);
extern uint8_t gbl_joyp;           /* P1 (0xFF00) as read */

/* ---- ROM streaming (projects generated with GBL_ROM_STREAM) ------------- */
/* Read 512-byte sector `sector` of the ROM (byte offset sector * 512) into
 * dst; return 0 on failure (the bytes then read as 0xFF). */
int gbl_platform_rom_read(uint32_t sector, uint8_t *dst);
/* 512 bytes of the stream cache, free for the platform before gbl_reset()
 * (e.g. to locate the ROM file); gbl_reset() empties the cache. */
uint8_t *gbl_stream_scratch(void);
/* Empties the stream cache, after the platform used all its buffers
 * (GBL_STREAM_SECTORS x 512 bytes from gbl_stream_scratch()). */
void gbl_stream_invalidate(void);
extern uint32_t gbl_stream_misses;  /* sectors read so far */
extern uint32_t gbl_stream_unpacked; /* ... decoded from compressed flash */

/* ---- cartridge RAM saves ------------------------------------------------- */
/* Sector `sector` (512 bytes) of the cartridge RAM file; return 0 when there
 * is no file (reads then give 0xFF, writes are dropped). */
int gbl_platform_save_read(uint32_t sector, uint8_t *dst);
int gbl_platform_save_write(uint32_t sector, const uint8_t *src);
void gbl_save_flush(void);           /* write back what changed */
/* Write back the first changed sector (cache way, or 512-byte sector of
 * in-RAM cartridge RAM) at index i or after; returns the index after it,
 * or -1 when none is left.  Spreads a periodic save over several frames. */
int gbl_save_flush_next(int i);
int gbl_save_pending(void);          /* changes not written yet */
extern uint32_t gbl_save_last_change; /* gbl_frame of the last change */
/* With GBL_ERAM_ON_SD: newest cartridge RAM sector, read and written by
 * gbl_rd / gbl_wr without the slow path (gbl_core.c). */
#define GBL_ERAM_WIN_NONE 0x10000u
extern uint32_t gbl_eram_rwin, gbl_eram_wwin;
extern uint8_t *gbl_eram_rptr, *gbl_eram_wptr;
/* 512 bytes free for the platform before gbl_reset() (NULL without
 * cartridge RAM). */
uint8_t *gbl_save_scratch(void);

/* ---- audio -------------------------------------------------------------- */
/* Unsigned 8-bit mono samples (128 = silence) at the configured rate. */
void gbl_apu_set_rate(uint32_t hz);
void gbl_apu_render(uint8_t *out, unsigned n);
extern uint8_t gbl_apu_mute;        /* debug: bit c silences channel c */

/* ---- memory ------------------------------------------------------------- */
uint8_t gbl_rd(uint16_t a);
void gbl_wr(uint16_t a, uint8_t v);
uint8_t gbl_io_rd(uint8_t n);        /* 0xFF00 + n, n < 0x80 */
void gbl_io_wr(uint8_t n, uint8_t v);
void gbl_vram_wr(uint16_t off, uint8_t v); /* off = address - 0x8000 */
void gbl_push(uint16_t v);
uint16_t gbl_pop(void);

/* ---- execution control -------------------------------------------------- */
int gbl_event(uint16_t resume_pc);   /* nonzero: control left resume_pc */
int gbl_halt(uint16_t resume_pc);
void gbl_exec(void);                 /* run from gbr.pc until the frame returns */
void gbl_sync(void);                 /* advance devices to gbr.cyc */
void gbl_update_evt(void);
gbl_fn gbl_lookup(uint16_t pc);
void gbl_interp_block(void);

/* ---- wait-loop fast-forward (generated back edges) ---------------------- */
/* Advance gbr.cyc by whole loop periods while every skipped iteration would
 * read the same values.  kind: 0 reads change only when code runs, 1 at a
 * device event, 2 at the next LY/STAT change after a read read_ofs cycles
 * into the iteration, 3 never skip (see gbl_spin_kind). */
void gbl_spin(uint32_t period, uint32_t read_ofs, uint32_t kind);
/* Delay loop: skip up to max iterations; returns how many were skipped. */
uint32_t gbl_spin_n(uint32_t period, uint32_t max);
uint32_t gbl_spin_kind(uint16_t addr);

/* ---- ALU helpers (full flag results) ----------------------------------- */
void gbl_add(uint8_t v);
void gbl_adc(uint8_t v);
void gbl_sub(uint8_t v);
void gbl_sbc(uint8_t v);
void gbl_cp(uint8_t v);
uint8_t gbl_inc(uint8_t v);
uint8_t gbl_dec(uint8_t v);
void gbl_addhl(uint16_t v);
uint16_t gbl_spofs(uint8_t v);       /* SP + e8 with flags (ADD SP / LD HL,SP+) */
void gbl_daa(void);
uint8_t gbl_cbop(uint8_t op, uint8_t v); /* CB 00-3F rotate/shift group */
uint8_t gbl_getf(void);
void gbl_setf(uint8_t f);

/* ---- save states ------------------------------------------------------- */
/* The machine (CPU, memories, mapper, timer, PPU, APU and the cartridge RAM,
 * also when it lives in the save file) is streamed through io: the same
 * order for saving and loading.  Call them at an instruction boundary,
 * i.e. from gbl_platform_safe_point; after gbl_state_io(.., 1) execution
 * must start over from gbr.pc (gbl_run again, the C stack dropped). */
typedef void (*gbl_state_fn)(void *data, uint32_t bytes, void *ctx);
void gbl_state_io(gbl_state_fn io, void *ctx, int load);
uint32_t gbl_state_bytes(void);
/* Set by the frontend: gbl_platform_safe_point() is called at the next
 * interrupt check, between two instructions with gbr.pc exact. */
extern uint8_t gbl_safe_request;
void gbl_platform_safe_point(void); /* weak default: nothing */

/* ---- diagnostics -------------------------------------------------------- */
extern uint8_t gbl_interp_only;     /* ignore compiled code (reference mode) */
extern uint32_t gbl_frame;
extern uint32_t gbl_interp_instrs;
extern uintptr_t gbl_stack_limit;   /* lowest C stack address gbl_exec may use */
#ifdef GBL_TRACE
void gbl_trace_hook(uint16_t pc);
#endif
#ifdef GBL_COVERAGE
void gbl_coverage_hit(uint16_t bank, uint16_t addr, int entry);
#endif

#ifdef __cplusplus
}
#endif

#endif
