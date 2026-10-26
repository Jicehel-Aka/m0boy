#include "md_full.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 320, H = 224 };
typedef struct { unsigned frame, mask; } Event;

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
    const char *ppm = NULL, *script = NULL, *coverage = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else { fprintf(stderr, "usage: md_pc [--frames N] [--input SCRIPT] [--ppm FILE] [--coverage FILE] [--interp]\n"); return 2; }
    }
    Event *events = NULL;
    size_t event_count = 0, event_at = 0;
    if (script && !read_events(script, &events, &event_count)) {
        fprintf(stderr, "invalid input script\n"); free(events); return 2;
    }
    MdFull *m = calloc(1, sizeof *m);
    uint32_t *pixels = malloc(W * H * sizeof *pixels);
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
    if (!limit) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
        }
        window = SDL_CreateWindow("m0boy Mega Drive", SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED, W * 3, H * 3, SDL_WINDOW_RESIZABLE);
        renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
        texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, W, H) : NULL;
        if (!texture) { fprintf(stderr, "SDL display: %s\n", SDL_GetError()); return 1; }
        SDL_RenderSetLogicalSize(renderer, W, H);
    }
    unsigned scripted = 0;
    int running = 1;
    uint64_t deadline = SDL_GetTicks64();
    while (running && !m->fault && (!limit || m->frames < limit)) {
        if (!limit) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) running = 0;
            if (!running) break;
        }
        while (event_at < event_count && events[event_at].frame == m->frames)
            scripted = events[event_at++].mask;
        m->pad = (uint8_t)(scripted | (limit ? 0u : keys()));
        md_full_frame(m, !interp);
        if (!limit && !m->fault) {
            md_full_render(m, pixels, W);
            SDL_UpdateTexture(texture, NULL, pixels, W * sizeof *pixels);
            SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
            deadline += 1000u / 60u;
            uint64_t now = SDL_GetTicks64();
            if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
            else if (now - deadline > 100u) deadline = now;
        }
    }
    md_full_render(m, pixels, W);
    int okay = (!ppm || write_ppm(ppm, pixels)) && (!coverage || save_coverage(coverage, m));
    printf("frames=%u pc=%06x fault=%u fault_pc=%06x opcode=%04x native_hits=%u fallback_hits=%u run_hash=%08x image_hash=%08x\n",
        m->frames, m->pc, m->fault, m->fault_pc, m->last_opcode,
        m->native_hits, m->fallback_hits, md_full_hash(m), image_hash(pixels));
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (!limit) SDL_Quit();
    const int failed = !okay || m->fault;
    free(m->coverage); free(m); free(pixels); free(events);
    return failed ? 1 : 0;
}
