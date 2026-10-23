/*
 * Pokitto hardware layer shared by the Pokitto drivers and the frontend's
 * Pokitto sections (meta_main.cpp, meta_menu.cpp).
 */
#ifndef POKITTO_HW_H
#define POKITTO_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- clock (pokitto_clock.c) --------------------------------------------- */
void pokitto_clock_boot(void);       /* crystal, PLL at M0_CLOCK_DEFAULT, SysTick */
uint32_t millis(void);
void delay(uint32_t ms);
/* Called after a clock change, to rescale what follows the CPU clock. */
void pokitto_audio_clock_changed(uint32_t hz);
void pokitto_sd_clock_changed(uint32_t hz);

/* ---- buttons (pokitto_buttons.c) ----------------------------------------- */
enum {
    POKITTO_A = 0x01, POKITTO_B = 0x02, POKITTO_C = 0x04,
    POKITTO_UP = 0x08, POKITTO_DOWN = 0x10, POKITTO_LEFT = 0x20, POKITTO_RIGHT = 0x40,
};
void pokitto_buttons_init(void);
uint8_t pokitto_buttons(void);       /* POKITTO_* bits, 1 = pressed */
int pokitto_c_held_at_boot(void);    /* before .data/.bss exist */
void pokitto_enter_loader(void);     /* never returns (unless no loader: returns) */

/* ---- display (pokitto_lcd.c) ---------------------------------------------- */
/* The Game Boy picture is centred: columns 30..189, rows 16..159 (1:1). */
#define POKITTO_GB_X 30u
#define POKITTO_GB_Y 16u
void pokitto_lcd_row(unsigned row, const uint16_t *px); /* 160 pixels to picture row `row` */
/* Display modes (pokitto_display.c, the menu's "Ecran"). */
enum {
    POKITTO_MODE_1X,        /* 160x144 unscaled in the middle */
    POKITTO_MODE_ZOOM,      /* gm0's 200x176: 180 rows, two cut at each edge */
    POKITTO_MODE_ZOOM_FULL, /* 200x176 with every line: four lines not doubled */
    POKITTO_MODE_COUNT
};
/* gm0's enlarged picture: 200 columns from x = 10, panel row y (0..175). */
#define POKITTO_ZOOM_X 10u
void pokitto_lcd_row_zoom(unsigned y, const uint16_t *px);
/* The PICO-8 picture is centred: columns 46..173, rows 24..151 (1:1). */
#define POKITTO_P8_X 46u
#define POKITTO_P8_Y 24u
void pokitto_lcd_row128(unsigned row, const uint16_t *px); /* 128 pixels to picture row `row` (pokitto_p8lcd.c, the PICO-8 projects only) */
void pokitto_lcd_fill_all(uint16_t color);               /* whole 220x176 panel */
void pokitto_fault_screen(void);                         /* red screen, never returns */

#ifdef __cplusplus
}
#endif

#endif
