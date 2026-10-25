/*
 * Bare-metal Cortex-M0+ benchmark image for the NES runtime (no Arduino
 * core).  Runs the generated cartridge headless, the PPU building the
 * META's panel rows, and stops in bench_done() after BENCH_FRAMES frames.
 * Every frame is shown unless BENCH_NES_SKIP=n leaves out one in n (the
 * frontend's pacing does that when frames run late).  Each frame's sound is
 * rendered as the META's driver asks for it (22049 Hz: 366 or 367 samples),
 * unless BENCH_NES_AUDIO=0.
 * m0_estimate.py executes it under Unicorn and turns executed instructions
 * into an M0+ cycle estimate.  The SPI transfer of the META's LCD is not
 * modelled (the picture's scaling is); the emulation's cost is what
 * decides whether a frame fits the 16.7 ms budget.
 */
#include "nes.h"
#include "nes_config.h"
#include "bench_input.h"

#include <stdint.h>

#ifndef BENCH_FRAMES
#define BENCH_FRAMES 300
#endif
#ifndef BENCH_NES_AUDIO
#define BENCH_NES_AUDIO 1
#endif
#define BENCH_AUDIO_RATE (48000000u / 2177u) /* meta_audio.cpp */

extern const uint8_t nes_prg[];
extern const uint8_t nes_chr[];

static uint16_t line_buffer[160];
static unsigned input_pos;
volatile uint32_t bench_sink;

/* The NES runtime's buttons (nes.h) from the estimator's gbl-style script:
 * a=1 b=2 select=4 start=8 right=16 left=32 up=64 down=128. */
static uint8_t nes_buttons(uint8_t gbl) {
    uint8_t v = 0;
    if (gbl & 0x01) v |= NES_A;
    if (gbl & 0x02) v |= NES_B;
    if (gbl & 0x04) v |= NES_SELECT;
    if (gbl & 0x08) v |= NES_START;
    if (gbl & 0x10) v |= NES_RIGHT;
    if (gbl & 0x20) v |= NES_LEFT;
    if (gbl & 0x40) v |= NES_UP;
    if (gbl & 0x80) v |= NES_DOWN;
    return v;
}

#ifdef BENCH_PIXEL_HASH
uint32_t bench_pixel_hash = 2166136261u;
#endif

/* the PPU's panel rows (ppu.h), as nes_main.cpp takes them; no SPI */
static void panel_row(void *host, unsigned y) {
    (void)host;
    bench_sink += line_buffer[0];
#ifdef BENCH_PIXEL_HASH
    {
        uint32_t h = (bench_pixel_hash ^ y) * 16777619u;
        for (unsigned i = 0; i < 160; i++) h = (h ^ line_buffer[i]) * 16777619u;
        bench_pixel_hash = h;
    }
#else
    (void)y;
#endif
}

__attribute__((noinline)) void bench_done(void) {
    for (;;) __asm__ volatile("nop");
}

__attribute__((noinline)) void bench_frame(uint32_t frame) {
    bench_sink = frame;
}

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);

void Reset_Handler(void) {
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0;
    main();
    bench_done();
}

__attribute__((section(".isr_vector"), used))
static const void *const vectors[2] = {&_estack, (void *)Reset_Handler};

int main(void) {
    nes_init(nes_prg, NES_PRG_SIZE, NES_CHR_SIZE ? nes_chr : 0, NES_CHR_SIZE,
             NES_VERTICAL_MIRROR);
    machine.ppu.panel = panel_row;
    machine.ppu.panel_out = line_buffer;
#ifdef BENCH_NES_PLAN /* -DBENCH_NES_PLAN=NES_PLAN_176: the Pokitto's rows */
    machine.ppu.panel_plan = BENCH_NES_PLAN;
#endif
    nes_audio_set_rate(BENCH_AUDIO_RATE);
    for (uint32_t f = 0; f <= BENCH_FRAMES; f++) {
        bench_frame(f);
        machine.input = 0;
        while (input_pos < sizeof(bench_inputs) / sizeof(bench_inputs[0]) &&
               bench_inputs[input_pos].frame <= f)
            machine.input = nes_buttons(bench_inputs[input_pos++].buttons);
        if (f == BENCH_FRAMES) break;
#ifdef BENCH_NES_SKIP
        machine.ppu.skip = f % BENCH_NES_SKIP == BENCH_NES_SKIP - 1;
#endif
        nes_frame();
#if BENCH_NES_AUDIO
        {
            /* m0_audio_frame_samples: 29781 CPU cycles a frame */
            static uint8_t samples[400];
            static uint32_t frac;
            unsigned count = BENCH_AUDIO_RATE * 29781u / 1789773u;
            frac += BENCH_AUDIO_RATE * 29781u % 1789773u;
            if (frac >= 1789773u) { frac -= 1789773u; count++; }
            nes_audio_render(samples, count);
            bench_sink += samples[0];
        }
#endif
    }
    bench_done();
    return 0;
}
