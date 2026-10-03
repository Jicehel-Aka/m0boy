/*
 * Gamebuino META hardware layer shared by the META drivers and the
 * frontend's META sections (meta_main.cpp, meta_menu.cpp): bare metal on
 * the SAMD21's CMSIS headers, no Arduino core.
 */
#ifndef META_HW_H
#define META_HW_H

#include <stdint.h>

#include <sam.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- startup and time (meta_startup.c) ------------------------------------ */
uint32_t millis(void);   /* SysTick milliseconds, right at 48 and 72 MHz */
void delay(uint32_t ms); /* busy wait */

/* ---- SPI bus (meta_spi.c) ------------------------------------------------- */
/* SERCOM4: MISO PA12, MOSI PB10, SCK PB11, shared by the display, the SD
 * card and the buttons, each with its own chip select. */
void meta_spi_init(void);
/* Mode 0, MSB first, at most hz (rated for the 48 MHz clock: pass
 * meta_spi_hz(hz), meta_clock.h).  Reconfigures only when the rate
 * changes. */
void meta_spi_rate(uint32_t hz);
uint8_t meta_spi_transfer(uint8_t out);

#ifdef __cplusplus
}
#endif

#endif
