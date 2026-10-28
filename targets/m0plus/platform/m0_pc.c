/* Desktop frontend of a recompiled META firmware: the translated Thumb code
 * (m0_native.c), the CPU (m0_cpu.c) and the SAMD21 board (samd21.c).
 *
 *   m0_firmware_pc [--frames N] [--ppm f] [--wav f] [--input f] [--random SEED]
 *                  [--profile prefix] [--interp] [--rate hz]
 *
 * With --frames the run is headless and prints run_hash/image_hash/audio_hash
 * (the same line whatever the SDL driver).  --profile writes <prefix>.cov
 * (executed instructions, the coverage a regeneration reads), <prefix>.mmio
 * (register accesses, UNMODELLED where the board has no model) and
 * <prefix>.hot (cycles per instruction, the poll loops first). */
#include "samd21.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M0_NO_SDL
#include <SDL.h>
#endif

extern const uint8_t m0_rom[];
extern const uint32_t m0_rom_size;
extern int m0_native_step(M0Cpu *);

static Samd21 board;
static uint16_t pixels[SAMD21_W * SAMD21_H];
static uint32_t *hits, *spent;

static uint32_t fnv(uint32_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}
static uint16_t rgb565(uint16_t c) { /* the board keeps the panel's BGR565 */
    return (uint16_t)((c >> 11) | (c & 0x07e0u) | ((c & 31u) << 11));
}
static void on_step(void *ctx, uint32_t pc, uint32_t cycles) {
    (void)ctx;
    uint32_t i = (pc - 0x4000u) >> 1;
    if (pc >= 0x4000u && i < m0_rom_size / 2) { hits[i]++; spent[i] += cycles; }
}
static void write_profile(const char *prefix) {
    char path[1024];
    snprintf(path, sizeof path, "%s.cov", prefix);
    FILE *f = fopen(path, "w");
    if (f) {
        for (uint32_t i = 0; i < m0_rom_size / 2; ++i)
            if (hits[i]) fprintf(f, "%x %u\n", 0x4000u + i * 2u, hits[i]);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s.mmio", prefix);
    if ((f = fopen(path, "w"))) { samd21_profile_mmio(&board, f); fclose(f); }
    snprintf(path, sizeof path, "%s.hot", prefix);
    if ((f = fopen(path, "w"))) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < m0_rom_size / 2; ++i) total += spent[i];
        fprintf(f, "# cycles per instruction, > 0.5%% of %llu; * = poll loop candidate\n",
                (unsigned long long)total);
        for (uint32_t i = 0; i < m0_rom_size / 2; ++i)
            if (total && spent[i] * 200ull > total)
                fprintf(f, "%x cycles=%u hits=%u share=%.1f%%\n", 0x4000u + i * 2u, spent[i], hits[i],
                        100.0 * spent[i] / (double)total);
        fclose(f);
    }
}
/* a script line "frame mask" (hex): the pad from that frame on */
static uint8_t scripted(const char *path, int frame, uint8_t held) {
    static FILE *f; static int loaded, at = -1; static unsigned mask;
    if (!loaded) { loaded = 1; f = fopen(path, "r"); if (f && fscanf(f, "%d %x", &at, &mask) != 2) at = -1; }
    while (f && at >= 0 && at <= frame) {
        held = (uint8_t)mask;
        if (fscanf(f, "%d %x", &at, &mask) != 2) at = -1;
    }
    return held;
}

int main(int argc, char **argv) {
    int frames = -1, interp = 0, window = 1;
    const char *ppm = 0, *wav = 0, *input = 0, *profile = 0;
    unsigned seed = 0, rate = 22050;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = atoi(argv[++i]); window = 0; }
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "--random") && i + 1 < argc) seed = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (argv[i][0] != '-') { /* a card image: not modelled yet */ }
        else { fprintf(stderr, "usage: %s [--frames N] [--ppm f] [--wav f] [--input f] [--random SEED] "
                               "[--profile prefix] [--interp]\n", argv[0]); return 2; }
    }
    if (samd21_init(&board, m0_rom, m0_rom_size, 0x4000u, pixels)) { fputs("cannot start the board\n", stderr); return 1; }
    if (profile) {
        hits = calloc(m0_rom_size / 2, 4); spent = calloc(m0_rom_size / 2, 4);
        samd21_profile_start(&board);
        board.step_hook = on_step;
    }
#ifndef M0_NO_SDL
    SDL_Window *win = 0; SDL_Renderer *ren = 0; SDL_Texture *tex = 0; SDL_AudioDeviceID dev = 0;
    if (window) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
        win = SDL_CreateWindow("META", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SAMD21_W * 4, SAMD21_H * 4, SDL_WINDOW_RESIZABLE);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(ren, SAMD21_W, SAMD21_H);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SAMD21_W, SAMD21_H);
        SDL_AudioSpec want = {0}, got;
        want.freq = (int)rate; want.format = AUDIO_U8; want.channels = 1; want.samples = 512;
        dev = SDL_OpenAudioDevice(0, 0, &want, &got, 0);
        if (dev) { rate = (unsigned)got.freq; SDL_PauseAudioDevice(dev, 0); }
    }
#endif
    samd21_audio_rate(&board, rate);
    FILE *wf = wav ? fopen(wav, "wb") : 0;
    uint32_t image = 0x811c9dc5u, audio = 0x811c9dc5u, frac = 0, rng = seed ? seed : 1u;
    uint8_t held = 0, random_held = 0;
    unsigned total_samples = 0;
    for (int frame = 0; frames < 0 || frame < frames; ++frame) {
        uint8_t pressed = held;
        if (input) pressed = scripted(input, frame, held), held = pressed;
        if (seed && frame % 12 == 0) {                      /* a random pad, held for a few frames */
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            random_held = (rng & 3u) ? (uint8_t)((rng >> 8) & 0x3fu) : 0u;
        }
        if (seed) pressed |= random_held;
#ifndef M0_NO_SDL
        if (window) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { frames = frame; break; }
                if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                    uint8_t bit = 0;
                    switch (e.key.keysym.sym) {
                    case SDLK_DOWN: bit = 1; break; case SDLK_LEFT: bit = 2; break;
                    case SDLK_RIGHT: bit = 4; break; case SDLK_UP: bit = 8; break;
                    case SDLK_j: bit = 16; break; case SDLK_k: bit = 32; break;
                    case SDLK_u: bit = 64; break; case SDLK_i: bit = 128; break;
                    case SDLK_ESCAPE: frames = frame; break;
                    }
                    if (e.type == SDL_KEYDOWN) held |= bit; else held &= (uint8_t)~bit;
                }
            }
            if (frames == frame) break;
            pressed = held;
        }
#endif
        samd21_buttons(&board, pressed);
        samd21_frame(&board, interp || profile ? 0 : m0_native_step);
        if (board.m.cpu.fault) { fprintf(stderr, "CPU fault at frame %d, pc=%08x\n", frame, board.m.cpu.r[15]); break; }
        frac += rate * 10000u;
        unsigned n = frac / 597275u; frac %= 597275u;
        uint8_t samples[2048];
        if (n > sizeof samples) n = sizeof samples;
        samd21_audio(&board, samples, n);
        total_samples += n;
        audio = fnv(audio, samples, n);
        if (wf) fwrite(samples, 1, n, wf);
        if (frames >= 0 && !window) image = fnv(image, pixels, sizeof pixels);
#ifndef M0_NO_SDL
        if (window) {
            uint16_t shown[SAMD21_W * SAMD21_H];
            for (unsigned i = 0; i < SAMD21_W * SAMD21_H; ++i) shown[i] = rgb565(pixels[i]);
            SDL_UpdateTexture(tex, 0, shown, SAMD21_W * 2);
            SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, 0, 0); SDL_RenderPresent(ren);
            if (dev) {
                while (SDL_GetQueuedAudioSize(dev) > rate / 8) SDL_Delay(1);
                SDL_QueueAudio(dev, samples, n);
            }
        }
#endif
    }
    if (wf) fclose(wf);
    if (ppm) {
        FILE *f = fopen(ppm, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", SAMD21_W, SAMD21_H);
            for (unsigned i = 0; i < SAMD21_W * SAMD21_H; ++i) {
                uint16_t c = rgb565(pixels[i]);
                fputc(((c >> 11) & 31u) << 3, f); fputc(((c >> 5) & 63u) << 2, f); fputc((c & 31u) << 3, f);
            }
            fclose(f);
        }
    }
    if (profile) write_profile(profile);
    uint32_t run = fnv(0x811c9dc5u, board.m.cpu.r, sizeof board.m.cpu.r);
    run = fnv(run, board.m.ram, sizeof board.m.ram);
    printf("run_hash=%08x image_hash=%08x audio_hash=%08x cycles=%llu samples=%u idle_skipped=%llu pc=%08x fault=%u\n",
           run, image, audio, (unsigned long long)board.m.cpu.cycles, total_samples,
           (unsigned long long)board.idle_skipped, board.m.cpu.r[15], board.m.cpu.fault);
#ifndef M0_NO_SDL
    if (window) SDL_Quit();
#endif
    return board.m.cpu.fault ? 1 : 0;
}
