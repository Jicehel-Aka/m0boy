/* PC backend (SDL2): the console's drivers behind the shared m0_* interfaces,
 * like espboy_hw.h for the ESPboy.  The firmware is a plain executable; the
 * SD card is the file sd.img in the working directory (pc_sd.c). */
#ifndef PC_HW_H
#define PC_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pressed buttons in m0_menu.h's M0_MB_* order: arrows, X = A, Z = B,
 * Return = MENU (start), Escape or Backspace = HOME (select, held: menu). */
uint8_t pc_buttons(void);
/* The shared frontend's console helpers (meta_hw.h and pokitto_hw.h have
 * theirs): the PC's watch has no dog and its waits do sleep. */
uint32_t millis(void);
void delay(uint32_t ms);
void m0_watchdog_feed(void);

#ifdef __cplusplus
}
#endif

#endif
