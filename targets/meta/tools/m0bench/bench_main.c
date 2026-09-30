/*
 * Bare-metal Cortex-M0+ benchmark image for gbl (no Arduino core).
 * Runs the generated game headless with the META's line mapping and stops
 * in bench_done() after BENCH_FRAMES frames.  m0_estimate.py executes it
 * under Unicorn and turns executed instructions into an M0+ cycle estimate.
 */
#include "gbl.h"
#include "bench_input.h"

#include <stdint.h>

#ifndef BENCH_FRAMES
#define BENCH_FRAMES 300
#endif

static uint16_t strip[2][160 * 8];
static int strip_cur, strip_fill;
static unsigned input_pos;
volatile uint32_t bench_sink;

/* Same row policy as the META SKIP mode: unchanged lines are not rendered. */
static uint32_t row_sig[144];
static uint8_t row_valid[144];
uint32_t bench_lines_skipped;

#ifdef BENCH_PIXEL_HASH
extern uint32_t bench_pixel_hash;
#endif

int gbl_platform_line_check(uint8_t ly, uint32_t sig) {
#ifdef BENCH_PIXEL_HASH
    /* The signatures too: C and assembly versions must agree line by line,
     * not only on the lines they cause to be drawn. */
    bench_pixel_hash = ((bench_pixel_hash ^ ly) * 16777619u ^ sig) * 16777619u;
#endif
    if (ly >= 144 || ly - ((ly * 57u) >> 9) * 9u == 8u) return 0;
#ifdef BENCH_LINE_SKIP
    if (row_valid[ly] && row_sig[ly] == sig) { bench_lines_skipped++; return 0; }
#endif
    row_sig[ly] = sig;
    row_valid[ly] = 1;
    return 1;
}

uint16_t *gbl_platform_line_begin(uint8_t ly) {
    if (ly >= 144 || ly - ((ly * 57u) >> 9) * 9u == 8u) return 0;
    return &strip[strip_cur][strip_fill * 160];
}

#ifdef BENCH_PIXEL_HASH
/* Hash of every rendered line (ly and pixels), read back by m0_estimate.py
 * to prove two builds (C / asm loops) render identically.  Costs cycles:
 * build it separately from timing runs. */
uint32_t bench_pixel_hash = 2166136261u;
#endif

#ifdef BENCH_STATE_HASH
/* Machine state at every VBlank entry (registers, flags, cycle counter,
 * WRAM, HRAM, IO), like the host's state_hash: proves that assembly
 * versions of timing code (events, PPU transitions) behave like the C ones.
 * Costs cycles: build it separately from timing runs. */
uint32_t bench_state_hash = 2166136261u;

static void state_mix(const uint8_t *p, uint32_t n) {
    uint32_t h = bench_state_hash;
    while (n--) h = (h ^ *p++) * 16777619u;
    bench_state_hash = h;
}
#endif

void gbl_platform_line_end(uint8_t ly) {
#ifdef BENCH_PIXEL_HASH
    {
        const uint16_t *p = &strip[strip_cur][strip_fill * 160];
        uint32_t h = (bench_pixel_hash ^ ly) * 16777619u;
        for (int x = 0; x < 160; x++) h = (h ^ p[x]) * 16777619u;
        bench_pixel_hash = h;
    }
#else
    (void)ly;
#endif
    if (++strip_fill == 8) {
        bench_sink += strip[strip_cur][0];
        strip_fill = 0;
        strip_cur ^= 1;
    }
}

__attribute__((noinline)) void bench_done(void) {
    for (;;) __asm__ volatile("nop");
}

__attribute__((noinline)) void bench_frame(uint32_t frame) {
    bench_sink = frame;
}

#include "gbl_config.h"
#if GBL_ERAM_SIZE > 0
/* No storage in the benchmark: cartridge RAM starts erased. */
int gbl_platform_save_read(uint32_t sector, uint8_t *dst) { (void)sector; (void)dst; return 0; }
int gbl_platform_save_write(uint32_t sector, const uint8_t *src) { (void)sector; (void)src; return 1; }
#endif

void gbl_platform_vblank(void) {
    uint32_t f = gbl_frame;
    bench_frame(f);
#ifdef BENCH_STATE_HASH
    {
        uint16_t regs[6] = {gbr.bc, gbr.de, gbr.hl, gbr.sp, gbr.pc,
                            (uint16_t)(gbr.a | gbl_getf() << 8)};
        uint32_t cyc = gbr.cyc;
        state_mix((const uint8_t *)regs, sizeof(regs));
        state_mix((const uint8_t *)&cyc, sizeof(cyc));
        state_mix(gbl_wram, sizeof(gbl_wram));
        state_mix(gbl_hram, sizeof(gbl_hram));
        state_mix(gbl_io, sizeof(gbl_io));
        /* PPU timing state up to lyc_line (gbl_timing_layout.h), window
         * line included: it is not visible in skipped frames. */
        extern const uint8_t gbl_ppu_state[];
        state_mix(gbl_ppu_state, 21);
    }
#endif
#ifdef BENCH_STATIC_SKIP
    gbl_skip_render = (uint8_t)gbl_frame_unchanged_next(!gbl_skip_render);
#endif
    strip_fill = 0;
    while (input_pos < sizeof(bench_inputs) / sizeof(bench_inputs[0]) &&
           bench_inputs[input_pos].frame <= f)
        gbl_set_buttons(bench_inputs[input_pos++].buttons);
    if (f >= BENCH_FRAMES) bench_done();
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

#ifdef BENCH_ALU_CHECK
/* Every (A, v) pair of the ALU helpers against the C formulas: the
 * assembly versions cannot run in the host's exhaustive test. */
uint32_t bench_alu_errors;

static void alu_check(void) {
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t v = 0; v < 256; v++) {
            gbr.a = (uint8_t)a;
            gbr.zr = gbr.cf = gbr.hf = gbr.nf = 0x55;
            gbl_cp((uint8_t)v);
            if (gbr.a != a || gbr.zr != (uint8_t)(a - v) || gbr.cf != (a < v) ||
                gbr.hf != ((a & 15) < (v & 15)) || gbr.nf != 1)
                bench_alu_errors++;
        }
}
#endif

int main(void) {
    char probe;
#ifdef BENCH_ALU_CHECK
    alu_check();
#endif
    gbl_stack_limit = (uintptr_t)&probe - 3072u;
    gbl_reset();
    gbl_run();
    return 0;
}
