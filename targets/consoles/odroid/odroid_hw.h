/* ODROID-GO (ESP32-WROVER, 16 MiB flash, 4 MiB PSRAM): the pins, the
 * buttons and the entry the shared ESP32 drivers call. */
#ifndef ODROID_HW_H
#define ODROID_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The shared frontend's console helpers (meta_hw.h has the META's): the
 * Arduino core's own clock functions - extern "C" exactly as the core
 * declares them (esp32-hal.h) - and no dog on this watch. */
unsigned long millis(void);
void delay(uint32_t ms);
void m0_watchdog_feed(void);

/* Pressed buttons in m0_menu.h's M0_MB_* order: A (IO32), B (IO33),
 * Start = MENU (IO39), Select or Menu = HOME (IO27, IO13); the d-pad is
 * two analog axes (IO34 x, IO35 y) read by thresholds. */
uint8_t esp32_buttons(void);
/* Back to the firmware's start (the menu's Quitter). */
void esp32_restart(void);

#ifdef __cplusplus
}
#endif

#endif
