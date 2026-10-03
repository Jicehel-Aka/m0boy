/* CPU clock of the Gamebuino META: 48 MHz (DFLL48M, the default) or
 * 72 MHz (FDPLL96M, out of the SAMD21's specification). */
#ifndef META_CLOCK_H
#define META_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generator 4 = DFLL48M for TC4/TC5 (audio) and the DAC, so that they
 * keep their rate whatever the CPU runs at.  Call once in setup(). */
void meta_clock_init(void);
/* 48 or 72; returns the clock actually set (48 when the PLL does not lock). */
uint8_t meta_clock_set(uint8_t mhz);
uint8_t meta_clock_mhz(void);
/* Microseconds, right at either clock. */
uint32_t meta_micros(void);
/* Frequency to give meta_spi_rate so that the SPI runs at most at hz: the
 * SERCOM follows the CPU clock, meta_spi_rate computes its divider for
 * 48 MHz. */
uint32_t meta_spi_hz(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif
