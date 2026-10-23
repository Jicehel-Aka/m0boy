/*
 * PICO-8 output on the Pokitto's display (p8_main.cpp): the picture's
 * 128-wide rows pushed at their place on the panel.
 *
 * A file of its own rather than a function in pokitto_lcd.c: the Game Boy
 * and Sega firmwares must link that file's bytes unchanged, and a function
 * added to it moves the placement of its static helpers whatever
 * --gc-sections does.  The bus is theirs (same wiring, same write
 * sequences); only the position on the panel changes.
 */
#include "meta_lcd.h"

#include "lpc11u6x.h"
#include "meta_target.h"
#include "pokitto_hw.h"

#define WR_BIT (1u << 12) /* P1_12, data latched on the rising edge */
#define CD_BIT (1u << 2)  /* P0_2, 0 command 1 data */

/* The picture's row `row` (0..127): GRAM write from (POKITTO_P8_X,
 * POKITTO_P8_Y + row), register 0x20 takes the panel row, 0x21 the column,
 * 0x22 starts the pixels.  Register writes hold WR low two cycles, the
 * data one, as pokitto_lcd.c pushes them. */
void pokitto_lcd_row128(unsigned row, const uint16_t *px) {
    volatile uint32_t *mpin = &GPIO_MPIN(2), *clr = &GPIO_CLR(1), *set = &GPIO_SET(1);
    GPIO_CLR(0) = CD_BIT;
    *mpin = 0x20u << 3; *clr = WR_BIT; __asm__ volatile("nop\n\tnop"); *set = WR_BIT;
    GPIO_SET(0) = CD_BIT;
    *mpin = (uint32_t)(POKITTO_P8_Y + row) << 3; *clr = WR_BIT; __asm__ volatile("nop"); *set = WR_BIT;
    GPIO_CLR(0) = CD_BIT;
    *mpin = 0x21u << 3; *clr = WR_BIT; __asm__ volatile("nop\n\tnop"); *set = WR_BIT;
    GPIO_SET(0) = CD_BIT;
    *mpin = (uint32_t)POKITTO_P8_X << 3; *clr = WR_BIT; __asm__ volatile("nop"); *set = WR_BIT;
    GPIO_CLR(0) = CD_BIT;
    *mpin = 0x22u << 3; *clr = WR_BIT; __asm__ volatile("nop\n\tnop"); *set = WR_BIT;
    GPIO_SET(0) = CD_BIT;
    for (unsigned n = 0; n < 128; n++) {
        *mpin = (uint32_t)px[n] << 3;
        *clr = WR_BIT;
        __asm__ volatile("nop");
        *set = WR_BIT;
    }
}
