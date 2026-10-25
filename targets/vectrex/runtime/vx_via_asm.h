/* via_tick in Thumb-1 (via_asm.S) on the Cortex-M0+, via.c's C elsewhere;
 * -DVX_VIA_ASM=0 keeps the C on the console too (the reference). */
#pragma once
#ifndef VX_VIA_ASM
#if defined(__thumb__) && !defined(__thumb2__)
#define VX_VIA_ASM 1
#else
#define VX_VIA_ASM 0
#endif
#endif
