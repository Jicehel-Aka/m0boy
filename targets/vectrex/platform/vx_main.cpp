/* Vectrex frontend for the Gamebuino META: the cartridge and the system ROM
 * (vx_cart.c) on the core in ../runtime, the console's LCD, audio, clock and
 * button drivers.
 *
 * The 512x512 beam square is drawn at 128x128 in a 4-bit phosphor (8 KiB),
 * centred on the panel.  The frame's 30000 cycles run in 128 slices; after
 * each one the next panel row is sent and fades, so the LCD's DMA overlaps
 * the emulation instead of following it.  A stroke stays lit for two frames
 * (VX_FADE): the games that draw at 25 Hz do not blink.
 *
 * Buttons: the d-pad is the stick, A, B, MENU and HOME the four buttons;
 * HOME held half a second opens the option menu (sg_menu.h). */
#include "vx_config.h"
#include "vectrex.h"
#include "m0_target.h"
#include "m0_clock.h"
#include "m0_audio.h"
#include "m0_lcd.h"
#include "meta_hw.h"
#include "m0_pad.h"
#include "sg_frontend.h"
#include "m0_state.h"
#include "m0_sd.h"
#include "m0_fat.h"
#include "m0_card.h"
#include <string.h>
extern "C" {
extern const uint8_t vectrex_bios[];
}

#define VX_SIZE 128
#define VX_LEFT ((160 - VX_SIZE) / 2)
/* 4-bit levels lost per frame */
#define VX_FADE 8

static Vectrex machine;
/* two pixels a byte, left low; word aligned: rows fade a word at a time */
static uint8_t phosphor[VX_SIZE * VX_SIZE / 2] __attribute__((aligned(4)));
/* per row, at least its brightest level (0: black), and whether the panel
 * shows it black already: such rows are neither converted nor sent */
static uint8_t row_level[VX_SIZE], row_black[VX_SIZE];
static uint16_t palette[16];
/* a phosphor byte's two panel pixels, as one word (the left one first in
 * memory) */
static uint32_t pair[256];
/* VX_FADE levels off each of a word's eight 4-bit pixels, at least 0 */
static inline uint32_t fade_word(uint32_t w) {
#if VX_FADE == 8
    /* v - 8 when v >= 8 is v's low three bits: kept where bit 3 is set */
    return w & (((w >> 3) & 0x11111111u) * 7u);
#else
    uint32_t r = 0;
    for (unsigned i = 0; i < 32; i += 4) {
        const int v = (int)((w >> i) & 15u) - VX_FADE;
        r |= (uint32_t)(v > 0 ? v : 0) << i;
    }
    return r;
#endif
}
static uint16_t line_buffer[2][160] __attribute__((aligned(4)));
static uint32_t line_sequence[2];
static unsigned line_slot;
static uint32_t deadline;
#ifndef VX_MAX_SKIP
#define VX_MAX_SKIP 2
#endif
static int skip_video;
static unsigned skipped_run;
static const char version[] __attribute__((used)) = "m0boy-vectrex-" M0_TARGET_NAME " " GBL_GEN_VERSION;
/* completed frames and the time their emulation took (read in the RAM by
 * symbol: gm0 --dump-ram), and gm0's m0input probe on the frame counter
 * and the buttons byte */
static volatile uint32_t vx_frames, vx_busy_us, vx_busy_max_us, vx_skipped, vx_cycles, vx_strokes;
static uint8_t vx_buttons;
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    vx_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &vx_frames, &vx_buttons, 0 };

extern "C" uint32_t vx_state_bytes(void) {
    return (uint32_t)(vx_machine_state_bytes() + sizeof phosphor + sizeof row_level +
                      sizeof row_black + sizeof vx_frames);
}

extern "C" void vx_state_io(void (*io)(void *, uint32_t, void *), void *ctx, int load) {
    vx_machine_state_io(&machine, io, ctx, load);
    io(phosphor, (uint32_t)sizeof phosphor, ctx);
    io(row_level, (uint32_t)sizeof row_level, ctx);
    io(row_black, (uint32_t)sizeof row_black, ctx);
    io((void *)&vx_frames, (uint32_t)sizeof vx_frames, ctx);
}

extern "C" uint32_t vx_state_frame(void) { return vx_frames; }

extern "C" uint32_t vx_state_identity(void) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < vectrex_cart_size; i++) h = (h ^ vectrex_cart[i]) * 16777619u;
    for (unsigned i = 0; i < 8192; i++) h = (h ^ vectrex_bios[i]) * 16777619u;
    return h | 1u;
}

static inline void plot(int x, int y, unsigned z) {
    row_level[y] = (uint8_t)(row_level[y] > z ? row_level[y] : z);
    uint8_t *p = &phosphor[(unsigned)y * (VX_SIZE / 2) + ((unsigned)x >> 1)];
    if (x & 1) {
        if ((unsigned)(*p >> 4) < z) *p = (uint8_t)((*p & 0x0f) | z << 4);
    } else if ((unsigned)(*p & 15) < z) {
        *p = (uint8_t)((*p & 0xf0) | z);
    }
}

#ifdef VX_DRAW_HASH
/* every stroke the core draws, hashed as the PC host does (gm0
 * --dump-ram): two builds compared game frame for game frame */
volatile uint32_t vx_draw_hash __attribute__((used)) = 2166136261u;
static void draw_hash(uint32_t x) {
    for (int i = 0; i < 4; i++) vx_draw_hash = (vx_draw_hash ^ ((x >> (i * 8)) & 255u)) * 16777619u;
}
#endif

/* the core's strokes, 512 -> 128 */
extern "C" void vx_draw(Vectrex *, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                        unsigned z, unsigned cycles) {
#ifdef VX_DRAW_HASH
    draw_hash((uint32_t)x0);
    draw_hash((uint32_t)y0);
    draw_hash((uint32_t)x1);
    draw_hash((uint32_t)y1);
    draw_hash(z << 16 | cycles);
#endif
    x0 >>= 10;
    y0 >>= 10;
    x1 >>= 10;
    y1 >>= 10;
    if (!vx_clip(&x0, &y0, &x1, &y1, VX_SIZE, VX_SIZE)) return;
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    /* intensity 0..127 to 4 bits; a dot held long is brighter */
    unsigned level = (z + 7) >> 3;
    if (!dx && !dy) level += cycles >> 3;
    if (level > 15) level = 15;
    if (!level) return;
    int err = dx - dy;
    for (;;) {
        plot(x0, y0, level);
        if (x0 == x1 && y0 == y1) break;
        int e = err * 2;
        if (e > -dy) { err -= dy; x0 += sx; }
        if (e < dx) { err += dx; y0 += sy; }
    }
}

/* panel row r: the phosphor row in colour, then faded */
static void panel_row(unsigned r) {
    uint8_t *row = &phosphor[r * (VX_SIZE / 2)];
    const unsigned level = row_level[r];
    if (!level && row_black[r]) return;
    row_level[r] = (uint8_t)(level > VX_FADE ? level - VX_FADE : 0);
    if (!level) {
        /* black now: once more to the panel, then left alone */
        if (skip_video) return;
        row_black[r] = 1;
    } else {
        row_black[r] = 0;
    }
    if (!skip_video) {
        uint16_t *out = line_buffer[line_slot];
        for (unsigned x = 0; x < VX_LEFT; x++) out[x] = out[160 - 1 - x] = palette[0];
        uint32_t *o = (uint32_t *)(out + VX_LEFT);
        uint32_t *w = (uint32_t *)row;
        for (unsigned x = 0; x < VX_SIZE / 8; x++) {
            const uint32_t b = w[x];
            o[0] = pair[b & 255u];
            o[1] = pair[(b >> 8) & 255u];
            o[2] = pair[(b >> 16) & 255u];
            o[3] = pair[b >> 24];
            o += 4;
            w[x] = fade_word(b);
        }
        line_sequence[line_slot] = m0_lcd_send_rows(out, (uint8_t)r, 1);
        line_slot ^= 1;
        while ((int32_t)(m0_lcd_done_seq() - line_sequence[line_slot]) < 0) {}
    } else {
        uint32_t *w = (uint32_t *)row;
        for (unsigned x = 0; x < VX_SIZE / 8; x++) w[x] = fade_word(w[x]);
    }
}

static void video_init(void) {
    /* a white phosphor, slightly blue */
    for (unsigned i = 0; i < 16; i++) {
        const unsigned r = i * 29 / 15, g = i * 61 / 15, b = i * 31 / 15;
        uint16_t c = (uint16_t)(r << 11 | g << 5 | b);
        palette[i] = VX_PANEL_SWAP ? (uint16_t)(c << 8 | c >> 8) : c;
    }
    /* little-endian: the low half is the left pixel, the low nibble's */
    for (unsigned b = 0; b < 256; b++) pair[b] = palette[b & 15] | (uint32_t)palette[b >> 4] << 16;
}

/* ---- sound: the AY's int16 to the drivers' 8-bit samples ---- */
static unsigned audio_rate = 22050;
extern "C" void vx_audio_set_rate(unsigned rate) { audio_rate = rate ? rate : 22050u; }
extern "C" void vx_audio_render(uint8_t *buffer, unsigned n) {
    int16_t chunk[64];
    while (n) {
        const unsigned k = n > 64 ? 64 : n;
        ay_render(&machine, chunk, k, audio_rate);
        for (unsigned i = 0; i < k; i++) {
            int s = (chunk[i] >> 8) + 128;
            buffer[i] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
        }
        buffer += k;
        n -= k;
    }
}

/* ---- option menu (sg_menu.h) ---- */
extern "C" uint16_t *m0_menu_scratch(unsigned *rows) {
    *rows = 2;
    return line_buffer[0];
}
static void restart(void) {
    machine.bios = vectrex_bios;
    vx_reset(&machine);
#if VX_FULL_BOOT
    machine.fast_boot = 0;
#endif
    for (unsigned i = 0; i < sizeof phosphor; i++) phosphor[i] = 0;
    for (unsigned i = 0; i < VX_SIZE; i++) row_level[i] = 15, row_black[i] = 0;
}
#define VX_MENU_RESTART() do { \
        restart(); \
        deadline = m0_micros(); \
        skipped_run = 0; \
    } while (0)
#if META_USB_SERIAL
#define VX_MENU_USB_STOP() meta_usb_stop()
#else
#define VX_MENU_USB_STOP() do { } while (0)
#endif
#define SG_MENU_BOOTLOADER() do { \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        VX_MENU_USB_STOP(); \
        *(volatile uint32_t *)0x20007FFC = 0x3aecfd88u; \
        NVIC_SystemReset(); \
    } while (0)
#define VX_MENU_QUIT() do { \
        m0_lcd_end_frame(); \
        m0_clock_set_lcd_spi(24); \
        m0_clock_set(48); \
        m0_audio_stop(); \
        NVIC_DisableIRQ(DMAC_IRQn); \
        m0_watchdog_stop(); \
        VX_MENU_USB_STOP(); \
        ((void (*)(void))(*((uint32_t *)0x3FF4)))(); \
    } while (0)
#define VX_MENU_RESUME() do { \
        m0_lcd_fill(0); \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#define SG_MENU_RESTART() VX_MENU_RESTART()
#define SG_MENU_QUIT() VX_MENU_QUIT()
#define SG_MENU_RESUME() VX_MENU_RESUME()
#define SG_MENU_TITLE "Vectrex"
#define SG_MENU_STATE_SLOTS VX_STATE_SLOTS
#define SG_MENU_STATE_BUF() ((uint8_t *)line_buffer)
#define SG_MENU_STATE_LOADED() do { \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
static int vx_card_open(void) {
    if (!m0_sd_init()) return 0;
    m0_fat_dir = m0_fat_game_dir(m0_card_sector, SG_MENU_STATE_BUF(), VX_BIN_NAME83, META_APP_START);
    return 1;
}
#if META_USB_SERIAL
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

static void frame(void) {
    m0_audio_frame();
    m0_lcd_end_frame();
    const uint8_t mb = M0_PAD(SG_PAD_RAW());
    machine.joy_x = (mb & M0_MB_LEFT) ? 0 : (mb & M0_MB_RIGHT) ? 255 : 128;
    machine.joy_y = (mb & M0_MB_UP) ? 255 : (mb & M0_MB_DOWN) ? 0 : 128;
    machine.buttons = (uint8_t)(((mb & M0_MB_A) ? 1 : 0) | ((mb & M0_MB_B) ? 2 : 0) |
                                ((mb & M0_MB_MENU) ? 4 : 0) | ((mb & M0_MB_HOME) ? 8 : 0));
    machine.via.ay[14] = (uint8_t)~machine.buttons;
    vx_buttons = machine.buttons;
    SG_PAD_WDT();
    sg_menu_frame();
    sg_menu_poll(mb);
    sg_menu_tick();
    SG_PACE_FRAME(deadline, skip_video, skipped_run, VX_MAX_SKIP);
}

int main() {
    SG_PLATFORM_INIT();
    video_init();
    restart();
    m0_state_init(vx_card_open(), SG_MENU_STATE_BUF());
    m0_audio_init();
    deadline = m0_micros();
    __asm__ volatile("" : : "r"(version), "r"(&vx_input_probe));
    for (;;) {
        const uint32_t start = m0_micros();
        machine.via.segments = 0;
        for (unsigned r = 0; r < VX_SIZE; r++) {
            vx_run_until(&machine, machine.frame_end + VX_FRAME_CYCLES * (r + 1) / VX_SIZE);
            panel_row(r);
        }
        vx_end_frame(&machine);
#ifdef VX_DRAW_HASH
        /* the PSG's registers too: the sound's writes go through the VIA */
        for (unsigned i = 0; i < 16; i++) draw_hash(machine.via.ay[i]);
        draw_hash(machine.via.ay_epoch);
#endif
        const uint32_t busy = m0_micros() - start;
        vx_busy_us += busy;
        if (busy > vx_busy_max_us) vx_busy_max_us = busy;
        vx_frames++;
        vx_skipped = machine.via.skipped;
        vx_cycles = machine.cpu.cycles;
        vx_strokes += machine.via.segments;
        frame();
    }
}
