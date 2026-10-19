/* Read-only data (the cartridge image, the font, the trigonometry tables).
 * On the ESP8266 it must be put in the flash (PROGMEM: .rodata would use the
 * 80 KB of RAM) and, being there, read a whole aligned word at a time. */
#ifndef P8_ROM_H
#define P8_ROM_H
#include <stdint.h>
#ifdef ESP8266
#include <pgmspace.h>
#define P8_ROM PROGMEM __attribute__((aligned(4)))
static inline uint8_t p8_rom_u8(const uint8_t *p) {
    const uint32_t w = *(const uint32_t *)((uintptr_t)p & ~(uintptr_t)3);
    return (uint8_t)(w >> (((uintptr_t)p & 3u) * 8u));
}
static inline uint16_t p8_rom_u16(const uint16_t *p) {
    const uint32_t w = *(const uint32_t *)((uintptr_t)p & ~(uintptr_t)3);
    return (uint16_t)(w >> (((uintptr_t)p & 2u) * 8u));
}
#else
#define P8_ROM
#define p8_rom_u8(p) (*(p))
#define p8_rom_u16(p) (*(p))
#endif
#endif
