/* The 5A22 (WDC 65C816): the 6502's registers widened (C as B:A, X and Y
 * with hidden high bytes), the direct page, the two bank registers and the
 * emulation flag.  The interpreter runs the documented opcode set — every
 * byte is a defined opcode, $42 WDM is a two-byte NOP.  The bus lands on
 * snes_bus_read/snes_bus_write: the LoROM or HiROM windows of the
 * cartridge, 128 KiB of WRAM with its mirrors, and a phase-1 I/O page (the
 * NMI register, the WRAM port, everything else open bus).  The zero page
 * is D's window in bank $00 and the stack is bank $00, S — never banked.
 *
 * Phase 1 facts, to be cross-checked like the PCE's mednafen pass:
 * the cycle counts follow the WDC manual's matrix (the D-page, width and
 * page-cross penalties included), BRK/COP push PC+2 in both modes, the
 * WAI/STP sleep spends the rest of the slice, and the frame is 59562 CPU
 * cycles (the slow-ROM divide is not modelled). */
#ifndef SNES_CPU_H
#define SNES_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SNES_P_C 0x01
#define SNES_P_Z 0x02
#define SNES_P_I 0x04
#define SNES_P_D 0x08
#define SNES_P_X 0x10   /* 1: index registers 8-bit (pushed as B) */
#define SNES_P_M 0x20   /* 1: accumulator 8-bit */
#define SNES_P_V 0x40
#define SNES_P_N 0x80

/* the frame: 262 lines of 1364 master clocks, the CPU at master/6 (the
 * phase-1 runtime runs the same rate whatever MEMSEL says) */
#define SNES_FRAME_CYCLES 59562u

typedef struct {
    uint16_t a;      /* C: B in the high byte, A in the low */
    uint16_t x, y;   /* the high bytes stay zero while the X flag is set */
    uint16_t s;
    uint16_t d;      /* the direct page */
    uint8_t k;       /* the program bank */
    uint8_t dbr;     /* the data bank */
    uint8_t p;       /* N V M X D I Z C; B exists only on the stack */
    uint8_t e;       /* the emulation flag (not part of P) */
    uint16_t pc;
    uint32_t cycles;
    uint8_t mdr;     /* the open bus's last value */
    /* the bus */
    uint8_t *wram;   /* 128 KiB at $7E0000 */
    const uint8_t *rom;
    uint32_t rom_bytes;
    int hirom;
    /* the phase-1 I/O state */
    int nmi_enable;
    int vblank_flag;
    int nmi_pending; /* the frame's edge, latched */
    int irq_line;    /* no source in phase 1: never set */
    int halted;      /* STP: the CPU stopped until the next reset */
    uint32_t wram_addr; /* the $2181-$2183 port */
} snes_cpu_t;

/* The machine: the CPU, 128 KiB of WRAM and the cartridge.  One machine
 * per firmware (as the other runtimes' own machine).  Phase 1 has no
 * picture: the PPU's VRAM, the APU's A-RAM and the DMA channels join
 * when the picture does. */
typedef struct {
    snes_cpu_t cpu;
    uint8_t wram[131072];
    uint32_t frames;
} snes_t;

/* rom must live as long as the machine */
extern snes_t machine;
void snes_init(const uint8_t *rom, uint32_t rom_bytes, int hirom);
void snes_reset(void);
/* run until the cycle count reaches `until` (past the last instruction
 * that spends it, as the budget goes negative) */
void snes_run(snes_cpu_t *cpu, uint32_t until);
/* one frame of emulation (the harness calls it once per frame) */
void snes_frame(void);

/* the bus: everything but the ROM/WRAM windows */
uint8_t snes_bus_read(uint32_t at);
void snes_bus_write(uint32_t at, uint8_t v);
/* the interrupt sequence on the machine's fields (the dispatcher's;
 * the B bit clear on the stack, D cleared) */
void snes_take_interrupt(snes_cpu_t *c, uint16_t vector);

/* the entry key the dispatcher and the recompiler share: K, DBR, the
 * direct page and the M/X/E flags packed, K lowest */
uint64_t snes_ctx_key(const snes_cpu_t *cpu);

#ifdef __cplusplus
}
#endif
#endif
