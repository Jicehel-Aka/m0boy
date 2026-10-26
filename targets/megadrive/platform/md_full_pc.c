#include "md_full.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 320, H = 224 };
typedef struct { unsigned frame, mask; } Event;
enum { FILTER_PIXEL, FILTER_DMG, FILTER_LCD, FILTER_SCAN, FILTER_GRID, FILTER_RAW, FILTER_COUNT };
static const char *const filter_names[] = {"pixel", "Game Boy DMG", "LCD RGB", "scanlines", "grid", "none"};
static SDL_GameController *pad;
static SDL_Joystick *joystick;

/* F8 follows gm0's filter order and 3x cell geometry. The source image stays
 * untouched for image_hash and --ppm; only the SDL texture is filtered. */
static uint32_t shade(uint32_t c, unsigned percent) {
    return 0xff000000u | (((((c >> 16) & 255u) * percent / 100u) << 16) |
        ((((c >> 8) & 255u) * percent / 100u) << 8) | ((c & 255u) * percent / 100u));
}
static void filter_frame(const uint32_t *src, uint32_t *dst, unsigned filter) {
    static const uint32_t dmg[4] = {0xff0f380f, 0xff306230, 0xff8bac0f, 0xff9bbc0f};
    for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
        const uint32_t c = src[y * W + x] | 0xff000000u;
        uint32_t body = c, edge = shade(c, 60);
        if (filter == FILTER_DMG) {
            unsigned lum = (((c >> 16) & 255u) * 616u + ((c >> 8) & 255u) * 604u +
                (c & 255u) * 224u) >> 11;
            body = dmg[lum >= 176u ? 3 : lum >= 120u ? 2 : lum >= 64u ? 1 : 0];
            edge = shade(body, 74);
        }
        for (unsigned j = 0; j < 3; ++j) for (unsigned i = 0; i < 3; ++i) {
            uint32_t v = body;
            if (filter == FILTER_PIXEL || filter == FILTER_DMG)
                v = (i == 1 && j == 1) ? body : edge;
            else if (filter == FILTER_GRID)
                v = (i == 2 || j == 2) ? 0xff000000u : body;
            else if (filter == FILTER_SCAN)
                v = j == 1 ? body : shade(body, 55);
            else if (filter == FILTER_LCD) {
                const unsigned channel = 16u - 8u * i;
                const uint32_t mask = 255u << channel;
                uint32_t dim = shade(body, j == 2 ? 11 : 25);
                uint32_t full = shade(body, j == 2 ? 45 : 100);
                v = (full & mask) | (dim & ~mask) | 0xff000000u;
            }
            dst[(y * 3u + j) * (W * 3u) + x * 3u + i] = v;
        }
    }
}
static void open_pad(int index) {
    if (!pad && SDL_IsGameController(index)) pad = SDL_GameControllerOpen(index);
    else if (!pad && !joystick && !SDL_IsGameController(index)) joystick = SDL_JoystickOpen(index);
}
static unsigned controller_keys(void) {
    unsigned b = 0;
    if (pad && SDL_GameControllerGetAttached(pad)) {
        const int dz = 6000;
        int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        b |= (x < -dz || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) ? 4u : 0u;
        b |= (x > dz || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) ? 8u : 0u;
        b |= (y < -dz || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) ? 1u : 0u;
        b |= (y > dz || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) ? 2u : 0u;
        b |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) ? 16u : 0u;
        b |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) ? 32u : 0u;
        b |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) ? 64u : 0u;
        b |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) ? 128u : 0u;
    } else if (joystick && SDL_JoystickGetAttached(joystick)) {
        const Uint8 hat = SDL_JoystickNumHats(joystick) ? SDL_JoystickGetHat(joystick, 0) : 0;
        const int x = SDL_JoystickNumAxes(joystick) ? SDL_JoystickGetAxis(joystick, 0) : 0;
        const int y = SDL_JoystickNumAxes(joystick) > 1 ? SDL_JoystickGetAxis(joystick, 1) : 0;
        b |= (x < -6000 || (hat & SDL_HAT_LEFT)) ? 4u : 0u;
        b |= (x > 6000 || (hat & SDL_HAT_RIGHT)) ? 8u : 0u;
        b |= (y < -6000 || (hat & SDL_HAT_UP)) ? 1u : 0u;
        b |= (y > 6000 || (hat & SDL_HAT_DOWN)) ? 2u : 0u;
        for (unsigned i = 0; i < 4 && i < (unsigned)SDL_JoystickNumButtons(joystick); ++i)
            if (SDL_JoystickGetButton(joystick, i)) b |= 16u << i;
    }
    return b;
}

static unsigned keys(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    return (k[SDL_SCANCODE_UP] ? 1u : 0u) | (k[SDL_SCANCODE_DOWN] ? 2u : 0u) |
        (k[SDL_SCANCODE_LEFT] ? 4u : 0u) | (k[SDL_SCANCODE_RIGHT] ? 8u : 0u) |
        (k[SDL_SCANCODE_X] ? 16u : 0u) | (k[SDL_SCANCODE_Z] ? 32u : 0u) |
        (k[SDL_SCANCODE_C] ? 64u : 0u) | (k[SDL_SCANCODE_RETURN] ? 128u : 0u);
}
static int read_events(const char *path, Event **out, size_t *count) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 0; }
    unsigned frame, previous = 0;
    int mask;
    while (fscanf(f, "%u %i", &frame, &mask) == 2) {
        if ((*count && frame <= previous) || mask < 0 || mask > 255) { fclose(f); return 0; }
        Event *p = realloc(*out, (*count + 1u) * sizeof **out);
        if (!p) { fclose(f); return 0; }
        *out = p; p[(*count)++] = (Event){frame, (unsigned)mask};
        previous = frame;
    }
    int okay = feof(f);
    fclose(f);
    return okay;
}
static int write_ppm(const char *path, const uint32_t *pixels) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (unsigned i = 0; i < W * H; ++i) {
        uint32_t p = pixels[i];
        fputc((p >> 16) & 255u, f); fputc((p >> 8) & 255u, f); fputc(p & 255u, f);
    }
    return fclose(f) == 0;
}
static void wav_header(FILE *f, uint32_t samples) {
    const uint32_t bytes = samples * 2u;
    const uint8_t h[44] = {'R','I','F','F', (uint8_t)(bytes + 36u), (uint8_t)((bytes + 36u) >> 8),
        (uint8_t)((bytes + 36u) >> 16), (uint8_t)((bytes + 36u) >> 24), 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0x44,0xac,0,0, 0x88,0x58,1,0,
        2,0, 16,0, 'd','a','t','a', (uint8_t)bytes, (uint8_t)(bytes >> 8),
        (uint8_t)(bytes >> 16), (uint8_t)(bytes >> 24)};
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}
static uint32_t image_hash(const uint32_t *pixels) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < W * H; ++i) {
        uint32_t p = pixels[i];
        for (unsigned b = 0; b < 3; ++b) { h = (h ^ (p & 255u)) * 16777619u; p >>= 8; }
    }
    return h;
}
static int save_coverage(const char *path, const MdFull *m) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 0; }
    for (uint32_t pc = 0; pc < md_rom_size; pc += 2)
        if (m->coverage[pc >> 3] & (1u << (pc & 7u))) fprintf(f, "%06x\n", pc);
    return fclose(f) == 0;
}
int main(int argc, char **argv) {
    unsigned limit = 0;
    int interp = 0;
    const char *ppm = NULL, *script = NULL, *coverage = NULL, *wav = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else { fprintf(stderr, "usage: md_pc [--frames N] [--input SCRIPT] [--ppm FILE] [--wav FILE] [--coverage FILE] [--interp]\n"); return 2; }
    }
    Event *events = NULL;
    size_t event_count = 0, event_at = 0;
    if (script && !read_events(script, &events, &event_count)) {
        fprintf(stderr, "invalid input script\n"); free(events); return 2;
    }
    MdFull *m = calloc(1, sizeof *m);
    uint32_t *pixels = malloc(W * H * sizeof *pixels);
    uint32_t *filtered = NULL;
    if (!m || !pixels) { free(m); free(pixels); free(events); return 1; }
    md_full_reset(m);
    if (coverage) {
        m->coverage_bytes = (md_rom_size + 7u) / 8u;
        m->coverage = calloc(m->coverage_bytes, 1);
        if (!m->coverage) { free(m); free(pixels); free(events); return 1; }
    }
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_AudioDeviceID audio_device = 0;
    FILE *wav_file = NULL;
    uint32_t wav_samples = 0;
    if (wav) {
        wav_file = fopen(wav, "wb+");
        if (!wav_file) { perror(wav); return 1; }
        wav_header(wav_file, 0);
    }
    if (!limit) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO)) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
        }
        window = SDL_CreateWindow("m0boy Mega Drive", SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED, W * 3, H * 3, SDL_WINDOW_RESIZABLE);
        renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
        filtered = malloc((size_t)W * H * 9u * sizeof *filtered);
        texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, W * 3, H * 3) : NULL;
        if (!texture || !filtered) { fprintf(stderr, "SDL display: %s\n", SDL_GetError()); return 1; }
        SDL_RenderSetLogicalSize(renderer, W * 3, H * 3);
        SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
        for (int i = 0; i < SDL_NumJoysticks(); ++i) open_pad(i);
        SDL_AudioSpec wanted = {0};
        wanted.freq = 44100; wanted.format = AUDIO_S16SYS; wanted.channels = 1; wanted.samples = 1024;
        audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
        if (audio_device) SDL_PauseAudioDevice(audio_device, 0);
        else fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
    }
    unsigned scripted = 0;
    int running = 1;
    uint64_t deadline = SDL_GetTicks64();
    unsigned frame_fraction = 0;
    unsigned filter = FILTER_PIXEL, scale = 0;
    while (running && !m->fault && (!limit || m->frames < limit)) {
        if (!limit) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) running = 0;
                else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F8) {
                    filter = (filter + 1u) % FILTER_COUNT;
                    fprintf(stderr, "filter: %s (F8)\n", filter_names[filter]);
                } else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F10) {
                    scale = (scale + 1u) % 3u;
                    SDL_RenderSetLogicalSize(renderer, scale == 2 ? 0 : W * 3, scale == 2 ? 0 : H * 3);
                    SDL_RenderSetIntegerScale(renderer, scale == 0 ? SDL_TRUE : SDL_FALSE);
                    SDL_SetTextureScaleMode(texture, scale == 2 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
                } else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F11) {
                    SDL_SetWindowFullscreen(window, SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP ?
                        0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_JOYDEVICEADDED) open_pad(ev.jdevice.which);
                else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && pad && !SDL_GameControllerGetAttached(pad)) {
                    SDL_GameControllerClose(pad); pad = NULL;
                } else if (ev.type == SDL_JOYDEVICEREMOVED && joystick && !SDL_JoystickGetAttached(joystick)) {
                    SDL_JoystickClose(joystick); joystick = NULL;
                }
            }
            if (!running) break;
        }
        while (event_at < event_count && events[event_at].frame == m->frames)
            scripted = events[event_at++].mask;
        m->pad = (uint8_t)(scripted | (limit ? 0u : keys() | controller_keys()));
        md_full_frame(m, !interp);
        int16_t pcm[735];
        md_full_audio_render(m, pcm, 735);
        if (wav_file) { fwrite(pcm, sizeof *pcm, 735, wav_file); wav_samples += 735; }
        if (audio_device && SDL_GetQueuedAudioSize(audio_device) < 44100u * sizeof *pcm / 4u)
            SDL_QueueAudio(audio_device, pcm, sizeof pcm);
        if (!limit && !m->fault) {
            md_full_render(m, pixels, W);
            filter_frame(pixels, filtered, filter);
            SDL_UpdateTexture(texture, NULL, filtered, W * 3 * sizeof *filtered);
            SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
            deadline += 16u;
            frame_fraction += 2u;
            if (frame_fraction >= 3u) { ++deadline; frame_fraction -= 3u; }
            uint64_t now = SDL_GetTicks64();
            if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
            else if (now - deadline > 100u) { deadline = now; frame_fraction = 0; }
        }
    }
    md_full_render(m, pixels, W);
    int okay = (!ppm || write_ppm(ppm, pixels)) && (!coverage || save_coverage(coverage, m));
    if (wav_file) { wav_header(wav_file, wav_samples); if (fclose(wav_file)) okay = 0; }
    printf("frames=%u pc=%06x fault=%u fault_pc=%06x opcode=%04x native_hits=%u fallback_hits=%u run_hash=%08x image_hash=%08x audio_hash=%08x\n",
        m->frames, m->pc, m->fault, m->fault_pc, m->last_opcode,
        m->native_hits, m->fallback_hits, md_full_hash(m), image_hash(pixels), m->audio_hash);
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (pad) SDL_GameControllerClose(pad);
    if (joystick) SDL_JoystickClose(joystick);
    if (!limit) SDL_Quit();
    const int failed = !okay || m->fault;
    free(m->coverage); free(m); free(pixels); free(filtered); free(events);
    return failed ? 1 : 0;
}
