/* Mega Drive frontend for the ESP32 consoles (Gamebuino AKA, ODROID-GO):
 * the md_full machine sits in SRAM and there is no framebuffer — every VDP
 * line is rendered RGB565 straight into a small ring of 8-row batches that
 * the i80 DMA drains while the emulation goes on (the audio is decimated to
 * the family's 11025 Hz mono 8-bit ring).  Buttons: A, B, MENU/C = C,
 * RUN/D = Start, the joystick completes the D-pad. */
#include <string.h>
extern "C" {
#include "md_full.h"
#include "m0_audio.h"
#include "m0_clock.h"
#include "m0_lcd.h"
#include "m0_menu.h"
void m0_lcd_wide_begin(uint8_t row0, uint8_t rows);
void m0_lcd_wide_queue(const void *batch, uint8_t rows);
}
#include "m0_target.h"
#include "md_config.h"
#if __has_include("odroid_hw.h")
#include "odroid_hw.h"
#else
#include "aka_hw.h"
#endif

extern "C" const uint8_t md_rom[];
extern "C" const uint32_t md_rom_size;

static MdFull machine;

/* four 8-row batches; the DMA queue holds at most that many transfers, so
 * a batch is only reused once its transfer is done (the semaphore lives in
 * the console driver) */
#define RING_BATCHES 4
static uint16_t ring[RING_BATCHES][8][320] __attribute__((aligned(64)));

/* called at the end of every active line, inside md_full_frame: render the
 * line RGB565 into the ring, hand full batches to the DMA queue */
static void md_line_hook(void *, const MdFull *m, unsigned line) {
    uint16_t (*batch)[320] = ring[line >> 3];
    md_full_render_line565(m, line, batch[line & 7]);
    if ((line & 7u) == 7u) m0_lcd_wide_queue(batch, 8);
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
#if MD_VIDEO_ASM
    if (md_span_selftest()) {   /* the tile-run kernel diverged from the C */
        m0_lcd_fill(M0_COLOR(0x00c00000));
        for (;;) { m0_watchdog_feed(); delay(1000); }
    }
#endif
    machine.line_hook = md_line_hook;
    m0_audio_init();
    uint32_t deadline = m0_micros();
    for (;;) {
        machine.screen = NULL;
        m0_lcd_wide_begin(MD_PANEL_ROW0, MD_ROWS);
        md_full_frame(&machine, 1);   /* native regions, interpreter fallback */
        if (machine.fault) {
            m0_lcd_fill(M0_COLOR(0x00800000));
            for (;;) { m0_watchdog_feed(); delay(1000); }
        }
        m0_audio_frame();
        deadline += 1000000u / 60u;
        const uint32_t now = m0_micros();
        if ((int32_t)(now - deadline) > 0) deadline = now;   /* late: keep emulating */
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
