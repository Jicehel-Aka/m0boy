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
void pokitto_clock_boot(void);       /* crystal, PLL at META_CLOCK_DEFAULT, SysTick */
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
/* The Game Boy picture is centred: columns 30..189, rows 16..159. */
#define POKITTO_GB_X 30u
#define POKITTO_GB_Y 16u
void pokitto_lcd_row(unsigned row, const uint16_t *px); /* 160 pixels to picture row `row` */
void pokitto_lcd_fill_all(uint16_t color);               /* whole 220x176 panel */
void pokitto_fault_screen(void);                         /* red screen, never returns */

#ifdef __cplusplus
}
#endif

#endif
