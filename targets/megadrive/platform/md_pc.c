/* PC display and keyboard for the bounded Mega Drive profile.  The source
 * display is 320x224; this first VDP profile exposes CRAM background only. */
#include "md.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MD_WIDTH = 320, MD_HEIGHT = 224 };

static int save_ppm(const char *path, const uint32_t *pixels) {
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); return 0; }
    fprintf(file, "P6\n%d %d\n255\n", MD_WIDTH, MD_HEIGHT);
    int wrote = 1;
    for (unsigned i = 0; i < MD_WIDTH * MD_HEIGHT; ++i) {
        const uint8_t rgb[3] = {(uint8_t)(pixels[i] >> 16),
                                (uint8_t)(pixels[i] >> 8), (uint8_t)pixels[i]};
        if (fwrite(rgb, 1, sizeof rgb, file) != sizeof rgb) { wrote = 0; break; }
    }
    return fclose(file) == 0 && wrote;
}

static void draw(SDL_Renderer *renderer, SDL_Texture *texture, uint32_t *pixels,
                 uint16_t *lines, const Md *m) {
    md_render_rgb565(m, lines, MD_WIDTH);
    for (unsigned i = 0; i < MD_WIDTH * MD_HEIGHT; ++i) {
        const uint16_t colour = m->fault ? 0xf800u : lines[i];
        pixels[i] = 0xff000000u |
            ((((colour >> 11) & 31u) * 255u / 31u) << 16) |
            ((((colour >> 5) & 63u) * 255u / 63u) << 8) |
            ((colour & 31u) * 255u / 31u);
    }
    SDL_UpdateTexture(texture, NULL, pixels, MD_WIDTH * sizeof *pixels);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
}

static uint8_t buttons(void) {
    const Uint8 *key = SDL_GetKeyboardState(NULL);
    return (uint8_t)((key[SDL_SCANCODE_UP] ? MD_UP : 0) |
                     (key[SDL_SCANCODE_DOWN] ? MD_DOWN : 0) |
                     (key[SDL_SCANCODE_LEFT] ? MD_LEFT : 0) |
                     (key[SDL_SCANCODE_RIGHT] ? MD_RIGHT : 0) |
                     (key[SDL_SCANCODE_X] ? MD_A : 0) |
                     (key[SDL_SCANCODE_Z] ? MD_B : 0) |
                     (key[SDL_SCANCODE_C] ? MD_C : 0) |
                     (key[SDL_SCANCODE_RETURN] ? MD_START : 0));
}

int main(int argc, char **argv) {
    unsigned limit = 0;
    int interp = 0;
    const char *ppm = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            limit = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else {
            fprintf(stderr, "usage: md_pc [--frames N] [--ppm shot.ppm] [--interp]\n");
            return 2;
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_Window *window = SDL_CreateWindow("m0boy Mega Drive", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, MD_WIDTH * 3,
                                          MD_HEIGHT * 3, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    if (!renderer) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, MD_WIDTH, MD_HEIGHT);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, MD_WIDTH, MD_HEIGHT);
    uint32_t *pixels = (uint32_t *)malloc(MD_WIDTH * MD_HEIGHT * sizeof *pixels);
    uint16_t *lines = (uint16_t *)malloc(MD_WIDTH * MD_HEIGHT * sizeof *lines);
    if (!texture || !pixels || !lines) {
        fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
        free(pixels);
        free(lines);
        if (texture) SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    Md *m = (Md *)malloc(sizeof *m);
    if (!m) { free(lines); free(pixels); SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    md_reset(m);
    int running = 1;
    uint64_t deadline = SDL_GetTicks64();
    while (running && !m->fault && (!limit || m->frames < limit)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
                running = 0;
        }
        if (!running) break;
        m->pad = buttons();
        if (interp) md_run_interp(m, 4096);
        else md_run_native(m, 4096);
        ++m->frames;
        draw(renderer, texture, pixels, lines, m);
        SDL_RenderPresent(renderer);
        deadline += 1000u / 60u;
        uint64_t now = SDL_GetTicks64();
        if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
        else if (now - deadline > 100u) deadline = now;
    }
    draw(renderer, texture, pixels, lines, m);
    const int saved = !ppm || save_ppm(ppm, pixels);
    printf("frames=%u instructions=%u pc=%06x fault=%u bg=%04x run_hash=%08x\n",
           m->frames, m->instructions, m->pc, m->fault, md_background_rgb565(m), md_hash(m));
    const int result = m->fault || !saved;
    free(m);
    free(lines);
    free(pixels);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
