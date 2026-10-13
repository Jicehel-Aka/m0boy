#ifndef M0_SG1000_H
#define M0_SG1000_H
/* SG-1000 runtime shared by recompiled code and the Z80 interpreter.
 *
 * Time is a T-state counter (machine.cyc) with two device events per frame:
 * VBlank at line 192 and the end of the frame at line 262.  Video lines are
 * rendered lazily, when the program touches the VDP or at VBlank, from the
 * VDP state of that moment: nothing is checked per line in generated code,
 * and a line still sees every register/VRAM write made before it ended.
 * The PSG is caught up the same way, before each write and at frame end.
 *
 * Execution: sg1000_run() looks pc up in the generated entry table (regions
 * of native code) and falls back to the interpreter for anything else.  Both
 * engines synchronise devices and take interrupts through sg_event, so a
 * precise build (sync before every instruction) reproduces the interpreter. */
#include "z80.h"
#include <stddef.h>
/* Generated projects configure the runtime (ROM size, cartridge RAM). */
#if defined(__has_include)
#if __has_include("sg_config.h")
#include "sg_config.h"
#endif
#endif
/* Cartridge RAM leaves no room for the 4 KiB colour-pair table. */
#ifndef SG_LOW_RAM
#define SG_LOW_RAM 0
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define SG_WIDTH 256
#define SG_HEIGHT 192
#define SG_LINES 262
#define SG_LINE_CYCLES 228
#define SG_FRAME_CYCLES (SG_LINES * SG_LINE_CYCLES)
#define SG_VBLANK_CYCLES (SG_HEIGHT * SG_LINE_CYCLES)
/* Device line: 5 of every 8 source pixels (256 -> 160). */
#define SG_OUT_WIDTH 160
#define SG_CART_RAM_SIZE 8192

typedef void (*sg_fn)(void);

typedef struct {
    z80_t cpu;               /* registers; cpu.user points back here */
    uint32_t cyc;            /* T-states, wrapping */
    uint32_t next_evt;       /* sync when cyc reaches it (forced to cyc for IRQs) */
    uint32_t frame_start;    /* cycle of line 0 of the current frame */
    uint32_t render_at;      /* cycle at which line render_line ends */
    uint32_t frames;
    uint16_t render_line;
    uint8_t vblank_done, stop;
    const uint8_t *rom;
    uint32_t rom_size, rom_mask; /* mask: power-of-two size - 1, else 0 */
    uint8_t *cart_ram;       /* 8 KiB at 8000-BFFF (mirrored), or NULL */
    uint8_t ram[1024], vram[16384], vdp[8];
    uint16_t vdp_address, sat_base;
    uint8_t vdp_latch, vdp_pending, vdp_buffer, vdp_status;
    uint8_t sprites_dirty;   /* sprite Y, terminator, size or table changed */
    uint8_t buttons[2], pause;
    /* SN76489 */
    uint16_t psg_tone[3];
    uint8_t psg_volume[4], psg_noise, psg_latch;
    uint32_t audio_rate, audio_phase, audio_tick_phase, audio_lfsr, audio_cyc;
    uint32_t audio_div, audio_tick_int, audio_tick_frac; /* PSG ticks per sample */
    int32_t audio_count[4];  /* PSG ticks to the next edge (<= 0: edge due) */
    uint16_t audio_head, audio_tail;
    uint8_t audio_output[4], audio_buffer[512];
    /* Output.  row: SG_OUT_WIDTH RGB565 pixels from palette; draw_line
     * selects the source lines that are displayed (others only update
     * sprite status).  ref_row: full 256-pixel colour indices, host only. */
    uint16_t palette[16];
    uint16_t *row_out;       /* where the next displayed line is rendered */
    void *platform;
    int (*draw_line)(void *, unsigned y);
    void (*row)(void *, unsigned y, uint16_t *pixels); /* pixels == row_out; may move row_out */
    void (*ref_row)(void *, unsigned y, const uint8_t *pixels);
    void (*frame)(void *);
    /* Execution */
    sg_fn (*lookup)(uint16_t pc); /* generated entry table, or NULL */
    uint32_t interpreted, compiled; /* statistics: instructions / region entries */
    uint32_t *coverage;           /* host: per-pc hit counters (65536), or NULL */
    uint8_t cart_ram_used;        /* a write reached 8000-BFFF */
} sg1000_t;

extern sg1000_t machine;
extern const uint32_t sg1000_palette[16];
/* Below this stack pointer regions and nested dispatch loops unwind. */
extern uintptr_t sg_stack_limit;

void sg1000_init(const uint8_t *rom, size_t size, uint8_t *cart_ram);
void sg1000_reset(void);
void sg1000_buttons(unsigned player, uint8_t pressed);
void sg1000_pause(int pressed);
/* Run until machine.stop is set (by a frame callback); resumable. */
void sg1000_run(void);

/* Bus, at machine.cyc (callers flush pending cycles first). */
static inline uint8_t sg1000_read(uint16_t a) {
    if (a >= 0xc000) return machine.ram[a & 1023];
    if (a >= 0x8000 && machine.cart_ram) return machine.cart_ram[a & (SG_CART_RAM_SIZE - 1)];
    if (machine.rom_mask) return machine.rom[a & machine.rom_mask];
    return a < machine.rom_size ? machine.rom[a] : 0xff;
}
void sg1000_write_cart(uint16_t a, uint8_t v);
static inline void sg1000_write(uint16_t a, uint8_t v) {
    if (a >= 0xc000) machine.ram[a & 1023] = v;
    else if (a >= 0x8000) sg1000_write_cart(a, v);
}
uint8_t sg1000_in(uint8_t port);
void sg1000_out(uint8_t port, uint8_t v);

/* Devices and events */
void sg_sync(void);                  /* process due device events */
int sg_event(uint16_t resume_pc);    /* sync + interrupt; nonzero: pc moved */
void sg_halt(void);                  /* HALT: skip time to the next interrupt */
void sg_irq_update(void);
void sg_exec(void);                  /* dispatch until the guest returns */
void sg_interp_block(void);
int sg_step(uint16_t after);         /* one interpreted instruction; 1: continue at after */

/* Video */
void sg1000_vdp_catchup(void);       /* render the lines that ended before cyc */
void sg1000_render_line(unsigned y, uint8_t *pixels); /* reference: 256 indices or status only */
void sg1000_render_out(unsigned y, uint16_t *out);    /* device: 160 RGB565 or status only */

/* Audio: unsigned 8-bit samples at rate, following emulated time */
void sg1000_audio_rate(unsigned rate);
void sg1000_audio_catchup(void);
void sg1000_audio_render(uint8_t *buffer, unsigned n);
static inline unsigned sg1000_audio_available(void) {
    return (unsigned)(machine.audio_head - machine.audio_tail) & 511u;
}

#ifdef __cplusplus
}
#endif
#endif
