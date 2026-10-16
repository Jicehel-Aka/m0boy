/*
 * Gamebuino META startup: vector table, reset handler, clocks, SysTick.
 *
 * Bare metal, no Arduino core: the reset handler points VTOR at this
 * firmware (the loader normally has), disarms the loader's launch
 * watchdog, copies .data (which also holds the runtime's SRAM functions,
 * GBL_PPU_RAMFUNC), clears .bss, sets the clocks as the Arduino Zero core
 * did (DFLL48M locked to the 32 kHz crystal on generator 0, one flash wait
 * state) and calls main().  Generators 2 (the watchdog's) and 3 (OSC8M) are
 * the core's too; meta_clock.cpp adds 4 and 5.  The peripherals clocked on
 * the APBC bus are the core's set, so the loader finds them as it left
 * them when HOME brings it back.
 */
#include <stdint.h>

#include "meta_hw.h"

extern uint32_t __etext, __data_start__, __data_end__, __bss_start__, __bss_end__, __StackTop;

int main(void);
void Reset_Handler(void);
void __libc_init_array(void);

uint32_t SystemCoreClock = 1000000u;
static volatile uint32_t ticks; /* milliseconds */

/* A fault, an interrupt nothing handles, or the watchdog's early warning
 * (the main loop has not fed it for 2 s, meta_watchdog_start): the
 * exception frame (r0-r3, r12, lr, pc, xpsr) and the exception number go to
 * meta_fault, which survives resets (.noinit).  With META_USB the handler
 * answers the host for two seconds (tools/meta-usb.py fault; the 1200-baud
 * touch still works), then the console restarts: a software reset, which
 * the bootloader turns into the game again. */
#define META_FAULT_MAGIC 0x46415532u /* layout 2 */
__attribute__((section(".noinit"))) volatile meta_fault_log meta_fault;
void USB_Handler(void);
void meta_watchdog_feed(void);
void meta_reset_to_bootloader(void) __attribute__((weak));
extern volatile uint8_t meta_usb_bootloader_request __attribute__((weak));

void meta_fault_c(uint32_t *frame) {
    for (int i = 0; i < 8; i++) meta_fault.frame[i] = frame[i];
    uint32_t ipsr;
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    meta_fault.exception = ipsr;
    meta_fault.count++;
    meta_fault.early = ticks < 5000u ? meta_fault.early + 1u : 0u;
    /* About two seconds: the SysTick counter keeps running with its
     * interrupt masked, one wrap of VAL per millisecond.  Forever when the
     * game keeps faulting right after boot: a restart loop would leave only
     * the RESET button to reach the bootloader. */
    int restart = meta_fault.early < 3u;
    uint32_t last = SysTick->VAL;
    for (uint32_t ms = 0, spins = 0; !restart || (ms < 2000u && spins < 20000000u); spins++) {
        uint32_t now = SysTick->VAL;
        if (now > last) ms++; /* counts down: it wrapped */
        last = now;
        meta_watchdog_feed(); /* not a bare CLEAR write: during a sync it stalls the bus for ms */
        if (meta_reset_to_bootloader) {
            USB_Handler();
            if (meta_usb_bootloader_request) meta_reset_to_bootloader();
        }
    }
    NVIC_SystemReset();
}

__attribute__((naked)) static void default_handler(void) {
    __asm__ volatile("movs r0, #4\n mov r1, lr\n tst r0, r1\n beq 1f\n mrs r0, psp\n b meta_fault_c\n"
                     "1: mrs r0, msp\n b meta_fault_c\n");
}

/* The watchdog: 4 s without meta_watchdog_feed resets the console (the
 * bootloader then starts the Gamebuino launcher); its early warning at 2 s
 * goes through default_handler first, which logs where the main loop was
 * and restarts the game.  Clock: generator 2, OSCULP32K / 32 = 1024 Hz. */
void meta_watchdog_start(void) {
    /* Generator 2's divider is 5 bits; with DIVSEL it divides by 2^(DIV+1). */
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(2) | GCLK_GENDIV_DIV(4);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(2) | GCLK_GENCTRL_SRC_OSCULP32K | GCLK_GENCTRL_DIVSEL | GCLK_GENCTRL_GENEN;
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_WDT_Val) | GCLK_CLKCTRL_GEN_GCLK2 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
    WDT->CONFIG.reg = WDT_CONFIG_PER_4K;
    WDT->EWCTRL.reg = WDT_EWCTRL_EWOFFSET_2K;
    while (WDT->STATUS.reg & WDT_STATUS_SYNCBUSY) {}
    WDT->INTFLAG.reg = WDT_INTFLAG_EW;
    WDT->INTENSET.reg = WDT_INTENSET_EW;
    NVIC_SetPriority(WDT_IRQn, 0);
    NVIC_EnableIRQ(WDT_IRQn);
    WDT->CTRL.reg = WDT_CTRL_ENABLE;
    while (WDT->STATUS.reg & WDT_STATUS_SYNCBUSY) {}
}

void meta_watchdog_stop(void) {
    NVIC_DisableIRQ(WDT_IRQn);
    WDT->INTENCLR.reg = WDT_INTENCLR_EW;
    WDT->CTRL.reg = 0;
    while (WDT->STATUS.reg & WDT_STATUS_SYNCBUSY) {}
}

/* Writes during a synchronisation are ignored: no need to wait. */
void meta_watchdog_feed(void) {
    if (!(WDT->STATUS.reg & WDT_STATUS_SYNCBUSY)) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
}

/* meta_usb.c, when the firmware has it (META_USB). */
extern volatile uint32_t meta_usb_prof[] __attribute__((weak)); /* count, then a ring of 32 PCs */

void SysTick_body(uint32_t *frame);

/* The exception frame is on the main stack (no thread uses the process
 * stack): its PC is where the CPU was, for the host's profiler. */
__attribute__((naked)) void SysTick_Handler(void) {
    __asm__ volatile("mrs r0, msp\n b SysTick_body\n");
}

void SysTick_body(uint32_t *frame) {
    ticks++;
    if (meta_usb_prof) {
        uint32_t n = meta_usb_prof[0];
        meta_usb_prof[1 + (n & 31u)] = frame[6];
        meta_usb_prof[0] = n + 1u;
    }
    /* The 1200-baud touch while the frontend never reaches its frame hook
     * (a hung game, a long boot): reset into the bootloader anyway. */
    static uint16_t request_ms;
    if (&meta_usb_bootloader_request && meta_usb_bootloader_request && ++request_ms > 1000)
        meta_reset_to_bootloader();
}

/* The drivers' interrupts (meta_lcd.cpp, meta_audio.cpp without DMA); a
 * build without one keeps the default handler. */
void DMAC_Handler(void) __attribute__((weak, alias("default_handler")));
void TC4_Handler(void) __attribute__((weak, alias("default_handler")));
void USB_Handler(void) __attribute__((weak, alias("default_handler")));

typedef void (*vector_fn)(void);

/* 16 core vectors and the SAMD21's 28 interrupts (plus a reserved word). */
__attribute__((section(".isr_vector"), used)) const vector_fn meta_vectors[45] = {
    (vector_fn)&__StackTop,
    Reset_Handler,
    default_handler, /* NMI */
    default_handler, /* HardFault */
    0, 0, 0, 0, 0, 0, 0,
    default_handler, /* SVCall */
    0, 0,
    default_handler, /* PendSV */
    SysTick_Handler,
    /* IRQ 0..27 */
    default_handler, default_handler, default_handler, default_handler, /* PM, SYSCTRL, WDT, RTC */
    default_handler, default_handler,                                   /* EIC, NVMCTRL */
    DMAC_Handler,                                                       /* 6 */
    USB_Handler, default_handler,                                       /* USB, EVSYS */
    default_handler, default_handler, default_handler, default_handler, /* SERCOM0..3 */
    default_handler, default_handler,                                   /* SERCOM4, 5 */
    default_handler, default_handler, default_handler, default_handler, /* TCC0..2, TC3 */
    TC4_Handler,                                                        /* 19 */
    default_handler, default_handler, default_handler,                  /* TC5..7 */
    default_handler, default_handler, default_handler, default_handler, /* ADC, AC, DAC, PTC */
    default_handler,                                                    /* I2S */
    0,
};

static void gclk_sync(void) {
    while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {}
}

/* The Arduino Zero core's SystemInit (startup.c), crystal variant. */
void SystemInit(void) {
    NVMCTRL->CTRLB.bit.RWS = NVMCTRL_CTRLB_RWS_HALF_Val; /* one wait state for 48 MHz */
    PM->APBAMASK.reg |= PM_APBAMASK_GCLK;

    SYSCTRL->XOSC32K.reg = SYSCTRL_XOSC32K_STARTUP(0x6u) | SYSCTRL_XOSC32K_XTALEN | SYSCTRL_XOSC32K_EN32K;
    SYSCTRL->XOSC32K.bit.ENABLE = 1; /* separately, datasheet 15.6.3 */
    while (!(SYSCTRL->PCLKSR.reg & SYSCTRL_PCLKSR_XOSC32KRDY)) {}

    GCLK->CTRL.reg = GCLK_CTRL_SWRST;
    while ((GCLK->CTRL.reg & GCLK_CTRL_SWRST) && (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY)) {}

    /* Generator 1: the crystal, reference of the DFLL48M. */
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(1);
    gclk_sync();
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(1) | GCLK_GENCTRL_SRC_XOSC32K | GCLK_GENCTRL_GENEN;
    gclk_sync();
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_DFLL48_Val) | GCLK_CLKCTRL_GEN_GCLK1 | GCLK_CLKCTRL_CLKEN;
    gclk_sync();

    /* DFLL48M in closed loop: 48 MHz / 32768 Hz = 1465 (rounded). */
    SYSCTRL->DFLLCTRL.reg = SYSCTRL_DFLLCTRL_ENABLE;
    while (!(SYSCTRL->PCLKSR.reg & SYSCTRL_PCLKSR_DFLLRDY)) {}
    SYSCTRL->DFLLMUL.reg = SYSCTRL_DFLLMUL_CSTEP(31) | SYSCTRL_DFLLMUL_FSTEP(511) |
                           SYSCTRL_DFLLMUL_MUL((48000000u + 32768u / 2u) / 32768u);
    while (!(SYSCTRL->PCLKSR.reg & SYSCTRL_PCLKSR_DFLLRDY)) {}
    SYSCTRL->DFLLCTRL.reg |= SYSCTRL_DFLLCTRL_MODE | SYSCTRL_DFLLCTRL_WAITLOCK | SYSCTRL_DFLLCTRL_QLDIS;
    while (!(SYSCTRL->PCLKSR.reg & SYSCTRL_PCLKSR_DFLLRDY)) {}
    SYSCTRL->DFLLCTRL.reg |= SYSCTRL_DFLLCTRL_ENABLE;
    while (!(SYSCTRL->PCLKSR.reg & SYSCTRL_PCLKSR_DFLLLCKC) || !(SYSCTRL->PCLKSR.reg & SYSCTRL_PCLKSR_DFLLLCKF)) {}
    while (!(SYSCTRL->PCLKSR.reg & SYSCTRL_PCLKSR_DFLLRDY)) {}

    /* Generator 0 (CPU and buses) on the DFLL48M. */
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(0);
    gclk_sync();
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(0) | GCLK_GENCTRL_SRC_DFLL48M | GCLK_GENCTRL_IDC | GCLK_GENCTRL_GENEN;
    gclk_sync();

    /* OSC8M undivided on generator 3. */
    SYSCTRL->OSC8M.bit.PRESC = SYSCTRL_OSC8M_PRESC_0_Val;
    SYSCTRL->OSC8M.bit.ONDEMAND = 0;
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(3);
    gclk_sync(); /* every GCLK write waits for SYNCBUSY (datasheet 15.6.6) */
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(3) | GCLK_GENCTRL_SRC_OSC8M | GCLK_GENCTRL_GENEN;
    gclk_sync();

    PM->CPUSEL.reg = PM_CPUSEL_CPUDIV_DIV1;
    PM->APBASEL.reg = PM_APBASEL_APBADIV_DIV1_Val;
    PM->APBBSEL.reg = PM_APBBSEL_APBBDIV_DIV1_Val;
    PM->APBCSEL.reg = PM_APBCSEL_APBCDIV_DIV1_Val;
    SystemCoreClock = 48000000u;

    NVMCTRL->CTRLB.bit.MANW = 1; /* no automatic flash writes (errata 13134) */
}

void Reset_Handler(void) {
    SCB->VTOR = (uint32_t)meta_vectors & SCB_VTOR_TBLOFF_Msk;

    /* The loader leaves its launch watchdog armed and nothing here feeds
     * it: disarm it, or the console resets shortly after boot.  When it is
     * locked on (ALWAYSON) the frontend feeds it once per frame instead. */
    NVIC_DisableIRQ(WDT_IRQn);
    WDT->INTENCLR.reg = WDT_INTENCLR_EW;
    WDT->INTFLAG.reg = WDT_INTFLAG_EW;
    WDT->CTRL.reg = 0;
    while (WDT->STATUS.reg & WDT_STATUS_SYNCBUSY) {}

    if (meta_fault.magic != META_FAULT_MAGIC) {
        for (unsigned i = 0; i < sizeof meta_fault / 4u; i++) ((volatile uint32_t *)&meta_fault)[i] = 0;
        meta_fault.magic = META_FAULT_MAGIC;
    }
    meta_fault.rcause = PM->RCAUSE.reg; /* 0x20: the watchdog itself (a lockup) */

    uint32_t *src = &__etext, *dst = &__data_start__;
    while (dst < &__data_end__) *dst++ = *src++;
    for (dst = &__bss_start__; dst < &__bss_end__;) *dst++ = 0;

    SystemInit();

    /* SysTick: 1 ms, second lowest priority (the core's setting). */
    SysTick_Config(SystemCoreClock / 1000u);
    NVIC_SetPriority(SysTick_IRQn, (1u << __NVIC_PRIO_BITS) - 2u);

    /* The core's APBC clocks: SERCOMs, timers, ADC and DAC. */
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM0 | PM_APBCMASK_SERCOM1 | PM_APBCMASK_SERCOM2 | PM_APBCMASK_SERCOM3 |
                        PM_APBCMASK_SERCOM4 | PM_APBCMASK_SERCOM5 | PM_APBCMASK_TCC0 | PM_APBCMASK_TCC1 |
                        PM_APBCMASK_TCC2 | PM_APBCMASK_TC3 | PM_APBCMASK_TC4 | PM_APBCMASK_TC5 |
                        PM_APBCMASK_ADC | PM_APBCMASK_DAC;

    meta_watchdog_start();

    __libc_init_array();
    main();
    for (;;) {}
}

uint32_t millis(void) { return ticks; }

void delay(uint32_t ms) {
    uint32_t t = millis();
    /* +1: the first tick may come right away. */
    while (millis() - t <= ms) {}
}
