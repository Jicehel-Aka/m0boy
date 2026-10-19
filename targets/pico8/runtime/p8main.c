#include "p8main.h"
#include "p8_audio.h"
#include <string.h>

/* the driver tells the rate it plays at before the cartridge starts */
unsigned p8_audio_rate_hint = 22050;

static void call_cb(Val *cb) {
    if (cb->t == P8_NIL) return;
    p8_top = p8_stack;
    p8_stack[0] = *cb;
    p8_call(p8_stack, 0);
}

void p8_start(void) {
    p8_heap_init();
    memset(p8_screen, 0, sizeof p8_screen);
    p8_gfx_reset();
    p8_input_reset();
    p8_input_update(0);
    p8_seed_rng(0);
    p8_audio_init(p8_audio_rate_hint);
    p8_fps = 30;
    p8_top = p8_stack;
    p8_gen_run_main();
    if (p8_cb_update60->t != P8_NIL) p8_fps = 60;
    call_cb(p8_cb_init);
}

void p8_frame(uint16_t buttons) {
    call_cb(p8_fps == 60 ? p8_cb_update60 : p8_cb_update);
    call_cb(p8_cb_draw);
    p8_input_update(buttons);
    p8_frames++;
}
