#include "pc_frontend.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s failed at line %d\n", #expr, __LINE__); return 1; } } while (0)

int main(void) {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0);
    PcFrontend pc = {0};
    CHECK(pc_frontend_open(&pc, "test", 2, 2));
    const uint32_t pixels[4] = {0xffffffffu, 0xff000000u, 0xff00ff00u, 0xffff0000u};
    CHECK(pc.filter == 0); /* no filter on startup */
    CHECK(pc_frontend_present(&pc, pixels, 2, 2, 2));
    CHECK(pc.texture_width == 2 && pc.texture_height == 2);
    SDL_Event event = {0};
    event.type = SDL_KEYUP;
    event.key.keysym.sym = SDLK_F8;
    CHECK(SDL_PushEvent(&event) == 1);
    pc_frontend_poll(&pc, 1);
    CHECK(pc.filter == 1);
    CHECK(pc_frontend_present(&pc, pixels, 2, 2, 2));
    CHECK(pc.texture_width == 6 && pc.texture_height == 6);
    void *texels;
    int pitch;
    CHECK(SDL_LockTexture(pc.texture, NULL, &texels, &pitch) == 0);
    const uint32_t *top = texels;
    const uint32_t *middle = (const uint32_t *)((const uint8_t *)texels + pitch);
    CHECK(top[0] != 0xffffffffu && middle[1] == 0xffffffffu);
    SDL_UnlockTexture(pc.texture);
    pc_frontend_close(&pc);
    SDL_Quit();
    return 0;
}
