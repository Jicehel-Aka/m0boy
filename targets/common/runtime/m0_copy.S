#if defined(__ARM_ARCH_6M__)
/* SG-1000 / Game Gear / Master System projects: the copies run where the
 * project's other loops do (SG_CODE_IN_SRAM, sg_config.h), like the LZSS
 * decoder (m0_rom_lzss.S).  The Game Boy includes m0_copy.inc from
 * gbl_core_asm.S. */
#if !defined(M0_COPY_RAMFUNC) && defined(__has_include)
#if __has_include("sg_config.h")
#include "sg_config.h"
#define M0_COPY_RAMFUNC SG_CODE_IN_SRAM
#endif
#endif
#include "m0_copy.inc"
#endif
