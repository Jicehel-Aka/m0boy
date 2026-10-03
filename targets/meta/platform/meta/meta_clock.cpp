/*
 * CPU clock switch for the Gamebuino META.
 *
 * 48 MHz: generator 0 runs from the DFLL48M as the Arduino core sets it up.
 * 72 MHz: generator 5 divides the DFLL48M down to 1 MHz, the FDPLL96M
 * multiplies it by 72 and generator 0 (CPU, SERCOMs) switches to it, with
 * two flash wait states instead of one.  This is past the SAMD21's 48 MHz
 * rating: it works on many parts, not necessarily on every console.
 * USB, TC4/TC5 and the DAC stay on generator 4 (DFLL48M); the SERCOMs follow
 * generator 0 because the core's SPI selects it on every reconfiguration:
 * the display keeps the core's fastest setting (36 MHz at 72 MHz, which
 * the ST7735 took in gm0's overclocked META port), the card and the
 * buttons ask meta_spi_hz() for a rate that stays within theirs.  SysTick is reloaded for
 * 1 ms ticks at the new clock and meta_micros() scales its fraction.
 */
#include "meta_clock.h"

#include <Arduino.h>

static uint8_t clock_mhz = 48;
static uint32_t frac_mul = 1048576u * 1000u / 48000u; /* 2^20 us per tick */

static void gclk_sync(void) {
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
}

static void route(uint16_t id, uint16_t gen) {
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(id) | gen | GCLK_CLKCTRL_CLKEN);
    gclk_sync();
}

void meta_clock_init(void) {
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(4) | GCLK_GENDIV_DIV(1);
    gclk_sync();
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(4) | GCLK_GENCTRL_SRC_DFLL48M | GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
    gclk_sync();
    route(GCLK_CLKCTRL_ID_USB_Val, GCLK_CLKCTRL_GEN_GCLK4);
    route(GCLK_CLKCTRL_ID_TC4_TC5_Val, GCLK_CLKCTRL_GEN_GCLK4);
    route(GCLK_CLKCTRL_ID_DAC_Val, GCLK_CLKCTRL_GEN_GCLK4);
}

uint8_t meta_clock_mhz(void) { return clock_mhz; }

static void systick_for(uint8_t mhz) {
    SystemCoreClock = (uint32_t)mhz * 1000000u;
    SysTick->LOAD = (uint32_t)mhz * 1000u - 1u;
    SysTick->VAL = 0;
    frac_mul = 1048576u * 1000u / ((uint32_t)mhz * 1000u);
}

static int pll_start(void) {
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(5) | GCLK_GENDIV_DIV(48);
    gclk_sync();
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(5) | GCLK_GENCTRL_SRC_DFLL48M | GCLK_GENCTRL_GENEN;
    gclk_sync();
    route(GCLK_CLKCTRL_ID_FDPLL_Val, GCLK_CLKCTRL_GEN_GCLK5);
    SYSCTRL->DPLLCTRLA.reg = 0;
    SYSCTRL->DPLLRATIO.reg = SYSCTRL_DPLLRATIO_LDR(71) | SYSCTRL_DPLLRATIO_LDRFRAC(0); /* 1 MHz x 72 */
    SYSCTRL->DPLLCTRLB.reg = SYSCTRL_DPLLCTRLB_REFCLK(2);                         /* reference: GCLK */
    SYSCTRL->DPLLCTRLA.reg = SYSCTRL_DPLLCTRLA_ENABLE;
    const uint32_t ready = SYSCTRL_DPLLSTATUS_LOCK | SYSCTRL_DPLLSTATUS_CLKRDY;
    for (uint32_t n = 0; n < 1000000u; n++)
        if ((SYSCTRL->DPLLSTATUS.reg & ready) == ready) return 1;
    SYSCTRL->DPLLCTRLA.reg = 0;
    return 0;
}

uint8_t meta_clock_set(uint8_t mhz) {
    if (mhz != 72) mhz = 48;
    if (mhz == clock_mhz) return clock_mhz;
    if (mhz == 72) {
        if (!pll_start()) return clock_mhz;
        __disable_irq();
        NVMCTRL->CTRLB.bit.RWS = 2; /* before the clock goes up */
        GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(0) | GCLK_GENCTRL_SRC_FDPLL | GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
        gclk_sync();
        systick_for(72);
        __enable_irq();
    } else {
        __disable_irq();
        GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(0) | GCLK_GENCTRL_SRC_DFLL48M | GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
        gclk_sync();
        NVMCTRL->CTRLB.bit.RWS = NVMCTRL_CTRLB_RWS_HALF_Val; /* after it came down */
        systick_for(48);
        __enable_irq();
        SYSCTRL->DPLLCTRLA.reg = 0;
    }
    clock_mhz = mhz;
    return clock_mhz;
}

/* The core's micros(), with the sub-millisecond part scaled for the actual
 * SysTick period. */
uint32_t meta_micros(void) {
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

uint32_t meta_spi_hz(uint32_t hz) {
    if (clock_mhz == 48) return hz;
    /* The core sets BAUD + 1 = 24 MHz / requested; at 72 MHz the SPI runs
     * at 36 MHz / (BAUD + 1): ask for 24 MHz / ceil(36 MHz / hz). */
    uint32_t n = (36000000u + hz - 1u) / hz;
    return 24000000u / n;
}
