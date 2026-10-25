/* Gamebuino AKA CORE v1.4: official wiring, i80 LCD, SDMMC and TAS2505. */
#ifndef AKA_HW_H
#define AKA_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned long millis(void); /* the Arduino core's own signature */
void delay(uint32_t ms);
void m0_watchdog_feed(void);

/* Pressed buttons in m0_menu.h's M0_MB_* order: D-pad, A, B, Home, Menu. */
uint8_t esp32_buttons(void);
void esp32_restart(void);

#ifdef __cplusplus
}
#endif

#endif
