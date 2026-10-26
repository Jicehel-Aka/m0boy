#ifndef M0BOY_MD_FULL_H
#define M0BOY_MD_FULL_H
#include <stddef.h>
#include <stdint.h>
#include "z80.h"

/* YM2612 operator; op index 0..3 is S1..S4 (registers +0, +8, +4, +12). */
typedef struct {
    uint32_t phase;
    uint16_t att;
    uint8_t state, key, ssg_invert;
    int16_t out, prev;
} MdFmOperator;
typedef struct {
    uint8_t addr[2], reg[2][256], fnum_latch, special_latch, status;
    uint16_t fnum[6], special_fnum[3];
    uint8_t block[6], special_block[3];
    MdFmOperator op[6][4];
    uint16_t eg_cnt, timer_a, timer_b;
    uint8_t eg_timer, timer_b_sub, lfo_step, lfo_sub;
    uint8_t reg_key[6], csm_key;   /* key bits from register 0x28 and from CSM */
    uint64_t busy_until;           /* master clock */
} MdYm2612;
typedef struct {
    uint16_t tone[3], counter[4], lfsr;
    uint8_t volume[4], noise, latch, high[4];
} MdPsg;

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
    MdYm2612 ym;
    MdPsg psg;
    /* Audio time in master clocks (68000 x7, Z80 x15, one FM sample per
     * 1008); output frames wait in audio_out until the frontend takes them. */
    uint64_t audio_time;
    uint32_t psg_rest;
    int32_t audio_last[2], audio_dc[2];
    int16_t audio_out[2 * 4096];
    uint32_t audio_frames, audio_hash;
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
void md_full_sound_control(MdFull *m, unsigned reset_line, uint8_t value);
void md_full_sound_frame(MdFull *m);
/* Moves up to max stereo frames (44.1 kHz, L then R) into samples. */
unsigned md_full_audio_read(MdFull *m, int16_t *samples, unsigned max);
uint32_t md_full_hash(const MdFull *m);
#endif
