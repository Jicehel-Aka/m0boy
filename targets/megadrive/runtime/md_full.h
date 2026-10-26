#ifndef M0BOY_MD_FULL_H
#define M0BOY_MD_FULL_H
#include <stddef.h>
#include <stdint.h>
#include "z80.h"

/* PC reference machine for the 68000 recompiler. ROM instructions can be
 * dispatched from generated C; uncovered addresses use md_step_interp. */
typedef struct {
    uint32_t d[8], a[8], pc, usp;
    uint16_t sr, last_opcode;
    uint8_t ram[65536], vram[65536], vsram[80];
    uint16_t cram[64], vdp_reg[24];
    uint32_t vdp_addr, vdp_first;
    uint8_t vdp_code, vdp_pending, dma_fill;
    uint8_t pad, pad_th, pad_control, z80_bus, z80_reset;
    uint8_t zram[8192];
    uint32_t z80_bank;
    z80_t sound_cpu;
    uint8_t ym_addr[2], ym_reg[2][256], ym_key[6];
    double ym_phase[6][4], ym_env[6][4];
    uint16_t psg_tone[3], psg_noise_lfsr;
    uint8_t psg_volume[4], psg_noise, psg_latch;
    double psg_phase[4];
    uint32_t audio_hash;
    uint8_t stopped, fault, irq_pending;
    uint32_t cycles, frame_cycles, frames, instructions;
    uint32_t native_hits, fallback_hits;
    uint32_t fault_pc;
    uint8_t *coverage;
    size_t coverage_bytes;
} MdFull;

extern const uint8_t md_rom[];
extern const uint32_t md_rom_size;
void md_full_reset(MdFull *m);
uint8_t md_full_read8(MdFull *m, uint32_t addr);
uint16_t md_full_read16(MdFull *m, uint32_t addr);
uint32_t md_full_read32(MdFull *m, uint32_t addr);
void md_full_write8(MdFull *m, uint32_t addr, uint8_t value);
void md_full_write16(MdFull *m, uint32_t addr, uint16_t value);
void md_full_write32(MdFull *m, uint32_t addr, uint32_t value);
void md_full_execute(MdFull *m, uint16_t op);
void md_full_step_interp(MdFull *m);
void md_full_step_native(MdFull *m);
void md_full_frame(MdFull *m, int native);
void md_full_render(const MdFull *m, uint32_t *pixels, unsigned stride);
void md_full_audio_reset(MdFull *m);
void md_full_sound_write(MdFull *m, uint32_t addr, uint8_t value);
uint8_t md_full_sound_read(MdFull *m, uint32_t addr);
void md_full_sound_frame(MdFull *m);
void md_full_audio_render(MdFull *m, int16_t *samples, unsigned count);
uint32_t md_full_hash(const MdFull *m);
#endif
