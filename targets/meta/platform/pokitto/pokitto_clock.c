/*
 * Pokitto clocks: 12 MHz crystal into the system PLL, 48 or 72 MHz.
 *
 * 48 MHz is PokittoLib's SystemInit (SYSPLLCTRL 0x23: M = 4, P = 2); 72 MHz
 * its _OSCT=2 overclock (0x25: M = 6, FCCO 288 MHz), past the LPC11U68's
 * 50 MHz rating but what Pokitto programs commonly run at (gm0 too).  The
 * flash wait states stay at their reset value, as in PokittoLib and gm0,
 * which run at 72 MHz that way.  SysTick counts milliseconds;
 * meta_micros adds the fraction of the current one.  A clock change runs
 * the core from the 12 MHz IRC while the PLL relocks, then rescales the
 * SysTick, the audio sample timer and the SD card's SPI divider.
 */
#include "meta_clock.h"

#include "lpc11u6x.h"
#include "meta_target.h"
#include "pokitto_hw.h"

static volatile uint32_t ms_count;
static uint8_t clock_mhz = 12;
static uint16_t us_mul; /* (1 << 18) / clock_mhz : micros sans division */

void SysTick_Handler(void) { ms_count++; }

uint32_t millis(void) { return ms_count; }

void delay(uint32_t ms) {
    uint32_t t = meta_micros();
    while (meta_micros() - t < ms * 1000u) {}
}

static void power(uint32_t bits, int on) {
    uint32_t v = SYSCON_PDRUNCFG & PDRUN_VALID_BITS;
    v = on ? v & ~bits : v | bits;
    SYSCON_PDRUNCFG = v | PDRUN_RESERVED_ONE;
}

static void main_clock(uint32_t sel) {
    SYSCON_MAINCLKSEL = sel; /* 0 IRC, 3 PLL output */
    SYSCON_MAINCLKUEN = 1;
    SYSCON_MAINCLKUEN = 0;
    SYSCON_MAINCLKUEN = 1;
    for (uint32_t n = 0; n < 100000u && !(SYSCON_MAINCLKUEN & 1u); n++) {}
}

static void systick_for(uint32_t mhz) {
    SYST_CSR = 0;
    SYST_RVR = mhz * 1000u - 1u;
    /* meta_micros multiplie par us_mul au lieu de diviser par clock_mhz :
     * le M0+ n'a pas de division matérielle et __udivsi3 coûtait 47 % du
     * temps du cœur (le limiteur de frames appelle meta_micros en boucle).
     * Comme le META et son frac_mul (meta_clock.cpp). */
    us_mul = (1u << 18) / mhz;
    SYST_CVR = 0;
    SYST_CSR = 7u; /* core clock, interrupt, enable */
}

static int pll_to(uint8_t mhz) {
    main_clock(0);
    power(PDRUN_SYSPLL, 0);
    SYSCON_SYSPLLCTRL = (uint32_t)(mhz / 12u - 1u) | (1u << 5); /* M, P = 2 */
    power(PDRUN_SYSPLL, 1);
    for (uint32_t n = 0; n < 200000u; n++)
        if (SYSCON_SYSPLLSTAT & 1u) {
            main_clock(3);
            return 1;
        }
    return 0; /* no lock: stay on the IRC */
}

void pokitto_clock_boot(void) {
    IOCON_PIO2(0) = IOCON_FUNC(1);    /* XTALIN (PokittoLib's SystemInit writes 0x01) */
    IOCON_PIO2(1) = IOCON_FUNC(1);    /* XTALOUT */
    SYSCON_SYSOSCCTRL = 0;            /* 1-20 MHz, not bypassed */
    power(PDRUN_SYSOSC, 1);
    for (volatile uint32_t i = 0; i < 2500u; i++) {}
    SYSCON_SYSPLLCLKSEL = 1; /* PLL input: crystal */
    SYSCON_SYSPLLCLKUEN = 1;
    SYSCON_SYSPLLCLKUEN = 0;
    SYSCON_SYSPLLCLKUEN = 1;
    for (uint32_t n = 0; n < 100000u && !(SYSCON_SYSPLLCLKUEN & 1u); n++) {}
    SYSCON_SYSAHBCLKDIV = 1;
    clock_mhz = 12;
    meta_clock_set(META_CLOCK_DEFAULT);
    if (clock_mhz == 12) systick_for(12);
}

void meta_clock_init(void) {}

uint8_t meta_clock_mhz(void) { return clock_mhz; }

uint8_t meta_clock_set(uint8_t mhz) {
    if (mhz != 72) mhz = 48;
    if (mhz == clock_mhz) return clock_mhz;
    irq_disable();
    uint8_t got = pll_to(mhz) ? mhz : 12;
    if (got == 12 && clock_mhz != 12 && pll_to(clock_mhz)) got = clock_mhz; /* back where it was */
    clock_mhz = got;
    systick_for(clock_mhz);
    irq_enable();
    pokitto_audio_clock_changed(clock_mhz * 1000000u);
    pokitto_sd_clock_changed(clock_mhz * 1000000u);
    return clock_mhz;
}

uint32_t meta_micros(void) {
    uint32_t count, val, pend;
    do {
        count = ms_count;
        val = SYST_CVR;
        pend = (SCB_ICSR & SCB_ICSR_PENDSTSET) != 0;
    } while (count != ms_count);
    /* A wrap not yet serviced (the counter reloaded before val was read):
     * the count is one millisecond behind.  A small val means the wrap came
     * after val was read. */
    if (pend && val < SYST_RVR / 2u) pend = 0;
    /* x * (2^18 / clock_mhz) >> 18 tient en 32 bits (x < clock_mhz * 1000) :
     * erreur <= 0,03 %, soit <= 0,25 µs par milliseconde. */
    return (count + pend) * 1000u + (((SYST_RVR - val) * us_mul) >> 18);
}

