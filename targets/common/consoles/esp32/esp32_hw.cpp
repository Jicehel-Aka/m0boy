/* The ESP32 family's shared drivers (ODROID-GO, Gamebuino AKA), behind the
 * same m0_* interfaces as the other consoles, after espboy_hw.cpp's model.
 * The panel plays 160-column rows M0_SCREEN_ROWS-tall, resampled onto the
 * 320x240 glass; the pins and the audio path come from the console's own
 * file (esp32_hw.h) and its esp32_config.h.  The emulation runs in setup(),
 * on the Arduino task's stack. */
#include <Arduino.h>
#include <SPI.h>
#include <esp_timer.h>
#include "esp32_hw.h"
#if __has_include("gbl.h") /* Game Boy frontend; the Sega ones have sg_audio.h */
#include "gbl.h"
#define ESP32_AUDIO_SET_RATE(r) gbl_apu_set_rate(r)
#define ESP32_AUDIO_RENDER(p, n) gbl_apu_render(p, n)
#else
#include "sg_audio.h"
#define ESP32_AUDIO_SET_RATE(r) sg_platform_audio_rate(r)
#define ESP32_AUDIO_RENDER(p, n) sg_platform_audio_render(p, n)
#endif
extern "C" {
#include "m0_audio.h"
#include "m0_clock.h"
#include "m0_lcd.h"
#include "m0_menu.h"
}
#include "m0_target.h"
#include "m0_scale.h" /* m0_nearest */

#include "esp32_config.h"

/* The frontend's stack-limit arithmetic wants the end of its statics
 * ("end", a linker symbol on the consoles it comes from); the ESP32's
 * scripts do not define one, so a byte above our own statics plays it. */
extern "C" {
char end[1] = {0};
}

/* ---- clock --------------------------------------------------------------- */
static uint8_t clock_mhz = 240;
extern "C" void m0_clock_init(void) {}
extern "C" uint8_t m0_clock_set(uint8_t mhz) {
    if (mhz != 80 && mhz != 160 && mhz != 240) return clock_mhz;
    setCpuFrequencyMhz(mhz);
    clock_mhz = mhz;
    return clock_mhz;
}
extern "C" uint8_t m0_clock_mhz(void) { return clock_mhz; }
extern "C" uint32_t m0_micros(void) { return (uint32_t)esp_timer_get_time(); }
extern "C" void m0_watchdog_feed(void) {}

/* ---- display -------------------------------------------------------------
 * 160 -> ESP32_PANEL_COLS columns (nearest of each row's centre) and
 * M0_SCREEN_ROWS -> ESP32_PANEL_ROWS rows: every incoming row lands on the
 * panel rows its band covers, streamed as it arrives like the ESPboy's.
 * The colours are RGB565 byte-swapped in memory order (M0_COLOR), which the
 * ILI9341/ST7789 want on the wire. */
#if !M0_TARGET_AKA
static SPIClass *spi;
void *esp32_lcd_bus(void) { return spi; }
static uint32_t lcd_seq;
static uint16_t next_row = 0xFFFF;
static uint16_t line[ESP32_PANEL_COLS] __attribute__((aligned(4)));

static void lcd_command(uint8_t c, const uint8_t *p = nullptr, unsigned n = 0) {
    esp32_lcd_dc(1);
    spi->transfer(c);
    esp32_lcd_dc(0);
    while (n--) spi->transfer(*p++);
}
#define LCD_CMD(c, ...) do { static const uint8_t d_[] = {__VA_ARGS__}; lcd_command(c, d_, sizeof d_); } while (0)

static void lcd_window(uint16_t y0) {
    const uint8_t x[4] = {0, 0, 0x01, 0x3F}; /* columns 0..319 */
    const uint8_t y[4] = {0, uint8_t(y0 >> 8), uint8_t(y0 & 0xFF),
                          uint8_t((ESP32_PANEL_ROWS - 1) >> 8)};
    /* the row window runs to the panel's end; 240 rows fit the low byte */
    lcd_command(0x2a, x, 4);
    lcd_command(0x2b, y, 4);
    lcd_command(0x2c);
    next_row = y0;
}

extern "C" void m0_lcd_init(void) {
    esp32_lcd_pins();
#if CONFIG_IDF_TARGET_ESP32S3
    spi = new SPIClass(FSPI); /* the S3 names its buses differently */
#else
    spi = new SPIClass(VSPI);
#endif
    spi->begin(ESP32_LCD_SCK, ESP32_LCD_MISO, ESP32_LCD_MOSI);
    spi->beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
    esp32_lcd_cs(1);
    esp32_lcd_cs(0);
    lcd_command(0x01); /* software reset */
    delay(150);
    lcd_command(0x11); /* sleep out */
    delay(150);
    LCD_CMD(0x3a, 0x55);             /* 16 bits per pixel */
    LCD_CMD(0x36, 0x48 | 0x08);      /* MX | BGR: the glass's wiring */
    lcd_command(0x21);               /* inversion on, as the panels of both like */
    lcd_command(0x29);               /* display on */
    delay(50);
    esp32_lcd_cs(1);
    next_row = 0xFFFF;
}
extern "C" uint32_t m0_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *p = (const uint16_t *)buf;
    esp32_lcd_cs(0);
    for (unsigned y = 0; y < rows; y++) {
        const unsigned src = row0 + y;
        if (src >= M0_SCREEN_ROWS) break;
        unsigned py = src * ESP32_PANEL_ROWS / M0_SCREEN_ROWS;
        const unsigned py_end = (src + 1) * ESP32_PANEL_ROWS / M0_SCREEN_ROWS;
        for (unsigned x = 0; x < ESP32_PANEL_COLS; x++)
            line[x] = p[m0_nearest(x, 160, ESP32_PANEL_COLS)];
        do {
            if (next_row != py) lcd_window(py);
            spi->writeBytes((const uint8_t *)line, sizeof line);
            next_row = uint16_t(py + 1);
            py++;
        } while (py < py_end);
    }
    esp32_lcd_cs(1);
    return ++lcd_seq;
}
extern "C" uint32_t m0_lcd_done_seq(void) { return lcd_seq; }
extern "C" int m0_lcd_busy(void) { return 0; }
extern "C" int m0_lcd_release(void) { return 1; }
extern "C" void m0_lcd_wait(void) {}
extern "C" void m0_lcd_end_frame(void) {}
extern "C" void m0_lcd_fill(uint16_t c) {
    for (unsigned x = 0; x < ESP32_PANEL_COLS; x++) line[x] = c;
    esp32_lcd_cs(0);
    for (unsigned py = 0; py < ESP32_PANEL_ROWS; py++) {
        if (next_row != py) lcd_window(py);
        spi->writeBytes((const uint8_t *)line, sizeof line);
        next_row = uint16_t(py + 1);
    }
    esp32_lcd_cs(1);
}

#endif /* !M0_TARGET_AKA: AKA uses its i80 panel driver. */

/* ---- audio ---------------------------------------------------------------
 * Unsigned 8-bit PCM at 11025 Hz in a ring the console's path drains (the
 * ODROID-GO's DAC from a timer interrupt, the AKA's I2S DMA); volume
 * applies as the samples join, like the ESPboy's ring. */
#define ESP32_AUDIO_RATE 11025u
static volatile uint16_t audio_head, audio_tail;
static uint8_t audio_ring[1024];
static uint8_t audio_volume = 4;
static bool audio_paused;
static uint32_t audio_frac;
static uint8_t audio_last = 128;

/* One sample off the ring (the interrupt/DAC path, esp32_hw.h), one at a
 * time. */
void esp32_audio_drain(void) {
    if (audio_tail != audio_head) {
        audio_last = audio_ring[audio_tail];
        audio_tail = (audio_tail + 1) & (sizeof audio_ring - 1);
    }
    esp32_audio_sample(audio_last);
}

/* The I2S path (AKA) pulls the same ring a buffer at a time. */
extern "C" void esp32_audio_pull(uint8_t *dst, unsigned n) {
    while (n--) {
        if (audio_tail != audio_head) {
            audio_last = audio_ring[audio_tail];
            audio_tail = (audio_tail + 1) & (sizeof audio_ring - 1);
        }
        *dst++ = audio_last;
    }
}
extern "C" void m0_audio_init(void) {
    audio_head = audio_tail = 0;
    ESP32_AUDIO_SET_RATE(ESP32_AUDIO_RATE);
    esp32_audio_start(ESP32_AUDIO_RATE);
}
extern "C" void m0_audio_frame(void) {
    uint8_t samples[192];
    unsigned n = m0_audio_frame_samples(ESP32_AUDIO_RATE, &audio_frac);
    if (n > sizeof samples) n = sizeof samples;
    ESP32_AUDIO_RENDER(samples, n);
    if (audio_paused) return;
    for (unsigned i = 0; i < n; i++) {
        uint16_t next = (audio_head + 1) & (sizeof audio_ring - 1);
        if (next == audio_tail) break;
        audio_ring[audio_head] = (uint8_t)(128 + ((int)samples[i] - 128) * audio_volume / 8);
        audio_head = next;
    }
}
extern "C" void m0_audio_idle(void) {}
extern "C" void m0_audio_set_volume(uint8_t v) { audio_volume = v > 8 ? 8 : v; }
extern "C" uint8_t m0_audio_volume(void) { return audio_volume; }
extern "C" void m0_audio_stop(void) { esp32_audio_stop(); }
extern "C" void m0_audio_pause(int on) {
    audio_paused = on;
    audio_tail = audio_head;
    if (on) esp32_audio_stop();
    else m0_audio_init();
}
