#ifndef M0BOY_MD_FULL_H
#define M0BOY_MD_FULL_H
#include <stddef.h>
#include <stdint.h>
#include "z80.h"
#include "md_ym2612.h"

typedef struct {
    uint16_t tone[3], counter[4], lfsr;
    uint8_t volume[4], noise, latch, high[4];
} MdPsg;

/* PC reference machine for the 68000 recompiler. ROM instructions can be
 * dispatched from generated C; uncovered addresses use md_step_interp. */
typedef struct MdFull {
    uint32_t d[8], a[8], pc, usp;
    uint16_t sr, last_opcode;
    uint8_t ram[65536], vram[65536], vsram[80];
    uint16_t cram[64], vdp_reg[24];
    /* CRAM as colours, built once per write (no per-pixel or per-line
     * conversion): ARGB for the reference renderer, RGB565 for the panel. */
    uint32_t cram_argb[64];
    uint16_t cram_565[64];
    uint32_t vdp_addr, vdp_first;
    uint8_t vdp_code, vdp_pending, dma_fill;
    uint8_t pad, pad_th, pad_control, z80_bus, z80_reset;
    /* Cartridge SRAM ($200000-$20FFFF), enabled by a write to $A130F1;
     * bit 1 of that write protects it.  Volatile across a reset;
     * sram_dirty: written since the frontend last stored it. */
    uint8_t sram[65536], sram_enable, sram_protect, sram_dirty;
    uint8_t zram[8192];
    uint32_t z80_bank;
    z80_t sound_cpu;
    MdYm2612 ym;
    MdPsg psg;
    /* Audio time in master clocks (68000 x7, Z80 x15, one FM sample per
     * 1008); output frames wait in audio_out until the frontend takes them. */
    uint64_t audio_time;
    /* master-clock position (x AUDIO_RATE) of the next 44.1 kHz output
     * frame; runs between FM samples so no emitter keeps a division */
    uint64_t audio_next;
    uint32_t psg_rest;
    int32_t audio_last[2], audio_dc[2];
    /* one frame of 44.1 kHz stereo is 735-736 frames */
    int16_t audio_out[2 * 768];
    uint32_t audio_frames, audio_hash;
    /* Optional: every YM2612 data write, with the FM sample it precedes. */
    void (*ym_log)(void *user, uint64_t sample, unsigned bank, uint8_t reg, uint8_t value);
    void *ym_log_user;
    uint8_t stopped, fault, irq_pending;   /* irq_pending: vertical interrupt */
    uint8_t hint_pending, hint_counter;
    uint16_t line;
    /* The CPU runs in slices that end with a line; a generated region stops
     * once frame_cycles reaches slice_limit, which events that may let an
     * interrupt in (SR, VDP interrupt enables, STOP) set to zero. */
    uint32_t slice_limit;
    uint32_t cycles, frame_cycles, frames, instructions;
    uint32_t native_hits, fallback_hits;
    uint32_t fault_pc;
    uint8_t *coverage;
    size_t coverage_bytes;
    uint32_t *screen;   /* 320x224 ARGB, drawn line by line when set */
    /* per-line hook (the ESP32 frontend renders its RGB565 ring with it);
     * called at the end of every active line, before the ARGB row write */
    void (*line_hook)(void *user, const struct MdFull *m, unsigned line);
    void *hook_user;
} MdFull;

extern const uint8_t md_rom[];
extern const uint32_t md_rom_size;
void md_full_reset(MdFull *m);
/* Rebuilds the CRAM palette LUTs (after an external state load). */
void md_full_cram_sync(MdFull *m);
uint8_t md_full_read8(MdFull *m, uint32_t addr);
uint16_t md_full_read16(MdFull *m, uint32_t addr);
uint32_t md_full_read32(MdFull *m, uint32_t addr);
void md_full_write8(MdFull *m, uint32_t addr, uint8_t value);
void md_full_write16(MdFull *m, uint32_t addr, uint16_t value);
void md_full_write32(MdFull *m, uint32_t addr, uint32_t value);
void md_full_execute(MdFull *m, uint16_t op);
void md_full_step_interp(MdFull *m);
void md_full_step_native(MdFull *m);
void md_full_idle(MdFull *m, unsigned a, unsigned b, uint32_t test_pc, uint32_t branch_pc);
void md_full_frame(MdFull *m, int native);
void md_full_render(const MdFull *m, uint32_t *pixels, unsigned stride);
void md_full_render_line(const MdFull *m, unsigned y, uint32_t *row);
void md_full_render_line565(const MdFull *m, unsigned y, uint16_t *row);
/* Xtensa-vs-C check of the tile runs (built under MD_VIDEO_ASM only). */
int md_span_selftest(void);
void md_full_audio_reset(MdFull *m);
void md_full_sound_write(MdFull *m, uint32_t addr, uint8_t value);
uint8_t md_full_sound_read(MdFull *m, uint32_t addr);
void md_full_sound_control(MdFull *m, unsigned reset_line, uint8_t value);
void md_full_sound_frame(MdFull *m);
/* Moves up to max stereo frames (44.1 kHz, L then R) into samples. */
unsigned md_full_audio_read(MdFull *m, int16_t *samples, unsigned max);
uint32_t md_full_hash(const MdFull *m);
#endif
