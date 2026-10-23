/* The entry points of the simulated MSX BIOS (targets/msx/runtime/msx.c): name,
 * address in the jump table, routine number (the port of the stub's OUT) and
 * whether a CALL to it may run the routine directly from recompiled code.
 * Not inlined: the routines that move the slots (the code after them may sit
 * in another window) and the one that reads the stack. */
#ifndef M0_MSX_BIOS_H
#define M0_MSX_BIOS_H
#define M0_MSX_BIOS(X) \
    X(RDSLT, 0x0c, 1, 1) X(WRSLT, 0x14, 2, 1) X(ENASLT, 0x24, 3, 0) X(CALLF, 0x30, 4, 0) X(DCOMPR, 0x20, 5, 1) \
    X(DISSCR, 0x41, 6, 1) X(ENASCR, 0x44, 7, 1) X(WRTVDP, 0x47, 8, 1) X(RDVRM, 0x4a, 9, 1) X(WRTVRM, 0x4d, 10, 1) \
    X(SETRD, 0x50, 11, 1) X(SETWRT, 0x53, 12, 1) X(FILVRM, 0x56, 13, 1) X(LDIRMV, 0x59, 14, 1) X(LDIRVM, 0x5c, 15, 1) \
    X(CHGMOD, 0x5f, 16, 1) X(CHGCLR, 0x62, 17, 1) X(CLRSPR, 0x69, 18, 1) X(INITXT, 0x6c, 19, 1) X(INIT32, 0x6f, 20, 1) \
    X(INIGRP, 0x72, 21, 1) X(INIMLT, 0x75, 22, 1) X(SETTXT, 0x78, 23, 1) X(SETT32, 0x7b, 24, 1) X(SETGRP, 0x7e, 25, 1) \
    X(SETMLT, 0x81, 26, 1) X(GICINI, 0x90, 27, 1) X(WRTPSG, 0x93, 28, 1) X(RDPSG, 0x96, 29, 1) X(CHSNS, 0x9c, 37, 1) \
    X(CHGET, 0x9f, 38, 1) X(CHPUT, 0xa2, 40, 1) X(CLS, 0xc3, 30, 1) X(POSIT, 0xc6, 41, 1) X(GTSTCK, 0xd5, 31, 1) \
    X(GTTRIG, 0xd8, 32, 1) X(GTPAD, 0xdb, 39, 1) X(GTPDL, 0xde, 39, 1) X(RSLREG, 0x138, 33, 1) X(WSLREG, 0x13b, 34, 0) \
    X(RDVDP, 0x13e, 35, 1) X(SNSMAT, 0x141, 36, 1)
#define M0_MSX_BIOS_TICK 42 /* the interrupt handler's JIFFY count (no entry point) */
#endif
