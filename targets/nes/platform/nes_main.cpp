/* NES frontend for the Gamebuino META and the Pokitto: the interpreted
 * cartridge (nes_cart.c) on the runtime in ../runtime, the consoles' LCD,
 * audio, clock and button drivers.
 *
 * The 256x240 picture is scaled to the panel by dropping whole rows
 * (240 -> 128) and sampling the columns (256 -> 160).  Buttons: the d-pad,
 * A = A, B = B, MENU = Start, HOME = Select; HOME held half a second opens
 * the option menu (sg_menu.h, as in the other frontends). */
#include "nes_config.h"
#include "nes.h"
#include "m0_target.h"
#include "m0_clock.h"
#include "m0_audio.h"
#include "m0_lcd.h"
#if M0_TARGET_ESPBOY
#include "espboy_hw.h"
#elif M0_TARGET_POKITTO
#include "pokitto_hw.h"
#elif M0_TARGET_PC
#include "pc_hw.h"
#include <stdlib.h>
#elif M0_TARGET_ESP32
#if __has_include("odroid_hw.h")
#include "odroid_hw.h"
#else
#include "aka_hw.h"
#endif
#else
#include "meta_hw.h"
#endif
#include "m0_pad.h"
#include "sg_frontend.h"
/* The panel's byte order lives here, not in the runtime: ppu.c keeps its
 * palette as native RGB565 and every console's frontend converts on the way
 * out (META/ESP32's SPI takes the high byte first, Pokitto/PC the low one). */
#define M0_PANEL_SWAP NES_PANEL_SWAP
#include "m0_scale.h"
extern "C" {
extern const uint8_t nes_prg[];
extern const uint8_t nes_chr[];
}
/* one displayed row at a time: 160 columns sampled from the 256 of a row */
static uint16_t line_buffer[2][160] __attribute__((aligned(4)));
static uint32_t line_sequence[2];
static unsigned line_slot;
static uint32_t deadline;
#ifndef NES_MAX_SKIP
#define NES_MAX_SKIP 2
#endif
static int skip_video;
static unsigned skipped_run;
static const char version[] __attribute__((used)) = "m0boy-nes-" M0_TARGET_NAME " " GBL_GEN_VERSION;

/* 240 -> 128 rows: eight panel rows hold fifteen source lines, the first
 * line of each group is kept (the nearest line of each row: groups of two
 * lines, of one when the source line is a multiple of 15).  ARMv6-M has no
 * multiply-high, so dividing would call __aeabi_uidiv; the run is walked
 * instead.  The 112 lines the panel drops still walk v and keep sprite-0
 * honest, they just skip composing the pixels. */
static unsigned next_line, shown_rows;
static int want_row(void *host, unsigned y) {
    (void)host;
    if (skip_video) return 0;
    if (y == 0) { next_line = 0; shown_rows = 0; }
    if (y != next_line) return 0;
    next_line += (++shown_rows & 7u) ? 2u : 1u;
    return 1;
}
/* 240 -> 128 rows: m0_nearest's column pattern (x*256 + 80)/160 keeps five
 * source columns of every eight, 0 2 3 5 6, so no division is needed. */
static void row(void *host, unsigned y, const uint16_t *pixels) {
    (void)host;
    (void)y;
    if (skip_video) return;
    const uint16_t *src = pixels + machine.ppu.x; /* the stream from fine x */
    uint16_t *dst = line_buffer[line_slot];
    for (unsigned x = 0, s = 0; x < 160; x += 5, s += 8) {
        dst[x + 0] = m0_panel(src[s + 0]);
        dst[x + 1] = m0_panel(src[s + 2]);
        dst[x + 2] = m0_panel(src[s + 3]);
        dst[x + 3] = m0_panel(src[s + 5]);
        dst[x + 4] = m0_panel(src[s + 6]);
    }
    line_sequence[line_slot] = m0_lcd_send_rows(dst, (uint8_t)(shown_rows - 1), 1);
    line_slot ^= 1;
    while ((int32_t)(m0_lcd_done_seq() - line_sequence[line_slot]) < 0) {}
}

/* ---- option menu (sg_menu.h) ---- */
extern "C" uint16_t *m0_menu_scratch(unsigned *rows) {
    *rows = 2;
    return line_buffer[0];
}
#define NES_MENU_RESTART() do { \
        nes_reset(); \
        deadline = m0_micros(); \
        skipped_run = 0; \
    } while (0)
#if M0_TARGET_ESPBOY
#define NES_MENU_QUIT() do { espboy_restart(); } while (0)
#elif M0_TARGET_POKITTO
#define NES_MENU_QUIT() do { \
        m0_audio_stop(); \
        pokitto_enter_loader(); \
        m0_audio_init(); \
    } while (0)
#elif M0_TARGET_PC
#define NES_MENU_QUIT() do { exit(0); } while (0)
#elif M0_TARGET_ESP32
#define NES_MENU_QUIT() do { esp32_restart(); } while (0)
#else
#if META_USB_SERIAL
#define NES_MENU_USB_STOP() meta_usb_stop()
#else
#define NES_MENU_USB_STOP() do { } while (0)
#endif
#define NES_MENU_BOOTLOADER() do { \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        NES_MENU_USB_STOP(); \
        *(volatile uint32_t *)0x20007FFC = 0x3aecfd88u; \
        NVIC_SystemReset(); \
    } while (0)
#define NES_MENU_QUIT() do { \
        m0_lcd_end_frame(); \
        m0_clock_set_lcd_spi(24); \
        m0_clock_set(48); \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        NES_MENU_USB_STOP(); \
        ((void (*)(void))(*((uint32_t *)0x3FF4)))(); \
    } while (0)
#endif
#define NES_MENU_RESUME() do { \
        m0_lcd_fill(0); \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#define SG_MENU_RESTART() NES_MENU_RESTART()
#define SG_MENU_QUIT() NES_MENU_QUIT()
#define SG_MENU_RESUME() NES_MENU_RESUME()
#define SG_MENU_BOOTLOADER() NES_MENU_BOOTLOADER()
#if defined(M0_TARGET_META) && M0_TARGET_META && META_USB_SERIAL
#include "meta_usb_msc.h"
#define SG_MENU_USB_SD() do { \
        m0_lcd_end_frame(); \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        unsigned rows; \
        uint8_t *sector = (uint8_t *)m0_menu_scratch(&rows); \
        meta_usb_msc_enter(sector); \
    } while (0)
#endif
#ifndef SG_MENU_TAP
#define SG_MENU_TAP() do { } while (0)
#endif
#include "sg_menu.h"

static void frame(void *) {
    m0_audio_frame();
    m0_lcd_end_frame();
    uint8_t pause = 0;
    const uint8_t mb = M0_PAD(SG_PAD_RAW());
    const uint8_t keys = ((mb & M0_MB_UP) ? NES_UP : 0) | ((mb & M0_MB_DOWN) ? NES_DOWN : 0) |
                         ((mb & M0_MB_LEFT) ? NES_LEFT : 0) | ((mb & M0_MB_RIGHT) ? NES_RIGHT : 0) |
                         ((mb & M0_MB_A) ? NES_A : 0) | ((mb & M0_MB_B) ? NES_B : 0) |
                         ((mb & M0_MB_MENU) ? NES_START : 0) | ((mb & M0_MB_HOME) ? NES_SELECT : 0);
    machine.input = keys;
    SG_PAD_WDT();
    sg_menu_frame();
    sg_menu_poll(mb);
    sg_menu_tick();
    SG_PACE_FRAME(deadline, skip_video, skipped_run, NES_MAX_SKIP);
}

int main() {
    SG_PLATFORM_INIT();
    nes_init(nes_prg, NES_PRG_SIZE, NES_CHR_SIZE ? nes_chr : 0, NES_CHR_SIZE, NES_VERTICAL_MIRROR);
    machine.row = row;
    machine.want_row = want_row;
    m0_audio_init();
    deadline = m0_micros();
    __asm__ volatile("" : : "r"(version));
    for (;;) {
        nes_frame();
        frame(0);
    }
}
