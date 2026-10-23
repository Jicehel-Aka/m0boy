/* PICO-8 frontend for the Gamebuino META: the recompiled cartridge on the
 * runtime in ../runtime, the console's LCD, audio, clock and button drivers.
 *
 * The 128x128 picture is drawn 1:1 in the middle of the 160x128 screen.
 * Buttons: the d-pad, A = O, B = X; MENU restarts the cartridge. */
#include "p8main.h"
#include "p8_audio.h"
#include "p8_config.h"
#include "meta_target.h"
#include "meta_clock.h"
#include "meta_audio.h"
#include "meta_lcd.h"
#if META_TARGET_ESPBOY
#include "espboy_hw.h"
#else
#include "meta_hw.h"
#endif
#include "sg_frontend.h"
#if defined(P8_STATE_NAME83)
#include "meta_card.h"
#endif
#include <string.h>

/* the META sends rows of the picture's 128 columns (narrowed window); the ESPboy's adapter takes 160-wide rows and the picture is at column 16 */
#if META_TARGET_ESPBOY
#define P8_OUT_WIDTH 160
#else
#define P8_OUT_WIDTH 128
#endif
#define P8_PIXELS 128
#define P8_MARGIN 16 /* columns left of the picture */
#if !META_TARGET_ESPBOY
extern "C" void meta_lcd_set_columns(uint8_t x0, uint8_t x1);
#endif
#define P8_BATCH 1

static uint16_t line_buffers[2][P8_BATCH][P8_OUT_WIDTH] __attribute__((aligned(4)));
static uint32_t line_sequences[2];
static unsigned line_slot;
static uint32_t deadline;
static unsigned menu_was_down, audio_phase;
static const char version[] __attribute__((used)) = "m0boy-pico8-" META_TARGET_NAME " " GBL_GEN_VERSION;

#if defined(P8_STATE_SLOTS)
/* Save states hold the heap's absolute pointers: they load only into this
 * very firmware, known by a hash of its code (computed once, then kept). */
#if META_TARGET_ESPBOY
extern "C" const uint32_t _irom0_text_start[], _irom0_text_end[];
#else
extern "C" const uint32_t __etext[];
#endif
extern "C" uint32_t p8_state_identity(void) {
    static uint32_t id;
    if (id) return id;
#if META_TARGET_ESPBOY
    const uint32_t *p = _irom0_text_start, *e = _irom0_text_end; /* word loads: the mapped flash takes no other */
#else
    const uint32_t *p = (const uint32_t *)META_APP_START, *e = __etext;
#endif
    uint32_t h = 2166136261u;
    for (; p < e; p++) h = (h ^ *p) * 16777619u;
    return id = h | 1u;
}
#endif

#if !META_TARGET_POKITTO
/* ---- option menu (sg_menu.h): HOME held half a second, MENU restarts ----- */
extern "C" uint16_t *meta_menu_scratch(unsigned *rows) {
#if META_TARGET_ESPBOY
    *rows = 2u * P8_BATCH; /* the ESPboy's rows take the whole 160 columns */
#else
    *rows = 1; /* the picture's two 128-pixel buffers hold one 160-pixel row (RAM is short) */
#endif
    return line_buffers[0][0];
}
#define SG_MENU_TAP() do { } while (0)
#define SG_MENU_RESTART() do { \
        p8_start(); /* the cartridge again */ \
        deadline = meta_micros(); \
    } while (0)
#if defined(P8_STATE_NAME83)
#define P8_MENU_SD_DESELECT() meta_sd_deselect()
#else
#define P8_MENU_SD_DESELECT() do { } while (0)
#endif
#if META_TARGET_ESPBOY
#define SG_MENU_QUIT() do { espboy_restart(); } while (0)
#else
#if META_USB_SERIAL
#define SG_MENU_USB_STOP() meta_usb_stop()
#else
#define SG_MENU_USB_STOP() do { } while (0)
#endif
/* The USB bootloader (sam-ba) directly, as the Game Gear frontend does. */
#define SG_MENU_BOOTLOADER() do { \
        meta_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        meta_watchdog_stop(); \
        SG_MENU_USB_STOP(); \
        P8_MENU_SD_DESELECT(); \
        *(volatile uint32_t *)0x20007FFC = 0x3aecfd88u; \
        NVIC_SystemReset(); \
    } while (0)
#if META_USB_SD
#include "meta_usb_msc.h"
#define SG_MENU_USB_SD() do { \
        meta_lcd_end_frame(); \
        meta_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        meta_watchdog_stop(); \
        unsigned rows; \
        uint8_t *sector = (uint8_t *)meta_menu_scratch(&rows); \
        meta_usb_msc_enter(sector); \
    } while (0)
#endif
/* Hand the console back to the Gamebuino loader (the Game Boy frontend's
 * enter_loader). */
#define SG_MENU_QUIT() do { \
        meta_lcd_end_frame(); \
        meta_clock_set_lcd_spi(24); /* the loader expects the core's clocks */ \
        meta_clock_set(48); \
        meta_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        meta_watchdog_stop(); \
        SG_MENU_USB_STOP(); \
        P8_MENU_SD_DESELECT(); \
        ((void (*)(void))(*((uint32_t *)0x3FF4)))(); \
    } while (0)
#endif
#define SG_MENU_RESUME() do { deadline = meta_micros(); } while (0)
#if META_TARGET_ESPBOY
/* the adapter takes the picture's 128 columns at 16 of each row, and the
 * menu's 160 narrowed while it is open (the generator's espboy_hw.cpp) */
extern "C" { unsigned char p8_menu_wide; }
#define SG_MENU_SCREEN_ENTER() do { p8_menu_wide = 1; } while (0)
#define SG_MENU_SCREEN_LEAVE() do { p8_menu_wide = 0; } while (0)
#else
/* the menu is 160 columns wide: the whole panel while it is open, then
 * the margins cleared and the picture's columns again */
#define SG_MENU_SCREEN_ENTER() do { \
        meta_lcd_end_frame(); \
        meta_lcd_set_columns(0, 159); \
    } while (0)
#define SG_MENU_SCREEN_LEAVE() do { \
        meta_lcd_end_frame(); \
        meta_lcd_fill(0); \
        meta_lcd_set_columns(P8_MARGIN, P8_MARGIN + P8_PIXELS - 1); \
    } while (0)
#endif
#if defined(P8_STATE_SLOTS)
/* Save states (sg_menu.h's Etat lines), between two frames: the card's
 * state file in the game's folder (ESPboy: the flash, espboy_flash.cpp),
 * the menu's rows as the sector scratch. */
#define SG_MENU_STATE_BUF() ((uint8_t *)line_buffers) /* 512 bytes and more */
#define SG_MENU_STATE_SLOTS P8_STATE_SLOTS
#define SG_MENU_STATE_LOADED() do { deadline = meta_micros(); } while (0)
#endif
#if defined(P8_STATE_NAME83)
static int p8_card_open(void) {
    if (!meta_sd_init()) return 0;
    meta_fat_dir = meta_fat_game_dir(meta_card_sector, SG_MENU_STATE_BUF(), P8_BIN_NAME83, META_APP_START);
    return 1;
}
#endif
#include "sg_menu.h"
#endif /* !META_TARGET_POKITTO */

/* the 16 base colours through the screen palette, as the panel takes them */
static void colour_table(uint16_t *tab) {
    for (unsigned i = 0; i < 16; i++) tab[i] = META_COLOR(p8_palette_rgb[p8g.pal_screen[i] & 15]);
}

static void draw_screen(void) {
    uint16_t tab[16];
    colour_table(tab);
    for (unsigned y = 0; y < P8_H; y += P8_BATCH) {
        uint16_t (*lines)[P8_OUT_WIDTH] = line_buffers[line_slot];
        for (unsigned r = 0; r < P8_BATCH; r++) {
            uint16_t *out = lines[r];
#if META_TARGET_ESPBOY
            out += P8_MARGIN;
#endif
            const uint8_t *in = &p8_screen[(y + r) * 64];
            for (unsigned x = 0; x < 64; x++) {
                const unsigned b = in[x];
                *out++ = tab[b & 15];
                *out++ = tab[b >> 4];
            }
        }
        line_sequences[line_slot] = meta_lcd_send_rows(lines, (uint8_t)y, P8_BATCH);
        line_slot ^= 1;
        while ((int32_t)(meta_lcd_done_seq() - line_sequences[line_slot]) < 0) {}
    }
}

/* A runtime error stops the cartridge: the message stays on the screen. */
extern "C" void p8_error(const char *msg, const char *what) {
    char text[96];
    unsigned n = 0;
    for (const char *s = "error: "; *s && n < sizeof text - 1; s++) text[n++] = *s;
    for (const char *s = msg; *s && n < sizeof text - 1; s++) text[n++] = *s;
    if (*what) text[n++] = ' ';
    for (const char *s = what; *s && n < sizeof text - 1; s++) text[n++] = *s;
    /* wrapped at 30 characters */
    int x, y = 2;
    p8_gfx_cls(0);
    p8g.cam_x = p8g.cam_y = 0;
    for (unsigned i = 0; i < n; i += 30) p8_gfx_print(text + i, n - i > 30 ? 30 : n - i, 2, y += 7, 7, 2, &x, &y);
    /* where it happened */
    char info[40];
    unsigned m = 0;
    for (const char *t = "frame "; *t; t++) info[m++] = *t;
    for (unsigned v = p8_frames, d = 100000; d; d /= 10) if (v / d || m > 6) info[m++] = (char)('0' + v / d % 10);
    info[m++] = ' ';
    for (const char *t = "free "; *t; t++) info[m++] = *t;
    for (size_t v = p8_heap_free_bytes(), d = 100000; d; d /= 10) if (v / d || m > 13) info[m++] = (char)('0' + v / d % 10);
    p8_gfx_print(info, m, 2, y + 7, 6, 2, &x, &y);
#ifdef P8_ERROR_STATS
    {
        extern void p8_heap_stats(size_t out[8][2]);
        size_t st[8][2];
        p8_heap_stats(st);
        const char *names = ".STCUVNc";
        unsigned line = 0;
        const int y0 = 30;
        for (unsigned k = 1; k < 8; k++) {
            char row[40];
            unsigned q = 0;
            row[q++] = names[k];
            row[q++] = ' ';
            for (size_t v = st[k][0], d = 1000; d; d /= 10) if (v / d || q > 2) row[q++] = (char)('0' + v / d % 10);
            row[q++] = ' ';
            for (size_t v = st[k][1], d = 100000; d; d /= 10) if (v / d || q > 8) row[q++] = (char)('0' + v / d % 10);
            int ry; p8_gfx_print(row, q, 2, y0 + 7 * (int)line++, 7, 2, &x, &ry);
        }
    }
#endif
    draw_screen();
    meta_lcd_end_frame();
    for (;;) meta_watchdog_feed();
}

static uint16_t read_buttons(unsigned *menu) {
    const uint8_t raw = SG_PAD_RAW();
    SG_PAD_WDT();
    *menu = raw;
    return (uint16_t)(((raw & 2u) ? 1u : 0u) | ((raw & 4u) ? 2u : 0u) | ((raw & 8u) ? 4u : 0u) | ((raw & 1u) ? 8u : 0u) |
                      ((raw & 16u) ? 16u : 0u) | ((raw & 32u) ? 32u : 0u));
}

#ifdef P8_STACK_CHECK /* debug: the deepest the C stack went, drawn in a corner of the picture */
static void stack_check(void) {
    uint32_t *p = (uint32_t *)(((uintptr_t)&end + 3u) & ~(uintptr_t)3u);
    while (p < (uint32_t *)&__StackTop && *p == SG_STACK_PAINT_WORD) p++;
    const unsigned used = (unsigned)((uintptr_t)&__StackTop - (uintptr_t)p);
    /* a bar on the top row: 128 pixels = 4096 bytes (a red pixel per 32 bytes) */
    for (unsigned x = 0; x < used / 32 && x < 128; x++) p8_screen[x >> 1] = (uint8_t)((x & 1) ? (p8_screen[x >> 1] & 0x0f) | 0x80 : (p8_screen[x >> 1] & 0xf0) | 0x08);
}
#endif

#if META_TARGET_ESPBOY
int espboy_main() {
#else
int main() {
#endif
#ifdef P8_STACK_CHECK
    SG_STACK_PAINT();
#endif
    SG_PLATFORM_INIT();
    meta_lcd_fill(0); /* the margins, once */
#if !META_TARGET_ESPBOY
    meta_lcd_set_columns(P8_MARGIN, P8_MARGIN + P8_PIXELS - 1);
#endif
#if defined(P8_STATE_NAME83)
    meta_state_init(p8_card_open(), SG_MENU_STATE_BUF());
#elif defined(P8_STATE_SLOTS)
    meta_state_init(1, SG_MENU_STATE_BUF());
#endif
    p8_start();
    meta_audio_init();
    deadline = meta_micros();
    __asm__ volatile("" : : "r"(version));
    for (;;) {
        unsigned raw;
        p8_frame(read_buttons(&raw));
#ifdef P8_STACK_CHECK
        stack_check();
#endif
        draw_screen();
        /* the audio driver counts 30 Hz frames */
        if (p8_fps == 30 || (audio_phase ^= 1)) meta_audio_frame();
        meta_lcd_end_frame();
#ifdef P8_STOP_AFTER_US /* debug: stop at a time and show the frame count (gm0 profiling) */
        if (meta_micros() > P8_STOP_AFTER_US) p8_error("stopped", "");
#endif
#if !META_TARGET_POKITTO
        sg_menu_frame();
        sg_menu_poll((uint8_t)raw);
        sg_menu_tick();
#endif
        if ((raw & 64u) && !menu_was_down) p8_start();
        menu_was_down = raw & 64u;
        if (!sg_menu_fast) {
            deadline += p8_fps == 60 ? 16667u : 33333u;
            while ((int32_t)(meta_micros() - deadline) < 0) meta_audio_idle();
            if ((int32_t)(meta_micros() - deadline) > 100000) deadline = meta_micros();
        } else {
            deadline = meta_micros();
        }
    }
}
