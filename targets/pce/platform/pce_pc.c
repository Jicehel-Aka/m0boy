/* PC display, keyboard and audio for the PCE runtime: the HuCard is
 * embedded in the project (pce_rom), SDL2 supplies the window and the
 * sound.  The run fingerprint follows the host harness's recipe, so a
 * scripted session here prints the harness's run_hash.  Keyboard:
 * arrows, X = I, Z = II, Enter = RUN, Shift = SELECT, Esc quits. */
#include "pce.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PCE_EMBEDDED
#include "pce_config.h"
extern const uint8_t pce_rom[];
#endif

#define AUDIO_RATE 11025u

static uint16_t rows[262][512];
static void on_row(void *host, unsigned line, const uint16_t *pixels, unsigned width) {
    (void)host;
    if (line < 262 && width <= 512)
        memcpy(rows[line], pixels, width * sizeof rows[0][0]);
}

static uint32_t fnv(const void *data, size_t n, uint32_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint8_t buttons(void) {
    const Uint8 *key = SDL_GetKeyboardState(NULL);
    return (uint8_t)((key[SDL_SCANCODE_UP] ? PCE_BTN_UP : 0) |
                     (key[SDL_SCANCODE_DOWN] ? PCE_BTN_DOWN : 0) |
                     (key[SDL_SCANCODE_LEFT] ? PCE_BTN_LEFT : 0) |
                     (key[SDL_SCANCODE_RIGHT] ? PCE_BTN_RIGHT : 0) |
                     (key[SDL_SCANCODE_X] ? PCE_BTN_I : 0) |
                     (key[SDL_SCANCODE_Z] ? PCE_BTN_II : 0) |
                     (key[SDL_SCANCODE_RETURN] ? PCE_BTN_RUN : 0) |
                     (key[SDL_SCANCODE_LSHIFT] || key[SDL_SCANCODE_RSHIFT] ? PCE_BTN_SELECT : 0));
}

int main(int argc, char **argv) {
    unsigned limit = 0;
    const char *ppm = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            limit = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else {
            fprintf(stderr, "usage: pce_pc [--frames N] [--ppm shot.ppm]\n");
            return 2;
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_Window *window = SDL_CreateWindow("m0boy PC Engine", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, 256 * 3,
                                          240 * 3, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    if (!renderer) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, 512, 262);
    if (!texture) {
        fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    /* the sound: the frame's samples queued as 16-bit signed PCM */
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);

    pce_init(pce_rom, PCE_ROM_SIZE);
    machine.row = on_row;

    int running = 1, scaled = 0;
    unsigned carry = 0;
    static int16_t pcm[4096];
    static uint32_t pixels[262 * 512];
    uint64_t deadline = SDL_GetTicks64();
    while (running && (!limit || machine.frames < limit)) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
                running = 0;
        if (!running) break;
        machine.input = buttons();
        pce_frame();
        if (audio) {
            carry += AUDIO_RATE * 119210u;
            unsigned count = carry / 7159090u;
            carry %= 7159090u;
            if (count > sizeof pcm / sizeof pcm[0]) count = sizeof pcm / sizeof pcm[0];
            uint8_t samples[sizeof pcm / 2];
            pce_audio(samples, count, AUDIO_RATE);
            for (unsigned i = 0; i < count; i++)
                pcm[i] = (int16_t)((samples[i] - 128) << 8);
            SDL_QueueAudio(audio, pcm, count * sizeof pcm[0]);
        }
        if (machine.have_frame) {
            const unsigned w = machine.vdc.active_width, h = machine.vdc.active_height;
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < w; x++) {
                    const uint16_t c = rows[machine.first_active_line + y][x];
                    pixels[y * 512 + x] = 0xff000000u |
                        ((((c >> 11) & 31u) * 255u / 31u) << 16) |
                        ((((c >> 5) & 63u) * 255u / 63u) << 8) |
                        ((c & 31u) * 255u / 31u);
                }
            const SDL_Rect src = {0, 0, (int)w, (int)h};
            SDL_UpdateTexture(texture, &src, pixels, 512 * sizeof pixels[0]);
            if (!scaled) {
                SDL_RenderSetLogicalSize(renderer, w, h);
                scaled = 1;
            }
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, &src, NULL);
            SDL_RenderPresent(renderer);
        }
        deadline += 1000u / 60u;
        uint64_t now = SDL_GetTicks64();
        if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
        else if (now - deadline > 100u) deadline = now;
    }
    const int saved = !ppm || (machine.have_frame && ({
        FILE *file = fopen(ppm, "wb");
        int ok = file != NULL;
        if (file) {
            fprintf(file, "P6\n%u %u\n255\n", machine.vdc.active_width, machine.vdc.active_height);
            for (unsigned y = 0; y < machine.vdc.active_height && ok; y++)
                for (unsigned x = 0; x < machine.vdc.active_width && ok; x++) {
                    const uint16_t c = rows[machine.first_active_line + y][x];
                    const uint8_t px[3] = {(uint8_t)(c >> 8 & 0xF8),
                                           (uint8_t)(c >> 3 & 0xFC), (uint8_t)(c << 3 & 0xF8)};
                    ok = fwrite(px, 1, 3, file) == 3;
                }
            ok &= fclose(file) == 0;
        }
        ok;
    }));
    uint32_t h = 2166136261u;
    const uint8_t regs[12] = {
        machine.cpu.a, machine.cpu.x, machine.cpu.y, machine.cpu.s, machine.cpu.p,
        (uint8_t)machine.cpu.pc, (uint8_t)(machine.cpu.pc >> 8),
        (uint8_t)machine.cpu.cycles, (uint8_t)(machine.cpu.cycles >> 8),
        (uint8_t)(machine.cpu.cycles >> 16), (uint8_t)(machine.cpu.cycles >> 24),
        (uint8_t)(machine.frames & 0xFFu),
    };
    h = fnv(regs, sizeof regs, h);
    h = fnv(machine.wram, sizeof machine.wram, h);
    h = fnv(machine.vdc.vram, sizeof machine.vdc.vram, h);
    h = fnv(machine.vdc.sat, sizeof machine.vdc.sat, h);
    h = fnv(machine.vce_colors, sizeof machine.vce_colors, h);
    if (machine.have_frame)
        for (unsigned y = 0; y < machine.vdc.active_height; y++)
            h = fnv(rows[machine.first_active_line + y],
                    machine.vdc.active_width * sizeof rows[0][0], h);
    printf("frames=%u pc=%04x run_hash=%08x\n", machine.frames, machine.cpu.pc, h);
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return !saved;
}
