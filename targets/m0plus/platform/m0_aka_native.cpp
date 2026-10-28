/* META firmware on the Gamebuino AKA: the translated Thumb code and the
 * SAMD21 board model (samd21.c) run on the ESP32-S3; no emulator. */
#include <Arduino.h>
#include <string.h>
#include "esp_heap_caps.h"
extern "C" {
#include "samd21.h"
#include "m0_audio.h"
#include "m0_clock.h"
#include "m0_lcd.h"
}
#include "aka_hw.h"
#include "m0_target.h"
extern "C" const uint8_t m0_rom[];
extern "C" const uint32_t m0_rom_size;
extern "C" int m0_native_step(M0Cpu *);
extern "C" uint32_t m0_stat_native, m0_stat_interp;

/* Sampling profiler: a task on the otherwise idle core reads the guest PC a
 * thousand times a second; the result (per 1 KB page of guest code) says where
 * the time really goes on this chip.  The PC is written back at page exits, so
 * a long loop inside one page is still counted on that page. */
#define M0_PROF_PAGES 256
static volatile uint32_t m0_prof[M0_PROF_PAGES + 1];
static volatile const uint32_t *m0_prof_pc;
static void m0_prof_task(void *) {
    for (;;) {
        uint32_t pc = *m0_prof_pc - M0_FLASH_BASE;
        uint32_t i = pc >> 10;
        m0_prof[i < M0_PROF_PAGES ? i : M0_PROF_PAGES]++;
        vTaskDelay(1);
    }
}
static void m0_prof_reset(void) { for (unsigned i = 0; i <= M0_PROF_PAGES; ++i) m0_prof[i] = 0; }
static void m0_prof_report(void) {
    uint32_t snap[M0_PROF_PAGES + 1], tot = 0;
    for (unsigned i = 0; i <= M0_PROF_PAGES; ++i) { snap[i] = m0_prof[i]; m0_prof[i] = 0; tot += snap[i]; }
    if (!tot) return;
    Serial.printf("[prof] %u samples; hottest guest pages (page = pc>>10, as in m0_page_XX):", tot);
    for (int k = 0; k < 10; ++k) {
        unsigned best = 0;
        for (unsigned i = 1; i <= M0_PROF_PAGES; ++i) if (snap[i] > snap[best]) best = i;
        if (!snap[best]) break;
        Serial.printf(" %x:%u%%", (unsigned)((M0_FLASH_BASE >> 10) + best), (unsigned)(100u * snap[best] / tot));
        snap[best] = 0;
    }
    Serial.println();
}

/* Boot-time calibration, printed once: what this chip really does. */
static void m0_bench(const Samd21 *b, const uint16_t *px) {
    volatile uint32_t sink = 0;
    uint32_t t = m0_micros();
    for (uint32_t i = 0; i < 2000000u; ++i) sink += i;
    uint32_t t_cpu = m0_micros() - t;
    t = m0_micros();
    for (uint32_t i = 0; i < 200000u; ++i) sink += b->m.ram[(i * 7u) & (sizeof b->m.ram - 1)];
    uint32_t t_ram = m0_micros() - t;
    t = m0_micros();
    for (uint32_t i = 0; i < 200000u; ++i) sink += m0_rom[(i * 61u) % m0_rom_size];
    uint32_t t_rom = m0_micros() - t;
    t = m0_micros();
    for (uint32_t i = 0; i < 200000u; ++i) sink += px[(i * 7u) % (SAMD21_W * SAMD21_H)];
    uint32_t t_pix = m0_micros() - t;
    Serial.printf("[bench] cpu %u MHz | 2M adds %u us | 200k reads: guest RAM %u us, ROM %u us, pixels %u us\n",
                  (unsigned)getCpuFrequencyMhz(), t_cpu, t_ram, t_rom, t_pix);
    Serial.printf("[bench] board @%p ram @%p pixels @%p rom @%p | heap %u, psram %u\n",
                  (const void *)b, (const void *)b->m.ram, (const void *)px, (const void *)m0_rom,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize());
}
static Samd21 board;                                    /* guest SRAM and the peripherals: internal RAM */
static uint16_t pixels[SAMD21_W * SAMD21_H];
static uint16_t rows[8][SAMD21_W] __attribute__((aligned(64)));
extern "C" void md_platform_audio_rate(unsigned rate) { samd21_audio_rate(&board, rate); }
extern "C" void md_platform_audio_render(uint8_t *out, unsigned n) { samd21_audio(&board, out, n); }
extern "C" int m0boy_console_main(void) {
    Serial.begin(115200);
    m0_lcd_init();
    /* The ROM image sits in the console's flash, whose cache refill costs ~2 us
     * a line (DIO, measured with [bench]); the game reads its data tables from
     * it all the time.  A copy in internal RAM (or PSRAM) makes those reads
     * 10-50x quicker. */
    const uint8_t *rom = m0_rom;
    if (uint8_t *copy = (uint8_t *)heap_caps_malloc(m0_rom_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        memcpy(copy, m0_rom, m0_rom_size); rom = copy;
    } else if (uint8_t *copy2 = (uint8_t *)ps_malloc(m0_rom_size)) {
        memcpy(copy2, m0_rom, m0_rom_size); rom = copy2;
    }
    if (samd21_init(&board, rom, m0_rom_size, M0_FLASH_BASE, pixels)) {
        m0_lcd_fill(M0_COLOR(0x00800000));
        for (;;) delay(1000);
    }
    m0_audio_init();
    m0_bench(&board, pixels);
    m0_prof_pc = &board.m.cpu.r[15];
    xTaskCreatePinnedToCore(m0_prof_task, "m0prof", 2048, nullptr, 1, nullptr, 0);
    Serial.printf("[bench] ROM read from %s\n", rom == m0_rom ? "flash" : "RAM copy");
    uint32_t next = m0_micros();
    /* Profiling: prints, once a second on the serial port (115200), where the
     * time goes (guest emulation / audio / screen), the frame rate reached and
     * the guest cycles per frame. */
    uint32_t t_btn = 0, t_emu = 0, t_aud = 0, t_lcd = 0, frames = 0, t_rep = m0_micros();
    uint64_t c_rep = board.m.cpu.cycles;
    for (;;) {
        uint32_t t0 = m0_micros();
        samd21_buttons(&board, esp32_buttons());
        uint32_t tb = m0_micros();
        t_btn += tb - t0;
        samd21_frame(&board, m0_native_step);
        uint32_t t1 = m0_micros();
        m0_audio_frame();
        uint32_t t2 = m0_micros();
        for (unsigned y0 = 0; y0 < SAMD21_H; y0 += 8) {
            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 0; x < SAMD21_W; ++x) {
                    /* the board keeps the META's BGR565, which is what this panel shows
                     * right with MADCTL's bit 3 clear (tester: swapped to RGB it came
                     * out BGR); the i80 bus swaps the bytes, aka_hw.cpp */
                    rows[y][x] = pixels[(y0 + y) * SAMD21_W + x];
                }
            }
            m0_lcd_send_rows(rows, (uint8_t)y0, 8);
        }
        uint32_t t3 = m0_micros();
        t_emu += t1 - tb; t_aud += t2 - t1; t_lcd += t3 - t2; ++frames;
        if (t3 - t_rep >= 1000000u) {
            Serial.printf("[perf] %u fps | per frame: buttons %u us, emu %u us, audio %u us, lcd %u us | "
                          "%u Mcycles/s guest, idle skipped %u Mc | native chains %u, interpreted %u per frame\n",
                          frames, t_btn / frames, t_emu / frames, t_aud / frames, t_lcd / frames,
                          (unsigned)((board.m.cpu.cycles - c_rep) / 1000000u),
                          (unsigned)(board.idle_skipped / 1000000u),
                          m0_stat_native / frames, m0_stat_interp / frames);
            m0_stat_native = m0_stat_interp = 0;
            { static unsigned n; if (++n % 5 == 0) m0_prof_report(); else if (n % 5 == 1) m0_prof_reset(); }
            Serial.printf("[heap] free %u min %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
            t_btn = t_emu = t_aud = t_lcd = frames = 0; t_rep = t3; c_rep = board.m.cpu.cycles;
        }
        next += 16743u;
        const uint32_t now = m0_micros();
        if ((int32_t)(next - now) > 0) delay((next - now) / 1000u);
        else next = now;
        m0_watchdog_feed();
    }
}
