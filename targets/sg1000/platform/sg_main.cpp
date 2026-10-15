/* SG-1000 frontend for the Gamebuino META and the Pokitto: recompiled code
 * (sg_gen.c) on the shared runtime, the consoles' LCD, audio, clock and
 * button drivers. */
#include "sg1000.h"
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
void pokitto_sd_clock_changed(uint32_t) {}
#endif
}
#if SG_CART_RAM
static uint8_t cart_ram[SG_CART_RAM_SIZE];
#endif
#if META_TARGET_POKITTO
static uint16_t line_buffer[SG_OUT_WIDTH] __attribute__((aligned(4)));
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
static const char version[] __attribute__((used)) = "m0boy-sg1000-" META_TARGET_NAME " " SG_GEN_VERSION;

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
#if META_TARGET_POKITTO
    uint8_t raw = pokitto_buttons();
    keys = ((raw & POKITTO_UP) ? 1 : 0) | ((raw & POKITTO_DOWN) ? 2 : 0) | ((raw & POKITTO_LEFT) ? 4 : 0) |
           ((raw & POKITTO_RIGHT) ? 8 : 0) | ((raw & POKITTO_A) ? 16 : 0) | ((raw & POKITTO_B) ? 32 : 0);
    pause = raw & POKITTO_C;
#else
    meta_spi_rate(12000000u);
    for (unsigned i = 0; i < 2 && (SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); i++) (void)SERCOM4->SPI.DATA.reg;
    SERCOM4->SPI.STATUS.reg = SERCOM_SPI_STATUS_BUFOVF;
    PORT->Group[1].OUTCLR.reg = 1u << 3;
    uint8_t raw = (uint8_t)~meta_spi_transfer(1);
    PORT->Group[1].OUTSET.reg = 1u << 3;
    keys = ((raw & 8) ? 1 : 0) | ((raw & 1) ? 2 : 0) | ((raw & 2) ? 4 : 0) | ((raw & 4) ? 8 : 0) | (raw & 48);
    pause = raw & 64;
    if (WDT->CTRL.reg & WDT_CTRL_ENABLE) { WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY; }
#endif
    sg1000_buttons(0, keys);
    sg1000_pause(pause);
    /* NTSC: 59736 T-states at 3.579545 MHz.  CPU, PSG and sprite status
     * keep full precision whatever is skipped. */
    deadline += 16688u;
    /* Late: skip presentations, at most SG_MAX_SKIP in a row (as the Game
     * Boy runtime's META_MAX_SKIP); the game itself keeps its speed. */
    skip_video = (int32_t)(meta_micros() - deadline) > 0 && skipped_run < SG_MAX_SKIP;
    skipped_run = skip_video ? skipped_run + 1 : 0;
    while ((int32_t)(meta_micros() - deadline) < 0) meta_audio_idle();
    /* behind by more than 6 frames (a stall): restart from now instead of
     * running fast to catch up; smaller debts are paid back */
    if ((int32_t)(meta_micros() - deadline) > 100128) deadline = meta_micros();
}
int main() {
#if META_TARGET_POKITTO
    pokitto_buttons_init();
#else
    meta_spi_init();
    PORT->Group[1].DIRSET.reg = 1u << 3;
    PORT->Group[1].OUTSET.reg = 1u << 3;
#endif
    meta_clock_init();
#if !META_TARGET_POKITTO && META_CLOCK_DEFAULT != 48
    meta_clock_set(META_CLOCK_DEFAULT); /* META_OVERCLOCK */
#endif
    meta_lcd_init();
    meta_lcd_fill(0);
#if SG_CART_RAM
    sg1000_init(sg_rom, sg_rom_size, cart_ram);
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
    /* Below the guard, runtime calls made from a region still need stack:
     * sync, line rendering, audio catch-up, LCD/audio drivers and
     * interrupts (about 2 KiB, -fstack-usage); the rest is for nesting. */
    {   /* at least 1 KiB left for nesting when RAM is tight (cartridge RAM) */
        uintptr_t stack = (uintptr_t)&__StackTop - (uintptr_t)&end, reserve = 2560;
        if (stack < reserve + 1024) reserve = stack > 2048 ? stack - 1024 : stack / 2;
        sg_stack_limit = (uintptr_t)&end + reserve;
    }
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
