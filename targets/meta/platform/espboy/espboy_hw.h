#ifndef ESPBOY_HW_H
#define ESPBOY_HW_H
#include <stdint.h>
#ifdef __cplusplus
#include <Arduino.h>
extern "C" {
#endif
uint8_t espboy_buttons(void);
void espboy_restart(void);
unsigned espboy_free_stack(void);
void meta_watchdog_feed(void);
#ifdef __cplusplus
}
#endif
#endif
