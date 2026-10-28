/* PC Engine frontend for the ESP32 consoles (Gamebuino AKA, ODROID-GO):
 * the HuC6280/VDC runtime (recompiled code with the interpreter behind it),
 * the consoles' LCD, audio, clock and button drivers.  The runtime pushes
 * every rendered line here (pce.c's row callback): the VDC's active window
 * is planned onto the panel's 120 source rows (the glass doubles them,
 * M0_PANEL_SRC_ROWS = M0_SCREEN_ROWS = 120), the width is sampled 256 -> 160
 * with the 5-in-8 box filter (another active width: nearest pixel).  Two
 * source lines that land on one panel row are blended (m0_avg2_panel), so
 * no line is dropped. */
#include "pce_config.h"
#include "pce.h"
#include "m0_target.h"
#include "m0_clock.h"
#include "m0_audio.h"
#include "m0_lcd.h"
#if M0_TARGET_ESP32
#if __has_include("odroid_hw.h")
#include "odroid_hw.h"
#else
#include "aka_hw.h"
#endif
#else
#error "the PC Engine's console frontend is the ESP32 family's for now"
#endif
#include "m0_scale.h" /* the box filter (256 -> 160) and the vertical plan */
#include "m0_rows.h"
extern "C" {
extern const uint8_t pce_rom[];
}
static const char version[] __attribute__((used)) = "m0boy-pce-" M0_TARGET_NAME " " GBL_GEN_VERSION;

static uint16_t line_buffer[2][160] __attribute__((aligned(4)));
static m0_rows rows;
static uint32_t deadline;
#ifndef PCE_MAX_SKIP
#define PCE_MAX_SKIP 2
#endif
static int skip_video;
static unsigned skipped_run;
static unsigned last_plan_row;
static int have_last;

static void row(void *, unsigned line, const uint16_t *pixels, unsigned width);
static void pce_start(void) {
    pce_init(pce_rom, PCE_ROM_SIZE);
    machine.row = row; /* pce_init clears it: every rendered line comes here */
}
/* The active window's plan: [first_active_line, +active_height) onto the
 * panel's 120 rows.  The VDC's window is the game's own (VDW/ VDS), read
 * live: a game that reprograms it mid-frame replans from the next line. */
static int display_row(unsigned line) {
    const unsigned first = machine.first_active_line, height = machine.vdc.active_height;
    if (!machine.have_frame || height < 24 || line < first || line - first >= height) return -1;
    return (int)m0_row_of(line - first, height, 120);
}
static void row(void *, unsigned line, const uint16_t *pixels, unsigned width) {
    if (skip_video) return;
    const int r = display_row(line);
    if (r < 0) {
        have_last = 0;
        return;
    }
    uint16_t *dst = line_buffer[rows.slot];
    if (width == 256) {
        for (unsigned x = 0; x < 256; x += 8) {
            uint32_t s[8];
            for (unsigned i = 0; i < 8; i++) s[i] = m0_spread565(pixels[x + i]);
            m0_box58_fast(s, dst + x / 8 * 5);
        }
    } else {
        for (unsigned x = 0; x < 160; x++)
            dst[x] = m0_panel(pixels[(unsigned)((uint32_t)x * width / 160)]);
    }
    if (have_last && r == (int)last_plan_row) {
        /* the second line of the pair blends over the first (already sent):
         * the averaged row goes out from the other buffer */
        const uint16_t *prev = line_buffer[rows.slot ^ 1];
        for (unsigned x = 0; x < 160; x++) dst[x] = m0_avg2_panel(prev[x], dst[x]);
        last_plan_row = (unsigned)r;
        m0_rows_send(&rows, dst, (unsigned)r, 1);
        return;
    }
    last_plan_row = (unsigned)r;
    have_last = 1;
    m0_rows_send(&rows, dst, (unsigned)r, 1);
}
/* ---- option menu (sg_menu.h): HOME held half a second, MENU stays Pause -- */
extern "C" uint16_t *m0_menu_scratch(unsigned *rows_out) {
    *rows_out = 2;
    return line_buffer[0];
}
#define SG_MENU_TAP() do { } while (0)
#define SG_MENU_RESTART() do { \
        pce_start(); \
        deadline = m0_micros(); \
        skipped_run = 0; \
    } while (0)
#define SG_MENU_QUIT() do { esp32_restart(); } while (0)
#define SG_MENU_RESUME() do { \
        m0_lcd_fill(0); \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#include "sg_frontend.h" /* the console setup, the pad, the pacing, the stack guard */
#include "sg_menu.h"

/* the stack guard's target (sg_frontend.h's SG_STACK_GUARD writes it; the
 * Sega runtimes own the symbol, the PCE's lives here) */
extern "C" {
uintptr_t sg_stack_limit;
}
/* the Arduino core calls m0boy_console_main (esp32_hw.cpp's setup) */
extern "C" int m0boy_console_main(void);
extern "C" int m0boy_console_main() {
    SG_STACK_PAINT();
    SG_PLATFORM_INIT();
    pce_start();
    SG_STACK_GUARD();
    m0_audio_init();
    deadline = m0_micros();
    /* Keep an inspectable source/target version stamp in the firmware. */
    __asm__ volatile("" : : "r"(version));
    for (;;) {
        pce_frame();
        m0_audio_frame();
        m0_lcd_end_frame();
        const uint8_t mb = M0_PAD(SG_PAD_RAW());
        unsigned keys = 0;
        if (mb & M0_MB_UP) keys |= PCE_BTN_UP;
        if (mb & M0_MB_DOWN) keys |= PCE_BTN_DOWN;
        if (mb & M0_MB_LEFT) keys |= PCE_BTN_LEFT;
        if (mb & M0_MB_RIGHT) keys |= PCE_BTN_RIGHT;
        if (mb & M0_MB_A) keys |= PCE_BTN_I;
        if (mb & M0_MB_B) keys |= PCE_BTN_II;
        if (mb & M0_MB_MENU) keys |= PCE_BTN_RUN; /* the AKA's RUN or D */
        machine.input = (uint8_t)keys;
        sg_menu_frame();
        sg_menu_poll(mb);
        sg_menu_tick();
        SG_PACE_FRAME(deadline, skip_video, skipped_run, PCE_MAX_SKIP);
    }
}
