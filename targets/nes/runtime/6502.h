/* The 2A03's 6502 core: a plain interpreter with cycle counts, the NES
 * running it without decimal mode.  The bus (RAM, PPU, APU, mapper) is the
 * host's through NES_RD/NES_WR, defined before this header; interrupts
 * arrive as nmi_line/irq_line the host raises before a step. */
#ifndef NES_6502_H
#define NES_6502_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint8_t nmi_edge;   /* the host sets it; the core takes the NMI once */
    uint8_t irq_line;   /* level: sampled before each instruction */
    uint32_t stall_pending; /* OAM DMA: master cycles the core must burn */
    uint32_t cycles;    /* master cycles consumed since reset */
    /* the bus's fast paths: RAM at $0000-$1FFF and the cartridge at
     * $8000+, which is where nearly every access lands (nes.c fills them) */
    uint8_t *ram;
    const uint8_t *prg;
    uint32_t prg_mask;
} nes6502_t;

/* p: N V - B D I Z C */
#define NES_P_C 0x01
#define NES_P_Z 0x02
#define NES_P_I 0x04
#define NES_P_D 0x08
#define NES_P_B 0x10
#define NES_P_U 0x20
#define NES_P_V 0x40
#define NES_P_N 0x80

void nes6502_reset(nes6502_t *cpu);
/* Runs until `cycles` master cycles are consumed (dma included by the host
 * through nes6502_stall); returns when an instruction boundary is reached
 * and the budget is spent. */
void nes6502_run(nes6502_t *cpu, uint32_t cycles);
void nes6502_stall(nes6502_t *cpu, uint32_t cycles); /* OAM DMA, pauses */

#endif


#ifdef __cplusplus
}
#endif