#ifndef M0BOY_PC_FRONTEND_H
#define M0BOY_PC_FRONTEND_H

#include <SDL.h>
#include <stdint.h>

enum {
    PC_UP = 1u << 0, PC_DOWN = 1u << 1, PC_LEFT = 1u << 2,
    PC_RIGHT = 1u << 3, PC_A = 1u << 4, PC_B = 1u << 5,
    PC_C = 1u << 6, PC_START = 1u << 7, PC_SELECT = 1u << 8,
    PC_D = 1u << 9
};

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_GameController *pad;
    SDL_Joystick *joystick;
    uint32_t *filtered;
    unsigned width, height, texture_width, texture_height;
    unsigned filter, scale;
    int quit;
} PcFrontend;

/* SDL_Init must precede open. F8: none (default), pixel, DMG, LCD RGB,
 * scanlines, grid. F10: integer, fitted, stretched. F11: fullscreen. */
int pc_frontend_open(PcFrontend *pc, const char *title, unsigned width, unsigned height);
void pc_frontend_poll(PcFrontend *pc, int escape_quits);
uint16_t pc_frontend_buttons(const PcFrontend *pc);
int pc_frontend_present(PcFrontend *pc, const uint32_t *pixels,
                        unsigned width, unsigned height, unsigned stride);
void pc_frontend_close(PcFrontend *pc);

#endif
