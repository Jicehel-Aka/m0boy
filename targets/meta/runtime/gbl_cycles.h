/* SM83 base T-cycles per opcode; conditional control flow lists the
 * not-taken cost.  One source for the interpreter (gbl_cpu.c, which also
 * exports it for gbl_interp_asm.S) and the recompiler's emitter
 * (recompiler/src/codegen/meta_emitter.cpp): the two must agree, a
 * differing table would desynchronise compiled code from the interpreter
 * on every cycle-dependent write. */
#ifndef GBL_CYCLES_H
#define GBL_CYCLES_H
#include <stdint.h>
const uint8_t gbl_cyc_main[256] = {
     4,12, 8, 8, 4, 4, 8, 4,20, 8, 8, 8, 4, 4, 8, 4,
     4,12, 8, 8, 4, 4, 8, 4,12, 8, 8, 8, 4, 4, 8, 4,
     8,12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
     8,12, 8, 8,12,12,12, 4, 8, 8, 8, 8, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     8,12,12,16,12,16, 8,16, 8,16,12, 4,12,24, 8,16,
     8,12,12, 4,12,16, 8,16, 8,16,12, 4,12, 4, 8,16,
    12,12, 8, 4, 4,16, 8,16,16, 4,16, 4, 4, 4, 8,16,
    12,12, 8, 4, 4,16, 8,16,12, 8,16, 4, 4, 4, 8,16,
};
#endif
