/* Game Gear and Master System frontend for the Gamebuino META and the
 * Pokitto: recompiled code (sg_gen.c) on the Game Gear runtime, the
 * consoles' LCD, audio, clock and button drivers.
 *
 * Game Gear, META (160x128): the 144 lines become 128 rows, two lines
 * averaged in every nine.  Pokitto (220x176): 160x144 enlarged to 200x176,
 * two rows in every nine lines drawn twice.
 * Master System (rows already filtered to 160 pixels): one line in 8/5
 * drawn, 120 rows on the META, the same rows enlarged to 200x150 on the
 * Pokitto; MENU / C is PAUSE. */
#include "gg.h"
#include "gg_display.h"
#include "sg_config.h"
#include "meta_target.h"
#include "meta_clock.h"
#include "meta_audio.h"
#include "meta_lcd.h"
#if META_TARGET_POKITTO
#include "pokitto_hw.h"
#else
#include "meta_hw.h"
#endif
extern "C" {
extern const uint8_t sg_rom[];
extern const size_t sg_rom_size;
sg_fn sg_gen_lookup(uint16_t pc);
extern char end; /* end of .bss: the C stack grows down to it */
extern char __StackTop;
#if META_TARGET_POKITTO
#if !GG_STREAM && !GG_CART_ON_CARD /* else pokitto_sd.c */
void pokitto_sd_clock_changed(uint32_t) {}
#endif
void pokitto_lcd_row_zoom(unsigned y, const uint16_t *px);
#endif
#if GG_STREAM || GG_CART_ON_CARD
int gg_stream_open(const uint8_t *rom, uint32_t rom_size);
#endif
}
#if SG_CART_RAM && !GG_CART_ON_CARD
static uint8_t cart_ram[GG_CART_RAM_SIZE];
#endif
/* Rows: GG_WIDTH pixels with GG_ROW_PAD writable pixels on each side.  Two
 * buffers: the META sends one by DMA while the other is drawn. */
static uint16_t line_buffers[2][GG_ROW_STRIDE] __attribute__((aligned(4)));
#if !META_TARGET_POKITTO
static uint32_t line_sequences[2];
static unsigned line_slot;
#endif
static uint32_t deadline;
#ifndef GG_MAX_SKIP
#define GG_MAX_SKIP 2
#endif
static int skip_video;
static unsigned skipped_run;
#ifdef GG_SELFTEST
extern "C" { volatile unsigned gg_selftest_result; }
#endif
#ifdef GG_BENCH
/* Measurement build, run in gm0 (see docs): no pacing and no skipped frame
 * (with GG_BENCH_PACED: the real pacing and frame skipping; with
 * GG_BENCH_NOVIDEO: no line drawn; with GG_BENCH_NOLCD on the META: lines
 * drawn but not sent), buttons from a script or from the host's
 * random player (gg_host --random), the time of every frame between two frame
 * numbers and the card sectors read.  The project's gg_bench.h, written by
 * the person measuring, defines GG_BENCH_FROM, GG_BENCH_TO and either
 * GG_BENCH_INPUT ("frame:mask:length,...", masks of GG_* keys) or
 * GG_BENCH_SEED (gg_host --random --start-frame 30).  Read back from
 * gg_bench: [0] done, [1] microseconds, [2..8] frames that read 0, 1, 2, 3,
 * 4-5, 6-8, 9+ sectors, [9..11] frames over 16.7, 25 and 33.4 ms, [12] the
 * longest frame, [13] frames not drawn; gg_bench_window: microseconds of 16
 * equal parts of the range. */
#include "gg_bench.h"
extern "C" { volatile uint32_t gg_bench[16], gg_bench_window[16]; }
static uint8_t bench_keys(uint32_t frame) {
    uint8_t keys = 0;
#ifdef GG_BENCH_SEED
    static uint32_t state = (uint32_t)GG_BENCH_SEED | 1u, held;
    static int primed;
    if (!primed) { primed = 1; state = state * 1103515245u + 12345u; held = (state >> 16) & 63; if (((state >> 24) & 15) == 0 && !GG_SMS) held |= GG_START; }
    if (frame % 16 == 0 && frame) {
        state = state * 1103515245u + 12345u;
        held = (state >> 16) & 63;
        if (((state >> 24) & 15) == 0 && !GG_SMS) held |= GG_START;
    }
    keys = (uint8_t)held;
    if (frame >= 30 && frame < 40) keys |= GG_IS_SMS ? GG_B1 : GG_START;
#else
    for (const char *p = GG_BENCH_INPUT; *p;) {
        unsigned at = 0, mask = 0, len = 0;
        while (*p >= '0' && *p <= '9') at = at * 10 + (unsigned)(*p++ - '0');
        for (p++; *p >= '0' && *p <= '9';) mask = mask * 10 + (unsigned)(*p++ - '0');
        for (p++; *p >= '0' && *p <= '9';) len = len * 10 + (unsigned)(*p++ - '0');
        if (frame >= at && frame < at + len) keys |= (uint8_t)mask;
        while (*p && *p++ != ',') {}
    }
#endif
    return keys;
}
static void bench_frame() {
    static uint32_t t0, last, misses;
    const uint32_t f = machine.frames, now = meta_micros(), d = gg_stream_misses - misses;
    misses = gg_stream_misses;
    if (f == GG_BENCH_FROM) t0 = now;
    else if (f > GG_BENCH_FROM && f <= GG_BENCH_TO) {
        const uint32_t us = now - last, part = (f - GG_BENCH_FROM - 1) * 16 / (GG_BENCH_TO - GG_BENCH_FROM);
        gg_bench[2 + (d < 4 ? d : d < 6 ? 4 : d < 9 ? 5 : 6)]++;
        gg_bench[9] += us > 16688; gg_bench[10] += us > 25000; gg_bench[11] += us > 33400;
        if (us > gg_bench[12]) gg_bench[12] = us;
        gg_bench_window[part] += us;
        gg_bench[13] += (uint32_t)skip_video; /* the frame just ended was not drawn (paced build) */
    }
    last = now;
    if (f == GG_BENCH_TO) { gg_bench[1] = now - t0; gg_bench[0] = 1; for (;;) {} }
    gg_buttons(bench_keys(f));
}
#endif
static const char version[] __attribute__((used)) = "m0boy-gg-" META_TARGET_NAME " " GBL_GEN_VERSION;

#if GG_SMS
/* One source line per row (the 256 -> 160 filter is already the costly
 * part): the line nearest the row's centre. */
static unsigned display_row(unsigned y) { return gg_sms_display_row(y); }
/* Pokitto: the 120 rows drawn on 150, one in four twice */
#define GG_PAIRS 0
#elif !META_TARGET_POKITTO
static unsigned display_row(unsigned y) { return gg_meta_display_row(y); }
#define GG_PAIRS 1
#else
#define GG_PAIRS 0
#endif
#if !META_TARGET_POKITTO
static void wait_slot(unsigned slot) {
    while ((int32_t)(meta_lcd_done_seq() - line_sequences[slot]) < 0) {}
}
#endif

/* Which lines are drawn, and how (gg.h): every line on the Pokitto; on the
 * META the first of two lines sharing a row is kept, the second averaged in. */
static int draw_line(void *, unsigned y) {
#ifdef GG_BENCH_NOVIDEO
    return 0; /* the game and the VDP alone */
#endif
    if (skip_video) return 0;
#if GG_SMS
    return gg_sms_line_shown(y);
#elif !GG_PAIRS
    (void)y;
    return 1;
#else
    const int mode = gg_meta_draw_mode(y);
    if (mode == 2) {
#if !META_TARGET_POKITTO
        wait_slot(line_slot ^ 1); /* the other buffer is free once sent */
        machine.row_tmp = line_buffers[line_slot ^ 1] + GG_ROW_PAD;
#endif
    }
    return mode;
#endif
}

#if META_TARGET_POKITTO && GG_SMS
static void row(void *, unsigned y, uint16_t *pixels) {
    const unsigned r = display_row(y), first = r * 5 / 4, last = (r + 1) * 5 / 4;
    for (unsigned k = first; k < last; k++) pokitto_lcd_row_zoom(k + 13, pixels); /* 150 rows centred */
}
#elif META_TARGET_POKITTO
static void row(void *, unsigned y, uint16_t *pixels) {
    const unsigned line = y - GG_TOP, last = gg_pokitto_rows[line + 1]; /* 144 -> 176 */
    for (unsigned r = gg_pokitto_rows[line]; r < last; r++) pokitto_lcd_row_zoom(r, pixels);
}
#else
static void row(void *, unsigned y, uint16_t *pixels) {
#ifdef GG_BENCH_NOLCD /* the lines are drawn, not sent */
    (void)y; (void)pixels;
    return;
#endif
    line_sequences[line_slot] = meta_lcd_send_rows(pixels, (uint8_t)(display_row(y) + (GG_SMS ? 4 : 0)), 1);
    line_slot ^= 1;
    wait_slot(line_slot);
    machine.row_out = line_buffers[line_slot] + GG_ROW_PAD;
}
#endif

static void frame(void *) {
    meta_audio_frame();
    meta_lcd_end_frame();
#if defined(GG_BENCH) && !defined(GG_BENCH_PACED)
    bench_frame();
    return;
#endif
    uint8_t keys = 0;
#if META_TARGET_POKITTO
    uint8_t raw = pokitto_buttons();
    keys = ((raw & POKITTO_UP) ? GG_UP : 0) | ((raw & POKITTO_DOWN) ? GG_DOWN : 0) |
           ((raw & POKITTO_LEFT) ? GG_LEFT_KEY : 0) | ((raw & POKITTO_RIGHT) ? GG_RIGHT_KEY : 0) |
           ((raw & POKITTO_A) ? GG_B1 : 0) | ((raw & POKITTO_B) ? GG_B2 : 0) | ((raw & POKITTO_C) ? (GG_SMS ? GG_PAUSE : GG_START) : 0);
#else
    uint8_t raw = meta_pad_read(); /* META_BUTTONS_SPI_HZ, library bit order */
    /* META: A is button 1, B button 2, MENU is START */
    keys = ((raw & 8) ? GG_UP : 0) | ((raw & 1) ? GG_DOWN : 0) | ((raw & 2) ? GG_LEFT_KEY : 0) |
           ((raw & 4) ? GG_RIGHT_KEY : 0) | ((raw & 16) ? GG_B1 : 0) | ((raw & 32) ? GG_B2 : 0) | ((raw & 64) ? (GG_SMS ? GG_PAUSE : GG_START) : 0);
    if (WDT->CTRL.reg & WDT_CTRL_ENABLE) { WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY; }
#endif
    gg_buttons(keys);
#ifdef GG_BENCH_PACED /* the real pacing and frame skipping, scripted buttons */
    bench_frame();
#endif
#if GG_CART_ON_CARD
    gg_save_tick();
#endif
    /* NTSC: 59736 T-states at 3.579545 MHz */
    deadline += 16688u;
    skip_video = (int32_t)(meta_micros() - deadline) > 0 && skipped_run < GG_MAX_SKIP;
    skipped_run = skip_video ? skipped_run + 1 : 0;
    while ((int32_t)(meta_micros() - deadline) < 0) meta_audio_idle();
    if ((int32_t)(meta_micros() - deadline) > 100128) deadline = meta_micros();
}

int main() {
#if META_TARGET_POKITTO
    pokitto_buttons_init();
#else
    meta_spi_init();
#endif
    meta_clock_init();
#if !META_TARGET_POKITTO && META_CLOCK_DEFAULT != 48
    meta_clock_set(META_CLOCK_DEFAULT); /* META_OVERCLOCK */
#endif
    meta_lcd_init();
    meta_lcd_fill(0);
#if GG_STREAM || GG_CART_ON_CARD
    /* ROM pages on the card: no card, file missing, fragmented, another ROM
     * fill the screen red, magenta, yellow, blue */
    if (int err = gg_stream_open(sg_rom, (uint32_t)sg_rom_size)) {
        static const uint16_t colours[5] = { 0, 0xf800, 0xf81f, 0xffe0, 0x001f };
        const uint16_t c = colours[err];
#if META_TARGET_POKITTO
        meta_lcd_fill(c);
#else
        meta_lcd_fill((uint16_t)(c << 8 | c >> 8));
#endif
        for (;;) {}
    }
#endif
#if SG_CART_RAM && !GG_CART_ON_CARD
    gg_init(sg_rom, sg_rom_size, cart_ram);
#else
    gg_init(sg_rom, sg_rom_size, 0);
#endif
    machine.lookup = sg_gen_lookup;
    machine.draw_line = draw_line;
    machine.row = row;
    machine.row_out = line_buffers[0] + GG_ROW_PAD;
    machine.row_tmp = line_buffers[1] + GG_ROW_PAD;
    machine.frame = frame;
    {   /* stack below the guard for runtime calls; at least 1 KiB for nesting */
        uintptr_t stack = (uintptr_t)&__StackTop - (uintptr_t)&end, reserve = 2560;
        if (stack < reserve + 1024) reserve = stack > 2048 ? stack - 1024 : stack / 2;
        sg_stack_limit = (uintptr_t)&end + reserve;
    }
#ifdef GG_SELFTEST
    gg_selftest_result = gg_render_selftest() + 1; /* read back by gm0 or a debugger */
    for (;;) {}
#endif
    meta_audio_init();
    deadline = meta_micros();
    __asm__ volatile("" : : "r"(version));
    for (;;) gg_run();
}
