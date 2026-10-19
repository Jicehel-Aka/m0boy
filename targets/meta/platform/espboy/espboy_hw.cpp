/* ESPboy hardware adapter. No framebuffer, SD stack or WiFi connection.
 * MCP23017 at 0x20: buttons PA0..7, TFT chip select PB0.
 * MCP4725 at 0x60 controls the backlight. TFT: HSPI, DC GPIO15.
 * Wiring reference: ESPboy-edu/ESPboy_ArduinoIDE_Libraries. */
#include "espboy_hw.h"
#include <SPI.h>
#include <twi.h>
extern "C" {
#include <user_interface.h>
#include "gbl.h"
#include "meta_audio.h"
#include "meta_clock.h"
#include "meta_lcd.h"
#include "meta_sd.h"
}
#include "meta_menu.h"
#include "meta_save.h"

static void mcp_write(uint8_t reg, uint8_t value) {
    uint8_t bytes[] = {reg, value};
    twi_writeTo(0x20, bytes, 2, 1);
}
static uint8_t mcp_read(uint8_t reg) {
    uint8_t value = 0xff;
    if (twi_writeTo(0x20, &reg, 1, 0)) return 0xff;
    if (twi_readFrom(0x20, &value, 1, 1)) return 0xff;
    return value;
}
extern "C" uint8_t espboy_buttons(void) {
    uint8_t b = (uint8_t)~mcp_read(0x12), out = 0;
    if (b & 1) out |= META_MB_LEFT;
    if (b & 2) out |= META_MB_UP;
    if (b & 4) out |= META_MB_DOWN;
    if (b & 8) out |= META_MB_RIGHT;
    if (b & 16) out |= META_MB_A;
    if (b & 32) out |= META_MB_B;
    if (b & 64) out |= META_MB_HOME;
    if (b & 128) out |= META_MB_MENU;
    return out;
}
extern "C" void espboy_restart(void) { (*(volatile uint32_t *)0x60000700u) |= 0x80000000u; for (;;) {} }
extern "C" unsigned espboy_free_stack(void) { char here; return (uintptr_t)&here - 0x3fffc000u; }
extern "C" void meta_watchdog_feed(void) { yield(); }
extern "C" void meta_clock_init(void) {}
extern "C" uint8_t meta_clock_set(uint8_t mhz) {
    system_update_cpu_freq(mhz == 80 ? 80 : 160); return system_get_cpu_freq();
}
extern "C" uint8_t meta_clock_mhz(void) { return system_get_cpu_freq(); }
extern "C" uint32_t meta_micros(void) { return micros(); }

static void command(uint8_t c, const uint8_t *p = nullptr, unsigned n = 0) {
    digitalWrite(15, LOW); SPI.transfer(c); digitalWrite(15, HIGH);
    while (n--) SPI.transfer(*p++);
}
static void window(uint8_t y) {
    const uint8_t xaddr[] = {0, 2, 0, 129};
    const uint8_t yaddr[] = {0, (uint8_t)(y + 3), 0, (uint8_t)(y + 3)};
    command(0x2a, xaddr, 4); command(0x2b, yaddr, 4); command(0x2c);
}
extern "C" void meta_lcd_init(void) {
    twi_init(4, 5); twi_setClock(400000);
    mcp_write(0x00, 0xff); mcp_write(0x0c, 0xff); // input buttons, pullups
    mcp_write(0x15, 0); mcp_write(0x01, 0xfe); // PB0 output, TFT selected
    uint8_t backlight[] = {0x40, 0xff, 0xf0};
    twi_writeTo(0x60, backlight, 3, 1);
    pinMode(15, OUTPUT); digitalWrite(15, HIGH);
    SPI.begin(); SPI.beginTransaction(SPISettings(27000000, MSBFIRST, SPI_MODE0));
    command(0x01); delay(150); command(0x11); delay(150);
    const uint8_t colmod = 5, madctl = 0xc8;
    command(0x3a, &colmod, 1); command(0x36, &madctl, 1);
    command(0x13); command(0x29); delay(20);
}
static uint32_t seq;
extern "C" uint32_t meta_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *p = (const uint16_t *)buf;
    for (unsigned y = 0; y < rows && row0 + y < 128; y++, p += 160) {
        window(row0 + y);
        // Nearest-neighbour 5:4, covering the full width. Input is BE RGB565.
        for (unsigned x = 0; x < 128; x++) {
            uint16_t c = p[(x * 5u + 2u) / 4u];
            SPI.transfer((uint8_t)c); SPI.transfer((uint8_t)(c >> 8));
        }
    }
    return ++seq;
}
extern "C" uint32_t meta_lcd_done_seq(void) { return seq; }
extern "C" int meta_lcd_busy(void) { return 0; }
extern "C" int meta_lcd_release(void) { return 1; }
extern "C" void meta_lcd_wait(void) {}
extern "C" void meta_lcd_end_frame(void) {}
extern "C" void meta_lcd_fill(uint16_t c) {
    uint16_t row[160]; for (unsigned x = 0; x < 160; x++) row[x] = c;
    for (unsigned y = 0; y < 128; y++) meta_lcd_send_rows(row, y, 1);
}

// No card: shared frontend disables the state menu. Cartridge RAM remains
// usable in RAM; persistence is deliberately not advertised by this port.
extern "C" int meta_sd_init(void) { return 0; }
extern "C" int meta_sd_read(uint32_t, uint8_t *) { return 0; }
extern "C" int meta_sd_write(uint32_t, const uint8_t *) { return 0; }
extern "C" void meta_sd_deselect(void) {}
uint32_t meta_save_reads, meta_save_writes, meta_save_errors;
int meta_save_init(int) { return META_SAVE_NO_FILE; }
void meta_save_tick(void) {}
extern "C" int gbl_platform_save_read(uint32_t, uint8_t *) { return 0; }
extern "C" int gbl_platform_save_write(uint32_t, const uint8_t *) { return 0; }

// ESP8266 timer1 PWM, GPIO0 speaker. Ring contains unsigned PCM at 11025 Hz.
static volatile uint16_t audio_head, audio_tail;
static uint8_t audio_ring[1024];
static uint8_t volume = 4;
static bool paused;
static uint32_t audio_frac;
static void IRAM_ATTR audio_irq(void) {
    static uint8_t phase;
    static uint8_t sample = 128;
    if (!(phase++ & 3) && audio_tail != audio_head) {
        sample = audio_ring[audio_tail]; audio_tail = (audio_tail + 1) & 1023;
    }
    // First-order pulse density; 44.1 kHz carrier, no analogWrite timer conflict.
    static uint16_t error;
    error += sample;
    if (error >= 256) { error -= 256; GPOS = 1u << 0; }
    else GPOC = 1u << 0;
}
extern "C" void meta_audio_init(void) {
    pinMode(0, OUTPUT); gbl_apu_set_rate(11025);
    timer1_attachInterrupt(audio_irq); timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
    timer1_write(5000000u / 44100u);
}
extern "C" void meta_audio_frame(void) {
    uint8_t samples[185];
    unsigned n = meta_audio_frame_samples(11025, &audio_frac);
    gbl_apu_render(samples, n);
    if (paused) return;
    for (unsigned i = 0; i < n; i++) {
        uint16_t next = (audio_head + 1) & 1023;
        if (next == audio_tail) break;
        audio_ring[audio_head] = (uint8_t)(128 + ((int)samples[i] - 128) * volume / 8);
        audio_head = next;
    }
}
extern "C" void meta_audio_idle(void) { yield(); }
extern "C" void meta_audio_set_volume(uint8_t v) { volume = v > 8 ? 8 : v; }
extern "C" uint8_t meta_audio_volume(void) { return volume; }
extern "C" void meta_audio_stop(void) { timer1_disable(); digitalWrite(0, LOW); }
extern "C" void meta_audio_pause(int on) {
    paused = on; audio_tail = audio_head;
    if (on) meta_audio_stop(); else meta_audio_init();
}
