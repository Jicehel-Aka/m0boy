/* PC backend, SDL2: the window plays the panel, keyboard/gamepad play the pad,
 * SDL_QueueAudio plays the speaker.  One C file behind the m0_lcd/m0_clock/
 * m0_audio/m0_watchdog interfaces the shared frontends call, after the model
 * of espboy_hw.cpp.  The panel geometry comes from the project's config
 * (PC_COLS/PC_ROWS: 160x144 Game Boy and Game Gear, 256x192 SG-1000/SMS). */
#include "pc_hw.h"
#include "pc_frontend.h"
#if __has_include("gbl.h") /* Game Boy frontend; the Sega ones have sg_audio.h */
#include "gbl.h"
#define PC_AUDIO_SET_RATE(r) gbl_apu_set_rate(r)
#define PC_AUDIO_RENDER(p, n) gbl_apu_render(p, n)
#else
#include "sg_audio.h"
#define PC_AUDIO_SET_RATE(r) sg_platform_audio_rate(r)
#define PC_AUDIO_RENDER(p, n) sg_platform_audio_render(p, n)
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "m0_audio.h"
#include "m0_clock.h"
#include "m0_lcd.h"
#include "m0_menu.h" /* M0_MB_*: the button bits pc_buttons returns */
#if __has_include("sg_config.h") /* SG/GG/SMS: PC_COLS/PC_ROWS live there */
#include "sg_config.h"
#endif
#include "m0_target.h" /* PC_COLS/PC_ROWS: this project's panel geometry */

/* The panel geometry comes from the project's config through m0_target.h
 * (PC_COLS/PC_ROWS: 160x128 Game Boy, 256x192 SG-1000/SMS, 160x144 Game
 * Gear) — never from defaults of our own. */
#define PC_AUDIO_RATE 11025u

static PcFrontend frontend;
static uint32_t frame_pixels[PC_ROWS * PC_COLS];
static uint32_t lcd_seq;
static unsigned frames_left;    /* M0_FRAMES: exit after that many frames */
static unsigned polls;          /* button reads while frames_left runs */
static const char *ppm_path;    /* M0_PPM: dump the last frame there */
static void dump_ppm(void);

void m0_watchdog_feed(void) {}
uint32_t millis(void) { return m0_micros() / 1000u; }
void delay(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---- clock --------------------------------------------------------------- */
static uint64_t clock_base;
void m0_clock_init(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    clock_base = (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}
uint8_t m0_clock_set(uint8_t mhz) { return mhz; } /* one rate on the PC */
uint8_t m0_clock_mhz(void) { return 240; }
uint32_t m0_micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u - clock_base);
}

/* ---- pad and events ------------------------------------------------------ */
uint8_t pc_buttons(void) {
    pc_frontend_poll(&frontend, 0);
    if (frontend.quit) exit(0);
    /* The boot-error loops never draw frames; under M0_FRAMES the run then
     * ends here instead, so a stuck firmware still exits on the CI. */
    if (frames_left && ++polls > frames_left * 10) {
        dump_ppm();
        exit(0);
    }
    const uint16_t k = pc_frontend_buttons(&frontend);
    uint8_t b = 0;
    if (k & PC_DOWN) b |= M0_MB_DOWN;
    if (k & PC_LEFT) b |= M0_MB_LEFT;
    if (k & PC_RIGHT) b |= M0_MB_RIGHT;
    if (k & PC_UP) b |= M0_MB_UP;
    if (k & PC_A) b |= M0_MB_A;
    if (k & PC_B) b |= M0_MB_B;
    if (k & PC_START) b |= M0_MB_MENU;
    if (k & PC_SELECT) b |= M0_MB_HOME;
    return b;
}

/* ---- display ------------------------------------------------------------- */
static void dump_ppm(void) {
    if (!ppm_path) return;
    FILE *f = fopen(ppm_path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)PC_COLS, (unsigned)PC_ROWS);
    for (unsigned i = 0; i < PC_ROWS * PC_COLS; i++) {
        uint8_t px[3] = {(uint8_t)(frame_pixels[i] >> 16), (uint8_t)(frame_pixels[i] >> 8),
                         (uint8_t)frame_pixels[i]};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

void m0_lcd_init(void) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    if (!pc_frontend_open(&frontend, "m0boy", PC_COLS, PC_ROWS)) exit(1);
    const char *frames = getenv("M0_FRAMES");
    if (frames) frames_left = (unsigned)strtoul(frames, NULL, 10);
    ppm_path = getenv("M0_PPM");
}
/* Native RGB565 rows (M0_COLOR keeps the host order) into the texture. */
uint32_t m0_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *p = (const uint16_t *)buf;
    for (unsigned y = row0; y < (unsigned)row0 + rows && y < PC_ROWS; y++)
        for (unsigned x = 0; x < PC_COLS; x++) {
            const uint16_t c = p[x];
            frame_pixels[y * PC_COLS + x] =
                0xFF000000u | ((c & 0xF800u) << 8) | ((c & 0x07E0u) << 5) | ((c & 0x001Fu) << 3);
        }
    return ++lcd_seq;
}
uint32_t m0_lcd_done_seq(void) { return lcd_seq; }
int m0_lcd_busy(void) { return 0; }
int m0_lcd_release(void) { return 1; }
void m0_lcd_wait(void) {}
void m0_lcd_fill(uint16_t color) {
    uint16_t line[PC_COLS];
    for (unsigned x = 0; x < PC_COLS; x++) line[x] = color;
    for (unsigned y = 0; y < PC_ROWS; y++) m0_lcd_send_rows(line, (uint8_t)y, 1);
}
void m0_lcd_end_frame(void) {
    pc_frontend_present(&frontend, frame_pixels, PC_COLS, PC_ROWS, PC_COLS);
    if (frames_left && !--frames_left) {
        dump_ppm();
        exit(0);
    }
}

/* ---- audio ---------------------------------------------------------------
 * Unsigned 8-bit PCM at 11025 Hz through SDL_QueueAudio, volume applied as
 * the samples join the queue (the ESPboy's ring in a queue's clothing). */
static SDL_AudioDeviceID audio;
static uint8_t volume = 4;
static uint32_t audio_frac;
static int paused;
void m0_audio_init(void) {
    SDL_AudioSpec want = {0}, have;
    want.freq = PC_AUDIO_RATE;
    want.format = AUDIO_U8;
    want.channels = 1;
    want.samples = 512;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    PC_AUDIO_SET_RATE(PC_AUDIO_RATE);
}
void m0_audio_frame(void) {
    uint8_t samples[192];
    unsigned n = m0_audio_frame_samples(PC_AUDIO_RATE, &audio_frac);
    if (n > sizeof samples) n = sizeof samples;
    PC_AUDIO_RENDER(samples, n);
    if (paused || !audio) return;
    for (unsigned i = 0; i < n; i++)
        samples[i] = (uint8_t)(128 + ((int)samples[i] - 128) * volume / 8);
    SDL_QueueAudio(audio, samples, n);
}
void m0_audio_idle(void) {}
void m0_audio_set_volume(uint8_t v) { volume = v > 8 ? 8 : v; }
uint8_t m0_audio_volume(void) { return volume; }
void m0_audio_stop(void) {
    if (audio) SDL_PauseAudioDevice(audio, 1);
}
void m0_audio_pause(int on) {
    paused = on;
    if (audio) SDL_PauseAudioDevice(audio, on ? 1 : 0);
}
