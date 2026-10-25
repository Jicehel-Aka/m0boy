/* NES frontend for the Gamebuino META and the Pokitto: the interpreted
 * cartridge (nes_cart.c) on the runtime in ../runtime, the consoles' LCD,
 * audio, clock and button drivers.
 *
 * The 256x240 picture is scaled to the panel by the PPU (ppu.h: pairs of
 * lines blended, 240 -> 128; the 5-in-8 box filter, 256 -> 160).  Buttons: the d-pad,
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
extern "C" {
extern const uint8_t nes_prg[];
extern const uint8_t nes_chr[];
}
/* one panel row at a time: the PPU writes it, the DMA sends it while the
 * next one is built */
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

/* The PPU scales the picture itself (ppu.h: 15 lines -> 8 rows, the 5-in-8
 * box filter across), straight into the row being filled. */
static void panel_row(void *host, unsigned r) {
    (void)host;
    line_sequence[line_slot] = m0_lcd_send_rows(line_buffer[line_slot], (uint8_t)r, 1);
    line_slot ^= 1;
    while ((int32_t)(m0_lcd_done_seq() - line_sequence[line_slot]) < 0) {}
    machine.ppu.panel_out = line_buffer[line_slot];
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
    machine.ppu.panel = panel_row;
    machine.ppu.panel_out = line_buffer[0];
    m0_audio_init();
    deadline = m0_micros();
    __asm__ volatile("" : : "r"(version));
    for (;;) {
        machine.ppu.skip = (uint8_t)skip_video;
        nes_frame();
        frame(0);
    }
}
