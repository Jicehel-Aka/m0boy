/* SG-1000 frontend for the Gamebuino META and the Pokitto: recompiled code
 * (sg_gen.c) on the shared runtime, the consoles' LCD, audio, clock and
 * button drivers. */
#include "sg1000.h"
#include "sg_config.h"
#include "meta_target.h"
#include "meta_clock.h"
#include "meta_audio.h"
#include "meta_lcd.h"
#if META_TARGET_ESPBOY
#include "espboy_hw.h"
#elif META_TARGET_POKITTO
#include "pokitto_hw.h"
#else
#include "meta_hw.h"
#endif
#include "sg_frontend.h"
#if defined(GG_STATE_NAME83)
#include "meta_card.h"
#endif
extern "C" {
extern const uint8_t sg_rom[];
extern const size_t sg_rom_size;
sg_fn sg_gen_lookup(uint16_t pc);
#if META_TARGET_POKITTO && !defined(GG_STATE_NAME83) /* else pokitto_sd.c */
void pokitto_sd_clock_changed(uint32_t) {}
#endif
}
#if SG_CART_RAM
static uint8_t cart_ram[SG_CART_RAM_SIZE] __attribute__((aligned(4)));
#endif
#if META_TARGET_POKITTO
/* one row, or with save states the 512 bytes of their sector scratch */
#if defined(GG_STATE_NAME83)
static uint16_t line_buffer[256] __attribute__((aligned(4)));
#else
static uint16_t line_buffer[SG_OUT_WIDTH] __attribute__((aligned(4)));
#endif
#else
/* Displayed lines go to the LCD in batches of consecutive rows (one window
 * and one DMA chain per batch); two batches alternate. */
#define SG_BATCH (SG_LOW_RAM ? 1 : 4)
static uint16_t line_buffers[2][SG_BATCH][SG_OUT_WIDTH] __attribute__((aligned(4)));
static uint32_t line_sequences[2];
static unsigned line_slot, batch_rows, batch_dest;
#endif
static uint32_t deadline;
#ifndef SG_MAX_SKIP
#define SG_MAX_SKIP 2
#endif
static int skip_video;
static unsigned skipped_run;
#ifdef SG_SELFTEST
extern "C" { volatile unsigned sg_selftest_result; }
#endif
static const char version[] __attribute__((used)) = "m0boy-sg1000-" META_TARGET_NAME " " GBL_GEN_VERSION;

static int draw_line(void *, unsigned y);
static void row(void *, unsigned y, uint16_t *pixels);
static void frame(void *);
/* The console set up: the runtime inited and the callbacks in place (the
 * option menu's Redemarrer runs it again). */
static void sg_start(void) {
#if SG_CART_RAM
    sg1000_init(sg_rom, sg_rom_size, cart_ram);
#if META_TARGET_ESPBOY
    espboy_ram_load(cart_ram, SG_CART_RAM_SIZE);
#endif
#else
    sg1000_init(sg_rom, sg_rom_size, 0);
#endif
    machine.lookup = sg_gen_lookup;
    machine.draw_line = draw_line;
    machine.row = row;
#if META_TARGET_POKITTO
    machine.row_out = line_buffer;
#else
    machine.row_out = line_buffers[0][0];
#endif
    machine.frame = frame;
}
/* ---- option menu (sg_menu.h): HOME held half a second, MENU stays Pause -- */
extern "C" uint16_t *meta_menu_scratch(unsigned *rows) {
#if META_TARGET_POKITTO
    *rows = 1; /* the single line buffer: bands sliced to one row */
    return line_buffer;
#else
    /* the batch buffers, contiguous: the menu's bands of 8 rows are sliced
     * to what fits */
    *rows = 2u * SG_BATCH;
    return line_buffers[0][0];
#endif
}
#if META_TARGET_POKITTO
static unsigned pause_frames; /* a HOME tap (the Pokitto's C) becomes Pause */
#define SG_MENU_TAP() do { pause_frames = 6; } while (0)
#else
#define SG_MENU_TAP() do { } while (0)
#endif
#define SG_MENU_RESTART() do { \
        sg_start(); /* the cartridge RAM cleared: a power cycle */ \
        deadline = meta_micros(); \
        skipped_run = 0; \
    } while (0)
#if defined(GG_STATE_NAME83)
#define SG_MENU_SD_DESELECT() meta_sd_deselect()
#else
#define SG_MENU_SD_DESELECT() do { } while (0)
#endif
#if META_TARGET_ESPBOY
#define SG_MENU_QUIT() do { espboy_restart(); } while (0)
#elif META_TARGET_POKITTO
#define SG_MENU_QUIT() do { \
        meta_audio_stop(); \
        SG_MENU_SD_DESELECT(); \
        pokitto_enter_loader(); \
        meta_audio_init(); \
    } while (0)
#else
#if META_USB
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
        SG_MENU_SD_DESELECT(); \
        *(volatile uint32_t *)0x20007FFC = 0x3aecfd88u; \
        NVIC_SystemReset(); \
    } while (0)
/* Hand the console back to the Gamebuino loader in the state it expects
 * (the Game Boy frontend's enter_loader). */
#define SG_MENU_QUIT() do { \
        meta_lcd_end_frame(); \
        meta_clock_set_lcd_spi(24); /* the loader expects the core's clocks */ \
        meta_clock_set(48); \
        meta_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        meta_watchdog_stop(); \
        SG_MENU_USB_STOP(); \
        SG_MENU_SD_DESELECT(); \
        ((void (*)(void))(*((uint32_t *)0x3FF4)))(); \
    } while (0)
#endif
/* The game draws 120 (Pokitto 150) of the screen's rows: the menu's
 * leftovers on the others are cleared. */
#define SG_MENU_RESUME() do { \
        meta_lcd_fill(0); \
        deadline = meta_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#if defined(GG_STATE_SLOTS)
/* Save states (sg_menu.h's Etat lines): the card's state file, in the
 * folder of the game's .BIN on the META (ESPboy: the flash,
 * espboy_flash.cpp); the line buffers as the sector scratch (the menu has
 * closed). */
#if META_TARGET_POKITTO
#define SG_MENU_STATE_BUF() ((uint8_t *)line_buffer)
#else
#define SG_MENU_STATE_BUF() ((uint8_t *)line_buffers)
#endif
#define SG_MENU_STATE_SLOTS GG_STATE_SLOTS
#define SG_MENU_STATE_LOADED() \
    do { \
        deadline = meta_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#endif
#if defined(GG_STATE_NAME83)
static int sg_card_open(void) {
    if (!meta_sd_init()) return 0;
#if !META_TARGET_POKITTO
    meta_fat_dir = meta_fat_game_dir(meta_card_sector, SG_MENU_STATE_BUF(), GG_BIN_NAME83, META_APP_START);
#endif
    return 1;
}
#endif
#include "sg_menu.h"

static unsigned display_row(unsigned y) {
#if META_TARGET_POKITTO
    return (y * 25) >> 5; /* 192 -> 150 */
#else
    return (y * 5) >> 3; /* 192 -> 120 */
#endif
}
/* Every source line counts: two lines that land on the same screen row are
 * drawn together as one row (the renderer averages them), so no line is
 * dropped (192 -> 120: rows of 2,2,1,2,1 lines; 192 -> 150 alike). */
static int draw_line(void *, unsigned y) {
    if (skip_video) return 0;
    if (y + 1 < SG_HEIGHT && display_row(y + 1) == display_row(y)) return 0; /* first of a pair */
    return y && display_row(y - 1) == display_row(y) ? 2 : 1;
}
/* Lines are rendered straight into the buffer machine.row_out points to. */
#if META_TARGET_POKITTO
static void row(void *, unsigned y, uint16_t *pixels) {
    pokitto_lcd_row_zoom(display_row(y) + 13, pixels);
}
#else
static void flush_batch(void) {
    if (!batch_rows) return;
    line_sequences[line_slot] = meta_lcd_send_rows(line_buffers[line_slot], (uint8_t)(batch_dest + 4), (uint8_t)batch_rows);
    line_slot ^= 1;
    batch_rows = 0;
    while ((int32_t)(meta_lcd_done_seq() - line_sequences[line_slot]) < 0) {}
    machine.row_out = line_buffers[line_slot][0];
}
static void row(void *, unsigned y, uint16_t *) {
    if (!batch_rows) batch_dest = display_row(y);
    if (++batch_rows == SG_BATCH) flush_batch();
    else machine.row_out = line_buffers[line_slot][batch_rows];
}
#endif
static void frame(void *) {
#if !META_TARGET_POKITTO
    flush_batch();
#endif
    meta_audio_frame();
    meta_lcd_end_frame();
    uint8_t keys = 0, pause = 0;
    const uint8_t raw = SG_PAD_RAW();
#if META_TARGET_POKITTO
    keys = ((raw & POKITTO_UP) ? 1 : 0) | ((raw & POKITTO_DOWN) ? 2 : 0) | ((raw & POKITTO_LEFT) ? 4 : 0) |
           ((raw & POKITTO_RIGHT) ? 8 : 0) | ((raw & POKITTO_A) ? 16 : 0) | ((raw & POKITTO_B) ? 32 : 0);
    if (pause_frames) { pause = 1; pause_frames--; }
#else
    keys = ((raw & 8) ? 1 : 0) | ((raw & 1) ? 2 : 0) | ((raw & 2) ? 4 : 0) | ((raw & 4) ? 8 : 0) | (raw & 48);
    pause = raw & 64;
    SG_PAD_WDT();
#endif
#if META_TARGET_ESPBOY && SG_CART_RAM
    espboy_ram_tick(cart_ram, SG_CART_RAM_SIZE, machine.frames);
#endif
    sg1000_buttons(0, keys);
    sg1000_pause(pause);
    sg_menu_frame();
    sg_menu_poll(raw);
    sg_menu_tick();
    SG_PACE_FRAME(deadline, skip_video, skipped_run, SG_MAX_SKIP);
}
#if META_TARGET_ESPBOY
int espboy_main() {
#else
int main() {
#endif
    SG_STACK_PAINT();
    SG_PLATFORM_INIT();
#if defined(GG_STATE_NAME83)
    meta_state_init(sg_card_open(), SG_MENU_STATE_BUF());
#endif
    sg_start();
#if META_TARGET_ESPBOY && defined(GG_STATE_SLOTS)
    meta_state_init(1, SG_MENU_STATE_BUF()); /* the slots follow the cartridge RAM's copies */
#endif
    SG_STACK_GUARD();
#ifdef SG_SELFTEST
    sg_selftest_result = sg1000_render_selftest() + 1; /* read back by a debugger or gm0 */
    for (;;) {}
#endif
    meta_audio_init();
    deadline = meta_micros();
    /* Keep an inspectable source/target version stamp in the firmware. */
    __asm__ volatile("" : : "r"(version));
    for (;;) sg1000_run();
}
