#include "pc_frontend.h"
#include <stdio.h>
#include <stdlib.h>

enum { RAW, PIXEL, DMG, LCD, SCAN, GRID, FILTER_COUNT };
static const char *const filter_name[FILTER_COUNT] = {
    "none", "pixel", "Game Boy DMG", "LCD RGB", "scanlines", "grid"
};

static void open_pad(PcFrontend *pc, int index) {
    if (!pc->pad && SDL_IsGameController(index))
        pc->pad = SDL_GameControllerOpen(index);
    else if (!pc->pad && !pc->joystick && !SDL_IsGameController(index))
        pc->joystick = SDL_JoystickOpen(index);
}

int pc_frontend_open(PcFrontend *pc, const char *title, unsigned width, unsigned height) {
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL gamepad: %s\n", SDL_GetError());
        return 0;
    }
    pc->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  (int)width * 3, (int)height * 3, SDL_WINDOW_RESIZABLE);
    pc->renderer = pc->window ? SDL_CreateRenderer(pc->window, -1, 0) : NULL;
    if (!pc->renderer) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        pc_frontend_close(pc);
        return 0;
    }
    SDL_RenderSetLogicalSize(pc->renderer, (int)width, (int)height);
    SDL_RenderSetIntegerScale(pc->renderer, SDL_TRUE);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) open_pad(pc, i);
    return 1;
}

void pc_frontend_poll(PcFrontend *pc, int escape_quits) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT ||
            (escape_quits && ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
            pc->quit = 1;
        else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F8) {
            pc->filter = (pc->filter + 1u) % FILTER_COUNT;
            fprintf(stderr, "filter: %s (F8)\n", filter_name[pc->filter]);
        } else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_F11) {
            SDL_SetWindowFullscreen(pc->window, SDL_GetWindowFlags(pc->window) & SDL_WINDOW_FULLSCREEN_DESKTOP ?
                                    0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_JOYDEVICEADDED)
            open_pad(pc, ev.jdevice.which);
        else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && pc->pad && !SDL_GameControllerGetAttached(pc->pad)) {
            SDL_GameControllerClose(pc->pad);
            pc->pad = NULL;
            for (int i = 0; i < SDL_NumJoysticks() && !pc->pad; ++i) open_pad(pc, i);
        } else if (ev.type == SDL_JOYDEVICEREMOVED && pc->joystick && !SDL_JoystickGetAttached(pc->joystick)) {
            SDL_JoystickClose(pc->joystick);
            pc->joystick = NULL;
            for (int i = 0; i < SDL_NumJoysticks() && !pc->joystick && !pc->pad; ++i) open_pad(pc, i);
        }
    }
}

uint16_t pc_frontend_buttons(const PcFrontend *pc) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = (k[SDL_SCANCODE_UP] ? PC_UP : 0) |
        (k[SDL_SCANCODE_DOWN] ? PC_DOWN : 0) |
        (k[SDL_SCANCODE_LEFT] ? PC_LEFT : 0) |
        (k[SDL_SCANCODE_RIGHT] ? PC_RIGHT : 0) |
        (k[SDL_SCANCODE_X] ? PC_A : 0) | (k[SDL_SCANCODE_Z] ? PC_B : 0) |
        (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_A] ? PC_C : 0) | (k[SDL_SCANCODE_S] ? PC_D : 0) |
        (k[SDL_SCANCODE_Q] ? PC_L : 0) | (k[SDL_SCANCODE_E] ? PC_R : 0) |
        (k[SDL_SCANCODE_RETURN] ? PC_START : 0) |
        (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE] ||
         k[SDL_SCANCODE_ESCAPE] ? PC_SELECT : 0);
    if (pc->pad && SDL_GameControllerGetAttached(pc->pad)) {
        const int x = SDL_GameControllerGetAxis(pc->pad, SDL_CONTROLLER_AXIS_LEFTX);
        const int y = SDL_GameControllerGetAxis(pc->pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (x < -6000 || SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= PC_LEFT;
        if (x > 6000 || SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= PC_RIGHT;
        if (y < -6000 || SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= PC_UP;
        if (y > 6000 || SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= PC_DOWN;
        if (SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_B)) b |= PC_A;
        if (SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_A)) b |= PC_B;
        if (SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_X)) b |= PC_C;
        if (SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_Y)) b |= PC_D;
        if (SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= PC_L;
        if (SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= PC_R;
        if (SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_START)) b |= PC_START;
        if (SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_BACK) ||
            SDL_GameControllerGetButton(pc->pad, SDL_CONTROLLER_BUTTON_GUIDE)) b |= PC_SELECT;
    } else if (pc->joystick && SDL_JoystickGetAttached(pc->joystick)) {
        const int x = SDL_JoystickNumAxes(pc->joystick) ? SDL_JoystickGetAxis(pc->joystick, 0) : 0;
        const int y = SDL_JoystickNumAxes(pc->joystick) > 1 ? SDL_JoystickGetAxis(pc->joystick, 1) : 0;
        const Uint8 hat = SDL_JoystickNumHats(pc->joystick) ? SDL_JoystickGetHat(pc->joystick, 0) : 0;
        if (x < -6000 || (hat & SDL_HAT_LEFT)) b |= PC_LEFT;
        if (x > 6000 || (hat & SDL_HAT_RIGHT)) b |= PC_RIGHT;
        if (y < -6000 || (hat & SDL_HAT_UP)) b |= PC_UP;
        if (y > 6000 || (hat & SDL_HAT_DOWN)) b |= PC_DOWN;
        const uint16_t button_bits[] = {PC_A, PC_B, PC_C, PC_START | PC_D, PC_SELECT, PC_D};
        for (unsigned i = 0; i < sizeof button_bits / sizeof button_bits[0] &&
                             i < (unsigned)SDL_JoystickNumButtons(pc->joystick); ++i)
            if (SDL_JoystickGetButton(pc->joystick, i)) b |= button_bits[i];
    }
    return b;
}

static uint32_t shade(uint32_t c, unsigned percent) {
    return 0xff000000u | (((((c >> 16) & 255u) * percent / 100u) << 16) |
        ((((c >> 8) & 255u) * percent / 100u) << 8) | ((c & 255u) * percent / 100u));
}

static void filter_frame(const uint32_t *src, unsigned stride, uint32_t *dst,
                         unsigned width, unsigned height, unsigned filter) {
    static const uint32_t dmg[4] = {0xff0f380f, 0xff306230, 0xff8bac0f, 0xff9bbc0f};
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const uint32_t c = src[y * stride + x] | 0xff000000u;
        uint32_t body = c, edge = shade(c, 60);
        if (filter == DMG) {
            const unsigned lum = (((c >> 16) & 255u) * 616u + ((c >> 8) & 255u) * 604u +
                                  (c & 255u) * 224u) >> 11;
            body = dmg[lum >= 176u ? 3 : lum >= 120u ? 2 : lum >= 64u ? 1 : 0];
            edge = shade(body, 74);
        }
        for (unsigned j = 0; j < 3; ++j) for (unsigned i = 0; i < 3; ++i) {
            uint32_t v = body;
            if (filter == PIXEL || filter == DMG) v = i == 1 && j == 1 ? body : edge;
            else if (filter == GRID) v = i == 2 || j == 2 ? 0xff000000u : body;
            else if (filter == SCAN) v = j == 1 ? body : shade(body, 55);
            else if (filter == LCD) {
                const unsigned shift = 16u - 8u * i, mask = 255u << shift;
                const uint32_t dim = shade(body, j == 2 ? 11 : 25);
                const uint32_t full = shade(body, j == 2 ? 45 : 100);
                v = (full & mask) | (dim & ~mask) | 0xff000000u;
            }
            dst[(y * 3u + j) * (width * 3u) + x * 3u + i] = v;
        }
    }
}

int pc_frontend_present(PcFrontend *pc, const uint32_t *pixels,
                        unsigned width, unsigned height, unsigned stride) {
    if (!pc->renderer || !width || !height) return 0;
    const unsigned factor = pc->filter == RAW ? 1u : 3u;
    const unsigned tw = width * factor, th = height * factor;
    if (!pc->texture || tw != pc->texture_width || th != pc->texture_height) {
        if (pc->texture) SDL_DestroyTexture(pc->texture);
        pc->texture = SDL_CreateTexture(pc->renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, (int)tw, (int)th);
        pc->texture_width = tw;
        pc->texture_height = th;
        if (!pc->texture) return 0;
        SDL_SetTextureScaleMode(pc->texture, SDL_ScaleModeNearest);
    }
    if (pc->filter == RAW) SDL_UpdateTexture(pc->texture, NULL, pixels, (int)(stride * sizeof *pixels));
    else {
        const size_t bytes = (size_t)tw * th * sizeof *pc->filtered;
        if (width != pc->width || height != pc->height || !pc->filtered) {
            uint32_t *buffer = realloc(pc->filtered, bytes);
            if (!buffer) return 0;
            pc->filtered = buffer;
            pc->width = width;
            pc->height = height;
        }
        filter_frame(pixels, stride, pc->filtered, width, height, pc->filter);
        SDL_UpdateTexture(pc->texture, NULL, pc->filtered, (int)(tw * sizeof *pc->filtered));
    }
    SDL_RenderSetLogicalSize(pc->renderer, (int)tw, (int)th);
    SDL_RenderClear(pc->renderer);
    SDL_RenderCopy(pc->renderer, pc->texture, NULL, NULL);
    SDL_RenderPresent(pc->renderer);
    return 1;
}

void pc_frontend_close(PcFrontend *pc) {
    if (pc->joystick) SDL_JoystickClose(pc->joystick);
    if (pc->pad) SDL_GameControllerClose(pc->pad);
    if (pc->texture) SDL_DestroyTexture(pc->texture);
    if (pc->renderer) SDL_DestroyRenderer(pc->renderer);
    if (pc->window) SDL_DestroyWindow(pc->window);
    free(pc->filtered);
    pc->joystick = NULL;
    pc->pad = NULL;
    pc->texture = NULL;
    pc->renderer = NULL;
    pc->window = NULL;
    pc->filtered = NULL;
}
