#if defined(__ARM_ARCH_6M__)
/* SG-1000 / Game Gear / Master System projects: the decoder runs where the
 * project's other loops do (SG_CODE_IN_SRAM, sg_config.h: flash on the
 * META, SRAM on the Pokitto).  From SRAM on the META with the CPU at 70 MHz
 * it decoded some sectors wrong (device, 2026-10-19: Gunstar Heroes left
 * the host's run by frame 600, differently on every run, and showed black
 * or broken screens; the same firmware with the decoder in flash matched
 * the host at frame 900 twice).  The Game Boy sets M0_ROM_RAMFUNC itself
 * (gbl_core_asm.S). */
#if !defined(M0_ROM_RAMFUNC) && defined(__has_include)
#if __has_include("sg_config.h")
#include "sg_config.h"
#define M0_ROM_RAMFUNC SG_CODE_IN_SRAM
#endif
#endif
#include "m0_rom_lzss.inc"
#endif
