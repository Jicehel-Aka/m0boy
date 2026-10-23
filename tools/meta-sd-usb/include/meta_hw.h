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
/* Mode 0, MSB first, at most hz.  Reconfigures only when the divider
 * changes. */
void meta_spi_rate(uint32_t hz);
/* The SERCOM's clock changed (meta_clock.cpp): hz, from now on. */
void meta_spi_clock(uint32_t hz);
uint8_t meta_spi_transfer(uint8_t out);
/* Pressed buttons: the pad's 74HC165 (CS PB03) read at META_BUTTONS_SPI_HZ,
 * active low, in the Gamebuino library bit order.  The SPI receive FIFO is
 * drained first; call it only when the bus is free. */
uint8_t meta_pad_read(void);

/* ---- faults and watchdog (meta_startup.c) -------------------------------- */
/* The last fault, kept across resets: exception frame (r0-r3, r12, lr, pc,
 * xpsr), exception number (3 HardFault, 18 the watchdog's early warning),
 * how many since the log was created, and the reset cause of this boot. */
typedef struct {
    uint32_t magic, count, rcause;
    uint32_t early; /* faults in a row within 5 s of boot: 3 stop the restarts */
    uint32_t frame[8];
    uint32_t exception;
} meta_fault_log;
extern volatile meta_fault_log meta_fault;
/* 4 s without a feed resets the console; the early warning at 2 s logs the
 * PC and restarts the game.  Feed once per frame and in long waits. */
void meta_watchdog_feed(void);
void meta_watchdog_stop(void);

#ifdef __cplusplus
}
#endif

#endif
