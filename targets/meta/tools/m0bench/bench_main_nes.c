/*
 * Bare-metal Cortex-M0+ benchmark image for the NES runtime (no Arduino
 * core).  Runs the generated cartridge headless with the META frontend's
 * row policy and stops in bench_done() after BENCH_FRAMES frames.
 * m0_estimate.py executes it under Unicorn and turns executed instructions
 * into an M0+ cycle estimate.  The SPI transfer of the META's LCD is not
 * modelled (only the picture's sampling is); the emulation's cost is what
 * decides whether a frame fits the 16.7 ms budget.
 */
#include "nes.h"
#include "nes_config.h"
#include "m0_scale.h"
#include "bench_input.h"

#include <stdint.h>

#ifndef BENCH_FRAMES
#define BENCH_FRAMES 300
#endif

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

/* 240 -> 128: the first line of each group is kept (nes_main.cpp) */
static unsigned next_line, shown_rows;
static int want_row(void *host, unsigned y) {
    (void)host;
    if (y == 0) { next_line = 0; shown_rows = 0; }
    if (y != next_line) return 0;
    next_line += (++shown_rows & 7u) ? 2u : 1u;
    return 1;
}

#ifdef BENCH_PIXEL_HASH
uint32_t bench_pixel_hash = 2166136261u;
#endif

/* the frontend's 5-of-8 column sampling, 0 2 3 5 6 (nes_main.cpp) */
static void row(void *host, unsigned y, const uint16_t *pixels) {
    (void)host;
    (void)y;
    const uint16_t *src = pixels + machine.ppu.x; /* the stream from fine x */
    unsigned x = 0, s = 0;
    for (; x < 160; x += 5, s += 8) {
        line_buffer[x + 0] = src[s + 0];
        line_buffer[x + 1] = src[s + 2];
        line_buffer[x + 2] = src[s + 3];
        line_buffer[x + 3] = src[s + 5];
        line_buffer[x + 4] = src[s + 6];
    }
    bench_sink += line_buffer[0];
#ifdef BENCH_PIXEL_HASH
    {
        uint32_t h = (bench_pixel_hash ^ y) * 16777619u;
        for (unsigned i = 0; i < 160; i++) h = (h ^ line_buffer[i]) * 16777619u;
        bench_pixel_hash = h;
    }
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
    machine.row = row;
    machine.want_row = want_row;
    for (uint32_t f = 0; f <= BENCH_FRAMES; f++) {
        bench_frame(f);
        machine.input = 0;
        while (input_pos < sizeof(bench_inputs) / sizeof(bench_inputs[0]) &&
               bench_inputs[input_pos].frame <= f)
            machine.input = nes_buttons(bench_inputs[input_pos++].buttons);
        if (f == BENCH_FRAMES) break;
        nes_frame();
    }
    bench_done();
    return 0;
}
