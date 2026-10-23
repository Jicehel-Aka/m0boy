#ifndef ESPBOY_HW_H
#define ESPBOY_HW_H
#include <stdint.h>
#ifdef __cplusplus
#include <Arduino.h>
int espboy_main(void); /* meta_main.cpp, called by setup() */
extern "C" {
#endif
uint8_t espboy_buttons(void);
void espboy_restart(void);
unsigned espboy_free_stack(void);
uintptr_t espboy_stack_floor(void);
void m0_watchdog_feed(void);
/* Sega frontends: cartridge RAM kept in flash (espboy_flash.cpp). */
void espboy_ram_load(uint8_t *ram, uint32_t bytes);
void espboy_ram_tick(const uint8_t *ram, uint32_t bytes, uint32_t frame);
void espboy_ram_flush(const uint8_t *ram, uint32_t bytes);
#ifdef __cplusplus
}
#endif
#endif
