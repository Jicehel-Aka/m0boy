/* PICO-8 drawing state and the 128x128 4-bit screen (two pixels per byte,
 * the low nibble is the left one). */
#ifndef P8GFX_H
#define P8GFX_H

#include <stdint.h>
#include "p8_rom.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P8_W 128
#define P8_H 128
#define P8_CART_BYTES 0x4300u

typedef struct P8Gfx {
    int16_t cam_x, cam_y;
    int16_t clip_x0, clip_y0, clip_x1, clip_y1; /* x1, y1 exclusive */
    int16_t cur_x, cur_y, margin;
    uint8_t pen;
    uint8_t pal_draw[16];   /* low nibble: colour; 0x10: transparent */
    uint8_t pal_screen[16]; /* applied when the frame is displayed */
    uint8_t pal_simple;     /* the draw palette is the default one: colour 0 transparent, the others as they are */
} P8Gfx;

extern P8Gfx p8g;
extern uint8_t p8_screen[(P8_W * P8_H) / 2];
/* sprites (0x0000), map (0x2000, rows 32-63 in 0x1000), flags (0x3000),
 * music (0x3100) and sound effects (0x3200) as the cartridge holds them;
 * generated, in flash */
extern const uint8_t p8_cart[P8_CART_BYTES];
extern const uint8_t p8_font_data[];

void p8_gfx_reset(void);
/* to call after the draw palette changed */
void p8_gfx_pal_changed(void);
void p8_gfx_cls(int color);
void p8_gfx_oval(int x0, int y0, int x1, int y1, int col, int fill);
int p8_gfx_sget(int x, int y);
int p8_gfx_print(const char *s, unsigned len, int x, int y, int col, int margin, int *out_x, int *out_y);
/* RGB of the 16 base colours, 0xRRGGBB */
extern const uint32_t p8_palette_rgb[16];

/* buttons: bit 0 left, 1 right, 2 up, 3 down, 4 O, 5 X */
extern uint16_t p8_buttons, p8_btnp;
extern uint32_t p8_frames;
extern unsigned p8_fps;
/* called once per frame after _draw with the buttons now held: updates
 * btnp (with its auto-repeat) and the frame counter */
void p8_input_update(uint16_t mask);
void p8_input_reset(void);
void p8_seed_rng(uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif
