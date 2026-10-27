/* Experimental META firmware frontend for the AKA. */
#include <Arduino.h>
#include <esp_heap_caps.h>
extern "C" {
#include "gm0_embedded.h"
#include "m0_audio.h"
#include "m0_clock.h"
#include "m0_lcd.h"
#include "m0_sd.h"
}
#include "aka_hw.h"
#include "m0_target.h"
extern "C" const uint8_t m0_rom[];
extern "C" const uint32_t m0_rom_size;
static uint16_t rows[8][160] __attribute__((aligned(64)));
static unsigned audio_rate = 11025;
extern "C" void md_platform_audio_rate(unsigned rate) { audio_rate = rate; }
extern "C" void md_platform_audio_render(uint8_t *out, unsigned n) { gm0_embedded_audio(out, n, audio_rate); }
extern "C" int m0boy_console_main(void) {
    m0_lcd_init();
    auto *flash = static_cast<uint8_t *>(heap_caps_malloc(GM0_EMBEDDED_FLASH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(GM0_EMBEDDED_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!flash || !pixels || gm0_embedded_init(m0_rom, m0_rom_size, GM0_EMBEDDED_META, flash, pixels)) {
        m0_lcd_fill(M0_COLOR(0x00800000));
        for (;;) delay(1000);
    }
    if (m0_sd_init()) gm0_embedded_sd_callbacks(m0_sd_read, m0_sd_write);
    m0_audio_init();
    uint32_t next = m0_micros();
    for (;;) {
        gm0_embedded_buttons(esp32_buttons());
        gm0_embedded_frame();
        m0_audio_frame();
        const uint16_t *screen = gm0_embedded_pixels();
        for (unsigned y0 = 0; y0 < 128; y0 += 8) {
            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 0; x < 160; ++x) {
                    uint16_t c = screen[(y0 + y) * 160u + x];
                    c = uint16_t(((c & 0xf800u) >> 11) | (c & 0x07e0u) | ((c & 31u) << 11));
                    rows[y][x] = uint16_t((c << 8) | (c >> 8));
                }
            }
            m0_lcd_send_rows(rows, (uint8_t)y0, 8);
        }
        next += 16743u;
        const uint32_t now = m0_micros();
        if ((int32_t)(next - now) > 0) delay((next - now) / 1000u);
        else next = now;
        m0_watchdog_feed();
    }
}
