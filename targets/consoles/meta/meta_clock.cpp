/*
 * Clocks of the Gamebuino META: CPU at 48 or 70 MHz (the frontends' "72",
 * OC_MHZ below), display SPI at 12, 24, 35 or 48 MHz (the option menu's
 * "Processeur" and "SPI ecran", which says 36).
 *
 * 48 MHz: generator 0 runs from the DFLL48M as meta_startup.c sets it up.
 * 70 MHz: generator 5 divides the DFLL48M down to 1 MHz, the FDPLL96M
 * multiplies it by 70 and generator 0 (CPU) switches to it, with two flash
 * wait states instead of one.  This is past the SAMD21's 48 MHz rating; on
 * the META tried, code run from SRAM faulted there (GBL_PPU_RAMFUNC).  SysTick is
 * reloaded for 1 ms ticks at the new clock and m0_micros() scales its
 * fraction.  TC4/TC5 and the DAC stay on generator 4 (DFLL48M).
 *
 * SERCOM4 (the SPI shared by display, card and buttons, meta_spi.c) has
 * generator 6 to itself, independent of the CPU.  A synchronous SPI runs at
 * most at half its clock: the DFLL48M gives 12 and 24 MHz, the FDPLL96M at
 * 72 or 96 MHz gives 36 and 48 (past the SERCOM's 48 MHz clock rating;
 * gm0's overclocked META port ran the ST7735 at 36).  The PLL has one
 * frequency: with the CPU at 72 MHz the display gets 36 MHz at most.
 * meta_spi_rate derives the card's and the buttons' dividers from the
 * SERCOM clock, so they keep their rates.
 */
#include "m0_clock.h"

#include "meta_hw.h"
#include "m0_target.h"

#if META_LCD_SPI_MHZ != 12 && META_LCD_SPI_MHZ != 24 && META_LCD_SPI_MHZ != 36 && META_LCD_SPI_MHZ != 48
#error "META_LCD_SPI_MHZ: 12, 24, 36 or 48"
#endif

/* The overclock: the frontends ask for 72 (the Pokitto's rate); the META
 * runs 70.  Measured on a META (2026-10-17): 72 MHz faulted within the first
 * frames, 70 held for minutes once no code runs from SRAM
 * (GBL_PPU_RAMFUNC 0, gbl_profile.h); 70 is also what an earlier Game Boy
 * emulator ran on the META. */
#define OC_MHZ 70

static uint8_t clock_mhz = 48, lcd_mhz = 24, pll_mhz; /* pll_mhz 0: off */
static uint32_t frac_mul = 1048576u * 1000u / 48000u; /* 2^20 us per tick */

static void gclk_sync(void) {
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
}

static void route(uint16_t id, uint16_t gen) {
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(id) | gen | GCLK_CLKCTRL_CLKEN);
    gclk_sync();
}

static void gen_source(uint32_t id, uint32_t src) {
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(id) | src | GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
    gclk_sync();
}

static void systick_for(uint8_t mhz) {
    SystemCoreClock = (uint32_t)mhz * 1000000u;
    SysTick->LOAD = (uint32_t)mhz * 1000u - 1u;
    SysTick->VAL = 0;
    frac_mul = 1048576u * 1000u / ((uint32_t)mhz * 1000u);
}

static int pll_start(uint8_t mhz) {
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(5) | GCLK_GENDIV_DIV(48);
    gclk_sync();
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(5) | GCLK_GENCTRL_SRC_DFLL48M | GCLK_GENCTRL_GENEN;
    gclk_sync();
    route(GCLK_CLKCTRL_ID_FDPLL_Val, GCLK_CLKCTRL_GEN_GCLK5);
    SYSCTRL->DPLLCTRLA.reg = 0;
    SYSCTRL->DPLLRATIO.reg = SYSCTRL_DPLLRATIO_LDR(mhz - 1u) | SYSCTRL_DPLLRATIO_LDRFRAC(0); /* 1 MHz x mhz */
    SYSCTRL->DPLLCTRLB.reg = SYSCTRL_DPLLCTRLB_REFCLK(2);                               /* reference: GCLK */
    SYSCTRL->DPLLCTRLA.reg = SYSCTRL_DPLLCTRLA_ENABLE;
    const uint32_t ready = SYSCTRL_DPLLSTATUS_LOCK | SYSCTRL_DPLLSTATUS_CLKRDY;
    for (uint32_t n = 0; n < 1000000u; n++)
        if ((SYSCTRL->DPLLSTATUS.reg & ready) == ready) return 1;
    SYSCTRL->DPLLCTRLA.reg = 0;
    return 0;
}

/* CPU and display SPI clocks.  Nothing may be using the SPI bus. */
static void apply(uint8_t cpu, uint8_t lcd) {
    uint8_t pll = cpu == 72 || lcd == 36 ? OC_MHZ : lcd == 48 ? 96 : 0;
    if (cpu == 72 && lcd == 48) lcd = 36;
    int pll_change = pll != pll_mhz;
    if (clock_mhz == 72 && (cpu != 72 || pll_change)) {
        __disable_irq();
        gen_source(0, GCLK_GENCTRL_SRC_DFLL48M);
        NVMCTRL->CTRLB.bit.RWS = NVMCTRL_CTRLB_RWS_HALF_Val; /* after the clock came down */
        systick_for(48);
        __enable_irq();
        clock_mhz = 48;
    }
    if (pll_change || lcd <= 24) gen_source(6, GCLK_GENCTRL_SRC_DFLL48M);
    if (pll_change) {
        SYSCTRL->DPLLCTRLA.reg = 0;
        pll_mhz = pll && pll_start(pll) ? pll : 0;
        if (pll && !pll_mhz) { /* no lock: stay on the DFLL48M */
            cpu = 48;
            if (lcd > 24) lcd = 24;
        }
    }
    if (cpu == 72 && clock_mhz != 72) {
        __disable_irq();
        NVMCTRL->CTRLB.bit.RWS = 2; /* before the clock goes up */
        gen_source(0, GCLK_GENCTRL_SRC_FDPLL);
        systick_for(OC_MHZ);
        __enable_irq();
        clock_mhz = 72;
    }
    if (lcd > 24) gen_source(6, GCLK_GENCTRL_SRC_FDPLL);
    lcd_mhz = lcd;
    meta_spi_clock(lcd > 24 ? (uint32_t)pll_mhz * 1000000u : 48000000u);
}

void m0_clock_init(void) {
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(4) | GCLK_GENDIV_DIV(1);
    gclk_sync();
    gen_source(4, GCLK_GENCTRL_SRC_DFLL48M);
    route(GCLK_CLKCTRL_ID_TC4_TC5_Val, GCLK_CLKCTRL_GEN_GCLK4);
    route(GCLK_CLKCTRL_ID_DAC_Val, GCLK_CLKCTRL_GEN_GCLK4);
    apply(clock_mhz, META_LCD_SPI_MHZ);
}

uint8_t m0_clock_mhz(void) { return clock_mhz; }

uint8_t m0_clock_set(uint8_t mhz) {
    apply(mhz == 72 ? 72 : 48, lcd_mhz);
    return clock_mhz;
}

uint8_t m0_clock_lcd_spi(void) { return lcd_mhz; }

uint8_t m0_clock_set_lcd_spi(uint8_t mhz) {
    apply(clock_mhz, mhz >= 48 ? 48 : mhz >= 36 ? 36 : mhz >= 24 ? 24 : 12);
    return lcd_mhz;
}

/* Arduino's micros(), with the sub-millisecond part scaled for the actual
 * SysTick period. */
uint32_t m0_micros(void) {
    uint32_t ticks, ticks2, pend, pend2, count, count2;
    ticks2 = SysTick->VAL;
    pend2 = !!(SCB->ICSR & SCB_ICSR_PENDSTSET_Msk);
    count2 = millis();
    do {
        ticks = ticks2;
        pend = pend2;
        count = count2;
        ticks2 = SysTick->VAL;
        pend2 = !!(SCB->ICSR & SCB_ICSR_PENDSTSET_Msk);
        count2 = millis();
    } while (pend != pend2 || count != count2 || ticks < ticks2);
    return (count + pend) * 1000u + (((SysTick->LOAD - ticks) * frac_mul) >> 20);
}
