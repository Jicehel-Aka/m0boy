/*
 * Optimisation profiles: the one list to edit to turn an optimisation on or
 * off for a console.
 *
 * Every optimisation is a switch of its own (1 on, 0 off, or a size) and
 * can be flipped independently; a definition on the command line wins
 * (PlatformIO build_flags, the Pokitto Makefile's EXTRA_CFLAGS,
 * m0_estimate.py --cflag).  The profile is chosen by GBL_TARGET_POKITTO,
 * which gbrecomp writes into gbl_config.h for --target pokitto; anything
 * else (the META, the host harness, the tests) gets the META profile.
 *
 * What each switch does is documented where it is defined or used:
 *   GBL_*      runtime/gbl_options.h (the runtime, shared)
 *   META_*     platform/src (shared frontend), platform/meta (META drivers)
 *   POKITTO_*  platform/pokitto (Pokitto drivers, pokitto.ld)
 * gbl_options.h turns the Thumb-1 switches off on other compilers (the host
 * harness), whatever the profile says, and resolves their dependencies.
 *
 * Included by gbl_options.h (runtime), meta_target.h (frontend), the
 * assembly files and, through the C preprocessor, the Pokitto linker
 * script: definitions only.
 */
#ifndef GBL_PROFILE_H
#define GBL_PROFILE_H

#include "gbl_config.h"

/* ---- what the cartridge needs (gbl_config.h, written per game) -----------
 * A profile can choose differently for each kind of build; every game gets
 * its own firmware, so the right variant is picked at compile time, with no
 * second build to make:
 *   GBL_PROFILE_SD_ROM     ROM read from the SD card (sector cache, card
 *                          reads on cache misses)
 *   GBL_PROFILE_PACKED_ROM ROM LZSS-packed in flash (sector cache, unpacking
 *                          on cache misses)
 *   GBL_PROFILE_ERAM_ON_SD cartridge RAM in the save file (1 KiB cache)
 * GBL_ERAM_ON_SD itself is a cartridge choice: RAM above 2 KiB goes to the
 * card (override with -D). */
#ifndef GBL_ERAM_ON_SD
#define GBL_ERAM_ON_SD (GBL_ERAM_SIZE > 2048)
#endif
#if defined(GBL_ROM_STREAM) && GBL_ROM_STREAM && !(defined(GBL_ROM_PACKED) && GBL_ROM_PACKED)
#define GBL_PROFILE_SD_ROM 1
#else
#define GBL_PROFILE_SD_ROM 0
#endif
#if defined(GBL_ROM_PACKED) && GBL_ROM_PACKED
#define GBL_PROFILE_PACKED_ROM 1
#else
#define GBL_PROFILE_PACKED_ROM 0
#endif
#define GBL_PROFILE_ERAM_ON_SD (GBL_ERAM_SIZE > 0 && GBL_ERAM_ON_SD)

#if defined(GBL_TARGET_POKITTO) && GBL_TARGET_POKITTO
/* ======================================================= Pokitto (LPC11U68) */

/* Its two 2 KiB RAM banks hold the line signatures, the audio queue, the
 * PPU's BGP table and the dispatch cache (pokitto.ld); the main RAM this
 * frees pays for the larger stream cache.  0 keeps everything in main RAM
 * (and the META's cache sizes). */
#ifndef POKITTO_RAM_BANKS
#define POKITTO_RAM_BANKS 1
#endif

/* runtime */
#ifndef GBL_PPU_BATCH
#define GBL_PPU_BATCH 8
#endif
#ifndef GBL_FRAME_SIGNATURE
#define GBL_FRAME_SIGNATURE 1
#endif
#ifndef GBL_LINE_SIGNATURE
#define GBL_LINE_SIGNATURE 1
#endif
#ifndef GBL_PPU_PAL32
#define GBL_PPU_PAL32 0
#endif
#ifndef GBL_GEN_OPTIMIZE_SIZE
#define GBL_GEN_OPTIMIZE_SIZE 1
#endif
#ifndef GBL_SPIN
#define GBL_SPIN 1
#endif
#ifndef GBL_INTERP_DELAY_LOOP
#define GBL_INTERP_DELAY_LOOP 1
#endif
#ifndef GBL_LOOKUP_CACHE /* entries, a power of two; 256 = 2 KiB (USB RAM bank) */
#if POKITTO_RAM_BANKS
#define GBL_LOOKUP_CACHE 256
#else
#define GBL_LOOKUP_CACHE 64
#endif
#endif
#ifndef GBL_STREAM_SECTORS /* 512-byte ROM sectors cached (streamed or packed ROMs) */
#if POKITTO_RAM_BANKS
#define GBL_STREAM_SECTORS 8
#else
#define GBL_STREAM_SECTORS 4
#endif
#endif
#ifndef GBL_PPU_ASM
#define GBL_PPU_ASM 1
#endif
#ifndef GBL_RENDER_LINE_ASM
#define GBL_RENDER_LINE_ASM 1
#endif
#ifndef GBL_PPU_RAMFUNC
#define GBL_PPU_RAMFUNC 1
#endif
#ifndef GBL_CORE_ASM
#define GBL_CORE_ASM 1
#endif
#ifndef GBL_TIMING_ASM
#define GBL_TIMING_ASM 1
#endif
#ifndef GBL_INTERP_ASM
#define GBL_INTERP_ASM 1
#endif
#ifndef GBL_UNPACK_ASM
#define GBL_UNPACK_ASM 1
#endif

/* frontend */
#ifndef META_LINE_SKIP
#define META_LINE_SKIP 1
#endif
#ifndef META_STATIC_SKIP
#define META_STATIC_SKIP 1
#endif
#ifndef META_MAX_SKIP
#define META_MAX_SKIP 3
#endif
#ifndef POKITTO_LCD_ASM
#define POKITTO_LCD_ASM 1
#endif
#ifndef POKITTO_AUDIO_ASM
#define POKITTO_AUDIO_ASM 1
#endif

#else
/* ============================================== Gamebuino META (SAMD21) */

/* runtime */
#ifndef GBL_PPU_BATCH
#define GBL_PPU_BATCH 8
#endif
#ifndef GBL_FRAME_SIGNATURE
#define GBL_FRAME_SIGNATURE 1
#endif
#ifndef GBL_LINE_SIGNATURE
#define GBL_LINE_SIGNATURE 1
#endif
#ifndef GBL_PPU_PAL32
#define GBL_PPU_PAL32 0
#endif
#ifndef GBL_GEN_OPTIMIZE_SIZE
#define GBL_GEN_OPTIMIZE_SIZE 1
#endif
#ifndef GBL_SPIN
#define GBL_SPIN 1
#endif
#ifndef GBL_INTERP_DELAY_LOOP
#define GBL_INTERP_DELAY_LOOP 1
#endif
#ifndef GBL_LOOKUP_CACHE
#define GBL_LOOKUP_CACHE 64
#endif
/* 4: streamed builds have about 2 KiB of RAM left, and that is the stack.
 * More sectors read the card much less (Donkey Kong Land: 764 reads with
 * 4 over 3,000 frames, 400 with 6, 217 with 8; Link's Awakening: frames
 * with 8 reads or more 26 -> 1 with 6), but 8 overflow the RAM and 6 would
 * leave about 1 KiB of stack: they need RAM taken from elsewhere first
 * (strips, dispatch cache), not measured on the device. */
#ifndef GBL_STREAM_SECTORS
#define GBL_STREAM_SECTORS 4
#endif
#ifndef GBL_PPU_ASM
#define GBL_PPU_ASM 1
#endif
#ifndef GBL_RENDER_LINE_ASM
#define GBL_RENDER_LINE_ASM 1
#endif
#ifndef GBL_PPU_RAMFUNC
#define GBL_PPU_RAMFUNC 1
#endif
#ifndef GBL_CORE_ASM
#define GBL_CORE_ASM 1
#endif
#ifndef GBL_TIMING_ASM
#define GBL_TIMING_ASM 1
#endif
#ifndef GBL_INTERP_ASM
#define GBL_INTERP_ASM 1
#endif
#ifndef GBL_UNPACK_ASM
#define GBL_UNPACK_ASM 1
#endif

/* frontend */
#ifndef META_LINE_SKIP
#define META_LINE_SKIP 1
#endif
#ifndef META_STATIC_SKIP
#define META_STATIC_SKIP 1
#endif
#ifndef META_MAX_SKIP
#define META_MAX_SKIP 3
#endif
#ifndef META_LCD_DMA
#define META_LCD_DMA 1
#endif
#ifndef META_AUDIO_DMA
#define META_AUDIO_DMA 1
#endif
#ifndef META_STRIP_LINES
#define META_STRIP_LINES 4
#endif
#ifndef META_SMOOTH_ASM
#define META_SMOOTH_ASM 1
#endif
#ifndef META_SMOOTH_SPECULATE
#define META_SMOOTH_SPECULATE 1
#endif
/* Builds with a ROM sector cache (SD or packed) or cartridge RAM on the
 * card have 2-2.5 KiB of RAM left (the stack) instead of 3-5: two strips in
 * flight instead of three, no merge modes (their 1.3 KiB of tables; TBLEND
 * falls back to TILE). */
#if (defined(GBL_ROM_STREAM) && GBL_ROM_STREAM) || GBL_ERAM_ON_SD
#define META_STREAM_RAM 1
#ifndef META_LCD_BUFFERS
#define META_LCD_BUFFERS 2
#endif
#ifndef META_MERGE_MODES
#define META_MERGE_MODES 0
#endif
#else
#define META_STREAM_RAM 0
#ifndef META_LCD_BUFFERS
#define META_LCD_BUFFERS 3
#endif
#ifndef META_MERGE_MODES
#define META_MERGE_MODES 1
#endif
#endif

#endif

#endif
