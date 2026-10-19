/* ESPboy hardware adapter, on the Arduino core's normal start-up (the one
 * the earlier, working ESPboy emulator used).
 * MCP23017 at 0x20 (I2C SDA 4, SCL 5): buttons on port A, TFT chip select on
 * GPB0.  ST7735 128x128 on the hardware SPI (MOSI 13, SCK 14), DC on GPIO16.
 * MCP4725 at 0x60: backlight.  Speaker on GPIO0 through the sigma-delta
 * generator, fed at 11025 Hz by timer1. */
#include "espboy_hw.h"
#include <SPI.h>
#include <twi.h>
#include <esp8266_peri.h>
#include <sigma_delta.h>
extern "C" {
#include <user_interface.h>
#include <cont.h>
}
#if __has_include("gbl.h") /* Game Boy frontend; the Sega ones have sg_audio.h */
#include "gbl.h"
#include "meta_sd.h"
#define ESP_AUDIO_RATE(r) gbl_apu_set_rate(r)
#define ESP_AUDIO_RENDER(p, n) gbl_apu_render(p, n)
#define ESPBOY_GB 1
#else
#include "sg_audio.h"
#define ESP_AUDIO_RATE(r) sg_platform_audio_rate(r)
#define ESP_AUDIO_RENDER(p, n) sg_platform_audio_render(p, n)
#define ESPBOY_GB 0
#endif
extern "C" {
#include "meta_audio.h"
#include "meta_clock.h"
#include "meta_lcd.h"
}
#include "meta_menu.h"

/* GPIO_SIGMA_DELTA, as the core's sigma-delta driver defines it. */
#define GPSD ESP8266_REG(0x368)
#define GPSDT 0 /* target, 8 bits */

#define PIN_TFT_DC 16
#define PIN_SPEAKER 0
#define MCP_ADDR 0x20
#define MCP_IODIRA 0x00
#define MCP_IODIRB 0x01
#define MCP_GPPUA 0x0c
#define MCP_GPIOA 0x12
#define MCP_OLATB 0x15

static void mcp_write(uint8_t reg, uint8_t value) {
    uint8_t bytes[] = {reg, value};
    twi_writeTo(MCP_ADDR, bytes, 2, 1);
}
static uint8_t mcp_read(uint8_t reg) {
    uint8_t value = 0xff;
    if (twi_writeTo(MCP_ADDR, &reg, 1, 0)) return 0xff;
    if (twi_readFrom(MCP_ADDR, &value, 1, 1)) return 0xff; /* 0 = success */
    return value;
}

/* ESPboy (MCP23017 port A, low when pressed): 0 left, 1 up, 2 down, 3 right,
 * 4 ACT, 5 ESC, 6 and 7 the auxiliary buttons (6 = select, 7 = start, as in
 * the earlier emulator that ran on the console).  The frontends take A from
 * bit 5 (ESC) and B from bit 4 (ACT), as that emulator did, which had a swap
 * for them; the reference ESPboy emulator's key help calls bit 4 "A": swap the
 * two lines below to follow it. */
extern "C" uint8_t espboy_buttons(void) {
    uint8_t b = (uint8_t)~mcp_read(MCP_GPIOA), out = 0;
    if (b & 1) out |= META_MB_LEFT;
    if (b & 2) out |= META_MB_UP;
    if (b & 4) out |= META_MB_DOWN;
    if (b & 8) out |= META_MB_RIGHT;
    if (b & 16) out |= META_MB_B;
    if (b & 32) out |= META_MB_A;
    if (b & 64) out |= META_MB_HOME;
    if (b & 128) out |= META_MB_MENU;
    return out;
}
extern "C" void espboy_restart(void) { ESP.restart(); for (;;) {} }
/* The emulation runs in setup(), on the core's continuation stack. */
extern "C" uintptr_t espboy_stack_floor(void) { return (uintptr_t)g_pcont->stack + 256u; }
extern "C" unsigned espboy_free_stack(void) {
    char here;
    return (uintptr_t)&here - (uintptr_t)g_pcont->stack;
}
/* Software and hardware watchdogs are off (setup()). */
extern "C" void meta_watchdog_feed(void) {}
extern "C" void meta_clock_init(void) {}
extern "C" uint8_t meta_clock_set(uint8_t mhz) {
    system_update_cpu_freq(mhz == 80 ? 80 : 160);
    return system_get_cpu_freq();
}
extern "C" uint8_t meta_clock_mhz(void) { return system_get_cpu_freq(); }
extern "C" uint32_t meta_micros(void) { return micros(); }

#if ESPBOY_GB
/* No card: the shared frontend only needs these to say so. */
extern "C" int meta_sd_init(void) { return 0; }
extern "C" int meta_sd_read(uint32_t, uint8_t *) { return 0; }
extern "C" int meta_sd_write(uint32_t, const uint8_t *) { return 0; }
extern "C" void meta_sd_deselect(void) {}
#endif

/* ---- display ------------------------------------------------------------ */
static void command(uint8_t c, const uint8_t *p = nullptr, unsigned n = 0) {
    digitalWrite(PIN_TFT_DC, LOW);
    SPI.transfer(c);
    digitalWrite(PIN_TFT_DC, HIGH);
    while (n--) SPI.transfer(*p++);
}
#define CMD(c, ...) do { static const uint8_t d_[] = {__VA_ARGS__}; command(c, d_, sizeof d_); } while (0)
static uint8_t next_row = 0xff; /* row the pixel stream continues at */
static void window(uint8_t y) {
    const uint8_t xaddr[] = {0, 2, 0, 129};
    const uint8_t yaddr[] = {0, (uint8_t)(y + 3), 0, (uint8_t)(128 + 2)};
    command(0x2a, xaddr, 4);
    command(0x2b, yaddr, 4);
    command(0x2c);
    next_row = y;
}
extern "C" void meta_lcd_init(void) {
    twi_init(4, 5);
    twi_setClock(400000);
    mcp_write(MCP_IODIRA, 0xff);
    mcp_write(MCP_GPPUA, 0xff);
    mcp_write(MCP_IODIRB, 0xfe); /* GPB0 (TFT chip select) is an output */
    mcp_write(MCP_OLATB, 0x00);  /* ... and selects the panel */
    uint8_t backlight[] = {0x40, 0xff, 0xf0};
    twi_writeTo(0x60, backlight, 3, 1);
    pinMode(PIN_TFT_DC, OUTPUT);
    digitalWrite(PIN_TFT_DC, HIGH);
    SPI.begin();
    SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
    /* ST7735 initialisation (green tab 1.44", as Adafruit's INITR_144GREENTAB). */
    command(0x01);
    delay(150);
    command(0x11);
    delay(150);
    CMD(0xb1, 0x01, 0x2c, 0x2d);
    CMD(0xb2, 0x01, 0x2c, 0x2d);
    CMD(0xb3, 0x01, 0x2c, 0x2d, 0x01, 0x2c, 0x2d);
    CMD(0xb4, 0x07);
    CMD(0xc0, 0xa2, 0x02, 0x84);
    CMD(0xc1, 0xc5);
    CMD(0xc2, 0x0a, 0x00);
    CMD(0xc3, 0x8a, 0x2a);
    CMD(0xc4, 0x8a, 0xee);
    CMD(0xc5, 0x0e);
    command(0x20);
    CMD(0x36, 0xc8); /* MY | MX | BGR */
    CMD(0x3a, 0x05); /* 16 bits per pixel */
    CMD(0xe0, 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2b, 0x39, 0x00, 0x01, 0x03, 0x10);
    CMD(0xe1, 0x03, 0x1d, 0x07, 0x06, 0x2e, 0x2c, 0x29, 0x2d, 0x2e, 0x2e, 0x37, 0x3f, 0x00, 0x00, 0x02, 0x10);
    command(0x13);
    delay(10);
    command(0x29);
    delay(100);
    window(0);
}

static uint32_t seq;
/* Rows come in order from row 0 each frame; the window is reopened when they
 * do not.  160 -> 128 columns, nearest neighbour; the pixels are already
 * RGB565 big-endian in memory order. */
extern "C" uint32_t meta_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    static uint16_t line[128] __attribute__((aligned(4)));
    const uint16_t *p = (const uint16_t *)buf;
    for (unsigned y = 0; y < rows && row0 + y < 128; y++, p += 160) {
        if (next_row != row0 + y) window(row0 + y);
        for (unsigned x = 0; x < 128; x++) line[x] = p[(x * 5u + 2u) / 4u];
        SPI.writeBytes((const uint8_t *)line, sizeof line);
        next_row++;
    }
    if (next_row >= 128) next_row = 0xff;
    return ++seq;
}
extern "C" uint32_t meta_lcd_done_seq(void) { return seq; }
extern "C" int meta_lcd_busy(void) { return 0; }
extern "C" int meta_lcd_release(void) { return 1; }
extern "C" void meta_lcd_wait(void) {}
extern "C" void meta_lcd_end_frame(void) {}
extern "C" void meta_lcd_fill(uint16_t c) {
    static uint16_t row[160] __attribute__((aligned(4)));
    for (unsigned x = 0; x < 160; x++) row[x] = c;
    for (unsigned y = 0; y < 128; y++) meta_lcd_send_rows(row, y, 1);
}

/* ---- audio ---------------------------------------------------------------
 * Unsigned 8-bit PCM at 11025 Hz in a ring, played by timer1's interrupt
 * (IRAM only: it may run while flash is being erased) into the sigma-delta
 * generator of GPIO0. */
#define AUDIO_RING 1024
static volatile uint16_t audio_head, audio_tail;
static uint8_t audio_ring[AUDIO_RING];
static uint8_t volume = 4;
static bool paused;
static uint32_t audio_frac;
static void IRAM_ATTR audio_irq(void) {
    static uint8_t sample = 128;
    if (audio_tail != audio_head) {
        sample = audio_ring[audio_tail];
        audio_tail = (audio_tail + 1) & (AUDIO_RING - 1);
    }
    GPSD = (GPSD & ~(0xFFu << GPSDT)) | ((uint32_t)sample << GPSDT);
}
extern "C" void meta_audio_init(void) {
    audio_head = audio_tail = 0;
    ESP_AUDIO_RATE(11025);
    sigmaDeltaSetup(0, 312500);
    sigmaDeltaAttachPin(PIN_SPEAKER);
    sigmaDeltaEnable();
    sigmaDeltaWrite(0, 128);
    timer1_attachInterrupt(audio_irq);
    timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
    timer1_write(5000000u / 11025u);
}
extern "C" void meta_audio_frame(void) {
    uint8_t samples[192];
    unsigned n = meta_audio_frame_samples(11025, &audio_frac);
    if (n > sizeof samples) n = sizeof samples;
    ESP_AUDIO_RENDER(samples, n);
    if (paused) return;
    for (unsigned i = 0; i < n; i++) {
        uint16_t next = (audio_head + 1) & (AUDIO_RING - 1);
        if (next == audio_tail) break;
        audio_ring[audio_head] = (uint8_t)(128 + ((int)samples[i] - 128) * volume / 8);
        audio_head = next;
    }
}
extern "C" void meta_audio_idle(void) {}
extern "C" void meta_audio_set_volume(uint8_t v) { volume = v > 8 ? 8 : v; }
extern "C" uint8_t meta_audio_volume(void) { return volume; }
extern "C" void meta_audio_stop(void) {
    timer1_disable();
    sigmaDeltaWrite(0, 128);
}
extern "C" void meta_audio_pause(int on) {
    paused = on;
    audio_tail = audio_head;
    if (on) meta_audio_stop(); else meta_audio_init();
}

/* ---- entry ---------------------------------------------------------------
 * Everything runs inside setup(): no loop() iterations, so the watchdogs are
 * disabled as the earlier emulator did. */
void setup(void) {
    ESP.wdtDisable();
    *((volatile uint32_t *)0x60000900) &= ~1u;
    wifi_set_opmode_current(NULL_MODE);
    espboy_main();
}
void loop(void) {}
