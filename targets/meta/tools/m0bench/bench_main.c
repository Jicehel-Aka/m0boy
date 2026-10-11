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

#ifdef BENCH_STATE_HASH
/* Independent of the selected display pipeline. */
uint32_t bench_state_hash = 2166136261u;

static void state_mix(const uint8_t *p, uint32_t n) {
    uint32_t h = bench_state_hash;
    while (n--) h = (h ^ *p++) * 16777619u;
    bench_state_hash = h;
}
#endif

#if defined(BENCH_POKITTO_DISPLAY)
/* The Pokitto's display pipeline (platform/pokitto: pokitto_display.c,
 * pokitto_lcd.c and its Thumb-1 pixel pushes, mode BENCH_POKITTO_DISPLAY:
 * 0 1:1, 1 200x176, 2 200x176 entier).  Its GPIO writes go to the region
 * m0_estimate.py maps at 0xA0000000, so every pixel store is counted. */
#include "meta_display.h"
void delay(uint32_t ms) { (void)ms; }
#define BENCH_OWN_DISPLAY 1
#elif defined(BENCH_META_DISPLAY)
#define BENCH_OWN_DISPLAY 1
/* The META's own display pipeline (platform/src/meta_display.c, mode
 * BENCH_META_DISPLAY: 0 SMOOTH, 1 TILE, ..., 6 TBLEND) with an LCD that takes every
 * strip at once: the rendering, blending and row bookkeeping of the
 * console, without the SPI transfer. */
#include "meta_display.h"
#include "meta_lcd.h"
static uint32_t lcd_seq;
#ifdef BENCH_PIXEL_HASH
uint32_t bench_pixel_hash = 2166136261u;
#endif
uint32_t meta_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
#ifdef BENCH_PIXEL_HASH
    const uint16_t *pixels = (const uint16_t *)buf;
    uint32_t h = (bench_pixel_hash ^ row0) * 16777619u;
    h = (h ^ rows) * 16777619u;
    for (uint32_t i = 0; i < (uint32_t)rows * 160u; i++)
        h = (h ^ pixels[i]) * 16777619u;
    bench_pixel_hash = h;
#endif
    (void)row0; (void)rows;
    bench_sink += *(const uint16_t *)buf;
    return ++lcd_seq;
}
uint32_t meta_lcd_done_seq(void) { return lcd_seq; }
#else
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

#endif /* BENCH_POKITTO_DISPLAY / BENCH_META_DISPLAY */

__attribute__((noinline)) void bench_done(void) {
    for (;;) __asm__ volatile("nop");
}

__attribute__((noinline)) void bench_frame(uint32_t frame) {
    bench_sink = frame;
}

#include "gbl_config.h"
#if GBL_ROM_STREAM
/* SD card stand-in: the whole ROM (host/*_romfull.c, in bench.ld's SDROM
 * region).  Only the copy is counted, not the SD transfer. */
extern const uint8_t gbl_rom_full[];
uint32_t bench_sd_sectors;

int gbl_platform_rom_read(uint32_t sector, uint8_t *dst) {
    if ((sector + 1u) * 512u > gbl_rom_size) return 0;
    const uint32_t *s = (const uint32_t *)(gbl_rom_full + sector * 512u);
    uint32_t *d = (uint32_t *)dst;
    for (int i = 0; i < 128; i++) d[i] = s[i];
    bench_sd_sectors++;
    return 1;
}
#endif
#if GBL_ERAM_SIZE > 0
/* Save file like the host's in-memory one: sectors never written read as
 * missing (the runtime then sees 0xFF), written ones keep their data.  The
 * data lives in the stand-in SD region (m0_estimate.py maps it writable):
 * cartridge RAM kept "on the card" (GBL_ERAM_ON_SD) can be 32 KiB.
 * (Dropping the writes made Zelda and Donkey Kong Land read garbage back
 * and crash.) */
#define BENCH_SAVE_AREA ((uint8_t *)0x10380000u)
static uint8_t save_valid[(GBL_ERAM_SIZE + 511) / 512];

int gbl_platform_save_read(uint32_t sector, uint8_t *dst) {
    if (sector * 512u >= GBL_ERAM_SIZE || !save_valid[sector]) return 0;
    const uint8_t *s = BENCH_SAVE_AREA + sector * 512u;
    for (int i = 0; i < 512; i++) dst[i] = s[i];
    return 1;
}

int gbl_platform_save_write(uint32_t sector, const uint8_t *src) {
    if (sector * 512u >= GBL_ERAM_SIZE) return 0;
    uint8_t *d = BENCH_SAVE_AREA + sector * 512u;
    for (int i = 0; i < 512; i++) d[i] = src[i];
    save_valid[sector] = 1;
    return 1;
}
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
#ifdef BENCH_FRAME_HASHES
        /* The running hash after every frame, in the stand-in SD region
         * (m0_estimate.py --dump 0x10300000:<4 * frames>): finds the first
         * frame where two builds part. */
        if (f < 0x4000u) ((uint32_t *)0x10300000u)[f] = bench_state_hash;
#endif
    }
#endif
#if defined(BENCH_SD_PER_FRAME) && GBL_ROM_STREAM && !GBL_ROM_PACKED
    /* SD sectors read so far, at every VBlank, in the stand-in SD region
     * (m0_estimate.py --dump 0x10300000:<4 * frames>): how card reads
     * cluster, for stream cache sizes (their time is not modelled). */
    if (f < 0x4000u) ((uint32_t *)0x10300000u)[f] = bench_sd_sectors;
#endif
#ifdef BENCH_STATIC_SKIP
    gbl_skip_render = (uint8_t)gbl_frame_unchanged_next(!gbl_skip_render);
#endif
#ifndef BENCH_OWN_DISPLAY
    strip_fill = 0;
#else
    meta_display_flush();
#endif
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
/* Every input of the ALU helpers (CP, ADD, SUB, ADC, SBC with both carries,
 * INC, DEC) against the C formulas: the assembly versions cannot run in the
 * host's exhaustive test. */
uint32_t bench_alu_errors;

static void alu_expect(uint32_t a, uint32_t zr, uint32_t cf, uint32_t hf, uint32_t nf) {
    if (gbr.a != a || gbr.zr != zr || gbr.cf != cf || gbr.hf != hf || gbr.nf != nf) bench_alu_errors++;
}

static void alu_check(void) {
    for (uint32_t c = 0; c < 2; c++)
        for (uint32_t a = 0; a < 256; a++)
            for (uint32_t v = 0; v < 256; v++) {
#define ALU_RUN(call) (gbr.a = (uint8_t)a, gbr.zr = gbr.hf = gbr.nf = 0x55, gbr.cf = (uint8_t)c, call)
                ALU_RUN(gbl_cp((uint8_t)v));
                alu_expect(a, (uint8_t)(a - v), a < v, (a & 15) < (v & 15), 1);
                ALU_RUN(gbl_add((uint8_t)v));
                alu_expect((uint8_t)(a + v), (uint8_t)(a + v), a + v > 255, (a & 15) + (v & 15) > 15, 0);
                ALU_RUN(gbl_sub((uint8_t)v));
                alu_expect((uint8_t)(a - v), (uint8_t)(a - v), a < v, (a & 15) < (v & 15), 1);
                ALU_RUN(gbl_adc((uint8_t)v));
                alu_expect((uint8_t)(a + v + c), (uint8_t)(a + v + c), a + v + c > 255, (a & 15) + (v & 15) + c > 15, 0);
                ALU_RUN(gbl_sbc((uint8_t)v));
                alu_expect((uint8_t)(a - v - c), (uint8_t)(a - v - c), a < v + c, (a & 15) < (v & 15) + c, 1);
            }
    for (uint32_t v = 0; v < 256; v++) {
        gbr.a = 0x33; gbr.cf = 0x44; gbr.zr = gbr.hf = gbr.nf = 0x55;
        if (gbl_inc((uint8_t)v) != (uint8_t)(v + 1)) bench_alu_errors++;
        alu_expect(0x33, (uint8_t)(v + 1), 0x44, (v & 15) == 15, 0);
        gbr.zr = gbr.hf = gbr.nf = 0x55;
        if (gbl_dec((uint8_t)v) != (uint8_t)(v - 1)) bench_alu_errors++;
        alu_expect(0x33, (uint8_t)(v - 1), 0x44, (v & 15) == 0, 1);
    }
}
#endif

int main(void) {
    char probe;
#ifdef BENCH_ALU_CHECK
    alu_check();
#endif
#ifdef BENCH_BIG_STACK
    /* m0_estimate.py maps 128 KiB of RAM and starts the stack at its top:
     * the stack guard never unwinds.  With the META's 3 KiB, where the
     * guard unwinds depends on every C frame size, and in fast builds the
     * unwound code takes interrupts at other instruction boundaries: two
     * correct builds can then end in different states.  Equivalence checks
     * of runtime changes use this mode. */
    (void)probe;
    gbl_stack_limit = 0x20008000u;
#else
    gbl_stack_limit = (uintptr_t)&probe - 3072u;
#endif
    gbl_reset();
#if defined(BENCH_POKITTO_DISPLAY)
    meta_display_set_mode(BENCH_POKITTO_DISPLAY);
#elif defined(BENCH_META_DISPLAY)
    meta_display_set_mode(BENCH_META_DISPLAY);
#endif
#ifdef BENCH_INTERP_ONLY
    /* Every instruction through the interpreter: compares interpreters
     * (GBL_INTERP_ASM=0 vs 1) on the whole game. */
    gbl_interp_only = 1;
#endif
    gbl_run();
    return 0;
}
