/*
 * Target configuration of the gbl frontend.
 *
 * The frontend sources in this directory are shared by the consoles the
 * gbl runtime runs on; this header is the one place that tells them which
 * console they are built for.  gbrecomp writes GBL_TARGET_POKITTO into
 * gbl_config.h for --target pokitto; without it the build is for the
 * Gamebuino META.  Every console-dependent choice of the shared files is a
 * META_* switch below, tested with #if; the console's own drivers (display,
 * audio, SD card, clock, startup) live in separate files that only its
 * build compiles (platform/src for the META, platform/pokitto for the
 * Pokitto), behind the same headers (meta_lcd.h, meta_display.h,
 * meta_audio.h, meta_sd.h, meta_clock.h).
 *
 *   Gamebuino META   SAMD21 (Cortex-M0+), 32 KiB RAM, 160x128 ST7735 on
 *                    SPI with DMA, Arduino core, loader at 0x0000.
 *   Pokitto          LPC11U68 (Cortex-M0+), 32 + 2 + 2 KiB RAM, 220x176
 *                    ST7775 on a 16-bit parallel bus driven by GPIO, bare
 *                    metal (no mbed), loader at 0x39000.
 */
#ifndef META_TARGET_H
#define META_TARGET_H

#include "gbl_config.h"

#if defined(GBL_TARGET_POKITTO) && GBL_TARGET_POKITTO

#define META_TARGET_POKITTO 1
#define META_TARGET_NAME "pokitto"
#define META_ARDUINO 0     /* bare metal: startup, clock and drivers of our own */
#define META_USB_SERIAL 0  /* no USB stack: no status line */
#define META_LCD_ASYNC 0   /* the CPU writes every pixel: no transfer runs behind */
#define META_SCALED_MODES 0 /* no 144->128 modes: 1:1, or gm0's 200x176 (pokitto_display.c) */
#define META_SCREEN_ROWS 144 /* rows of the picture the menu draws on */
#define META_SWAP_AB_DEFAULT 0 /* Pokitto A (right) is Game Boy A already */
#define META_CLOCK_DEFAULT 72 /* PokittoLib's usual overclock (_OSCT=2); 48 in the menu */
/* Panel colours: native RGB565 on the 16-bit bus. */
#define META_COLOR(rgb) \
    ((uint16_t)((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)))

#else

#define META_TARGET_POKITTO 0
#define META_TARGET_NAME "meta"
#define META_ARDUINO 1
#define META_USB_SERIAL 1
#define META_LCD_ASYNC 1   /* strips go out by DMA while emulation continues */
#define META_SCALED_MODES 1 /* 144 lines onto 128: TBLEND, SMOOTH, ... */
#define META_SCREEN_ROWS 128
#define META_SWAP_AB_DEFAULT 1 /* the META's A is the right-hand button */
#define META_CLOCK_DEFAULT 48
/* Panel colours: RGB565 byte-swapped (the SPI sends the high byte first). */
#define META_COLOR(rgb)                                                                          \
    ((uint16_t)(((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) >> 8 | \
                ((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) << 8))

#endif

#endif
