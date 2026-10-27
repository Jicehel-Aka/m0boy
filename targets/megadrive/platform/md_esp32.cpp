/* Mega Drive frontend for the ESP32 consoles (Gamebuino AKA, ODROID-GO):
 * the md_full machine sits in SRAM, the VDP's per-line ARGB framebuffer in
 * PSRAM; the 320x224 lines are converted to the panel's RGB565 wire order
 * and pushed 1:1, centred on the 320x240 glass.  The audio is decimated to
 * the family's 11025 Hz mono 8-bit ring.  Buttons: A, B, MENU/C = C,
 * RUN/D = Start, the joystick completes the D-pad. */
#include <esp_heap_caps.h>
#include <string.h>
extern "C" {
#include "md_full.h"
#include "m0_audio.h"
#include "m0_clock.h"
#include "m0_lcd.h"
#include "m0_menu.h"
uint32_t m0_lcd_send_wide(const void *buf, uint8_t row0, uint8_t rows);
}
#include "m0_target.h"
#if __has_include("odroid_hw.h")
#include "odroid_hw.h"
#else
#include "aka_hw.h"
#endif

extern "C" const uint8_t md_rom[];
extern "C" const uint32_t md_rom_size;

/* 320x224 ARGB (the VDP draws each line during the frame, H-int scrolls
 * included), 320 columns of RGB565 eight rows at a time. */
static MdFull machine;
static uint16_t batch[8][320] __attribute__((aligned(64)));
static uint32_t *fb;
static uint32_t deadline;
static int skip_video;
static unsigned skipped_run;
static const char version[] __attribute__((used)) = "m0boy-megadrive-" M0_TARGET_NAME " " GBL_GEN_VERSION;

static uint32_t *framebuffer(void) {
    return (uint32_t *)heap_caps_malloc(320u * 224u * 4u, MALLOC_CAP_SPIRAM);
}

static void push_frame(void) {
    for (unsigned y = 0; y < 224; y += 8) {
        for (unsigned r = 0; r < 8; ++r) {
            const uint32_t *src = fb + (y + r) * 320u;
            uint16_t *dst = batch[r];
            /* native RGB565: the panel's MADCTL answers the channel order,
             * the wire takes the bytes as they sit (no software swap) */
            for (unsigned x = 0; x < 320; ++x) {
                const uint32_t p = src[x];
                dst[x] = (uint16_t)(((p >> 8) & 0xF800u) | ((p >> 5) & 0x07E0u) | ((p >> 3) & 0x001Fu));
            }
        }
        m0_lcd_send_wide(batch, (uint8_t)(8 + y), 8);   /* rows 8..231: centred */
    }
}

static int16_t pcm[2 * 800];
static int held;
extern "C" void md_platform_audio_rate(unsigned hz) { (void)hz; }
/* n mono samples of the frame, each the average of four stereo 44.1 kHz
 * frames off the machine's stream (held when the stream runs short). */
extern "C" void md_platform_audio_render(uint8_t *out, unsigned n) {
    const unsigned got = md_full_audio_read(&machine, pcm, n * 4u);
    unsigned f = 0;
    for (unsigned i = 0; i < n; ++i) {
        int sum = 0, frames = 0;
        for (; frames < 4 && f < got; ++frames, ++f) sum += pcm[2 * f] + pcm[2 * f + 1];
        int v = frames ? (sum / (2 * frames)) >> 7 : held;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        held = v;
        out[i] = (uint8_t)(v + 128);
    }
}

extern "C" int m0boy_console_main(void) {
    m0_lcd_init();
    md_full_reset(&machine);
    fb = framebuffer();
    if (!fb) {
        m0_lcd_fill(M0_COLOR(0x00c00000));
        for (;;) {}
    }
    m0_audio_init();
    deadline = m0_micros();
    __asm__ volatile("" : : "r"(version));
    for (;;) {
        machine.screen = skip_video ? NULL : fb;
        md_full_frame(&machine, 1);   /* native regions, interpreter fallback */
        if (machine.fault) {
            m0_lcd_fill(M0_COLOR(0x00800000));
            for (;;) { m0_watchdog_feed(); delay(1000); }
        }
        m0_audio_frame();
        if (!skip_video) push_frame();
        deadline += 1000000u / 60u;
        const uint32_t now = m0_micros();
        if ((int32_t)(now - deadline) > 0) {
            /* late: hold the video back, but always draw one frame in four —
             * the picture must never stick to nothing when the core crawls */
            deadline = now;
            skip_video = (skipped_run++ % 4u) != 3u;
        } else {
            skip_video = 0;
            skipped_run = 0;
        }
        const uint8_t mb = esp32_buttons();
        unsigned pad = 0;
        if (mb & M0_MB_UP) pad |= 1;
        if (mb & M0_MB_DOWN) pad |= 2;
        if (mb & M0_MB_LEFT) pad |= 4;
        if (mb & M0_MB_RIGHT) pad |= 8;
        if (mb & M0_MB_A) pad |= 16;
        if (mb & M0_MB_B) pad |= 32;
        if (mb & M0_MB_HOME) pad |= 64;    /* MENU/C */
        if (mb & M0_MB_MENU) pad |= 128;   /* RUN/D: Start */
        machine.pad = (uint8_t)pad;
        m0_watchdog_feed();
    }
}
