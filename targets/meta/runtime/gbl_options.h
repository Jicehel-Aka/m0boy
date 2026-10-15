/*
 * Build options for the gbl runtime.  The value of each optimisation per
 * console is in gbl_profile.h; override any of them with -D.
 *
 * GBL_PPU_BATCH           Scanlines per PPU event when no per-line STAT
 *                         interrupt (mode 0 / mode 2) is enabled; 1 restores
 *                         one event per line.  Batching needs GBL_VRAM_HOOK so
 *                         VRAM/OAM writes first render the lines they follow.
 * GBL_FRAME_SIGNATURE     Track a per-frame signature of everything that
 *                         affects the picture (PPU register timeline and a
 *                         VRAM/OAM change counter), for static-frame skipping
 *                         in the frontend.
 * GBL_LINE_SIGNATURE      Before rendering each scanline, ask the frontend
 *                         gbl_platform_line_check(ly, sig) with a signature
 *                         of everything the line depends on (registers, the
 *                         map row and tile versions, its sprites); returning
 *                         0 skips rendering it.  Equal signatures mean equal
 *                         pixels, so skipping unchanged lines is exact.
 * GBL_PPU_PAL32           Convert background pixels four at a time with a
 *                         2 KiB table (gm0's pal32) instead of two at a time.
 * GBL_VRAM_HOOK           Route VRAM writes through gbl_vram_wr().  Forced on
 *                         by the two options above.
 * GBL_GEN_OPTIMIZE_SIZE   Compile generated code with -Os (it is a small part
 *                         of the run time and the largest part of the flash).
 * GBL_SPIN                Fast-forward wait loops recognised by the generator
 *                         (gbl_spin); 0 runs every iteration.  The results are
 *                         identical either way, only the time spent differs.
 * GBL_INTERP_DELAY_LOOP   Interpreter shortcut for DEC A / JR NZ,-3 (the OAM
 *                         DMA wait most games copy to HRAM).
 * GBL_PPU_ASM             Tile fetch and colour conversion in hand-written
 *                         Thumb-1 (gbl_ppu_asm.S).  Default on for ARMv6-M
 *                         (Cortex-M0+); the C loops are used elsewhere.
 * GBL_RENDER_LINE_ASM     render_line (sprite selection, line signature and
 *                         the calls to the frontend and draw_line) in
 *                         Thumb-1 (gbl_ppu_asm.S); follows GBL_PPU_ASM and
 *                         needs GBL_LINE_SIGNATURE.
 * GBL_PPU_RAMFUNC         Run those loops from SRAM (about 150 bytes), out of
 *                         reach of flash wait states.
 * GBL_CORE_ASM            gbl_push / gbl_pop in Thumb-1 (gbl_core_asm.S) on
 *                         Cortex-M0+.
 * GBL_TIMING_ASM          gbl_ppu_advance (the PPU line state machine),
 *                         gbl_update_evt and gbl_sync in Thumb-1
 *                         (gbl_timing_asm.S); default on for Cortex-M0+.
 * GBL_INTERP_ASM          Interpreter loop and common opcodes in Thumb-1
 *                         (gbl_interp_asm.S), the others through the C
 *                         gbl_interp_op; default on for Cortex-M0+ (not in
 *                         coverage or trace builds).
 * GBL_BATTERY             Cartridge RAM is saved (set by the generator for
 *                         battery-backed cartridges).
 * GBL_ERAM_ON_SD          Cartridge RAM lives in the platform's save file
 *                         behind a 1 KiB cache instead of RAM; default for
 *                         cartridge RAM larger than 2 KiB.
 * GBL_ROM_PACKED          The streamed part of the ROM comes from LZSS-
 *                         compressed flash instead of the SD card (set by the
 *                         generator: gbrecomp --meta-pack).
 * GBL_UNPACK_ASM          Decode those sectors with the Thumb-1 decoder
 *                         (gbl_core_asm.S); default on for Cortex-M0+.
 * GBL_STREAM_SECTORS      512-byte sectors cached for a streamed ROM
 *                         (GBL_ROM_STREAM projects only).
 */
#ifndef GBL_OPTIONS_H
#define GBL_OPTIONS_H

/* The console's choices for every optimisation switch above. */
#include "gbl_profile.h"

/* Every switch must come from the profile (a missing one would read as 0
 * in #if and silently turn an optimisation off). */
#if !defined(GBL_PPU_BATCH) || !defined(GBL_FRAME_SIGNATURE) || !defined(GBL_LINE_SIGNATURE) ||   \
    !defined(GBL_PPU_PAL32) || !defined(GBL_GEN_OPTIMIZE_SIZE) || !defined(GBL_SPIN) ||           \
    !defined(GBL_INTERP_DELAY_LOOP) || !defined(GBL_LOOKUP_CACHE) || !defined(GBL_STREAM_SECTORS) || \
    !defined(GBL_PPU_ASM) || !defined(GBL_RENDER_LINE_ASM) || !defined(GBL_PPU_RAMFUNC) ||        \
    !defined(GBL_CORE_ASM) || !defined(GBL_TIMING_ASM) || !defined(GBL_INTERP_ASM) ||             \
    !defined(GBL_UNPACK_ASM)
#error "gbl_profile.h: a runtime switch is missing from this console's profile"
#endif

/* ---- the cartridge (gbl_config.h) --------------------------------------- */
#ifndef GBL_BATTERY
#define GBL_BATTERY 0
#endif
#ifndef GBL_ROM_STREAM
#define GBL_ROM_STREAM 0
#endif
#ifndef GBL_ROM_PACKED
#define GBL_ROM_PACKED 0
#endif
#ifndef GBL_ROM_SD
#define GBL_ROM_SD (GBL_ROM_STREAM && !GBL_ROM_PACKED)
#endif

/* ---- what the build can do: Thumb-1 paths need an ARMv6-M compiler (not
 * the host harness), the assembly interpreter has no coverage or trace
 * hooks, the assembly render_line needs the assembly PPU and line
 * signatures. ---------------------------------------------------------------- */
#if !defined(__ARM_ARCH_6M__)
#undef GBL_PPU_ASM
#define GBL_PPU_ASM 0
#undef GBL_TIMING_ASM
#define GBL_TIMING_ASM 0
#undef GBL_INTERP_ASM
#define GBL_INTERP_ASM 0
#undef GBL_UNPACK_ASM
#define GBL_UNPACK_ASM 0
#endif
#if GBL_INTERP_ASM && (defined(GBL_COVERAGE) || defined(GBL_TRACE) || defined(GBL_ROM_TRACE))
#undef GBL_INTERP_ASM
#define GBL_INTERP_ASM 0
#endif
#if GBL_RENDER_LINE_ASM && !(GBL_PPU_ASM && GBL_LINE_SIGNATURE)
#undef GBL_RENDER_LINE_ASM
#define GBL_RENDER_LINE_ASM 0
#endif
#if GBL_PPU_BATCH > 1 || GBL_FRAME_SIGNATURE || GBL_LINE_SIGNATURE
#undef GBL_VRAM_HOOK
#define GBL_VRAM_HOOK 1
#endif
#ifndef GBL_VRAM_HOOK
#define GBL_VRAM_HOOK 0
#endif
#if GBL_LOOKUP_CACHE & (GBL_LOOKUP_CACHE - 1) || GBL_LOOKUP_CACHE > 256
#error "GBL_LOOKUP_CACHE must be 0 or a power of two up to 256"
#endif
#if GBL_PPU_BATCH < 1 || (GBL_PPU_BATCH & (GBL_PPU_BATCH - 1))
#error "GBL_PPU_BATCH must be a power of two"
#endif

#endif
