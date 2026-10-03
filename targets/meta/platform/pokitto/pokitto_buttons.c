/*
 * Pokitto buttons and the way back to the loader.
 *
 * The seven buttons are plain GPIO inputs, high when pressed (PokittoLib
 * reads them the same way, with mbed's default pull-down): A P1_9, B P1_4,
 * C P1_10, up P1_13, down P1_3, left P1_25, right P1_7.
 *
 * The loader leaves a boot key in the last flash sector (0xB007AB1E, then
 * its version and its entry table); PokittoLib's start_application hands
 * over the same way: interrupts and SysTick off, the core back on the IRC,
 * the PLL powered down, VTOR, SP and PC from the loader's table.
 */
#include "lpc11u6x.h"
#include "pokitto_hw.h"

static const uint8_t pins[7] = {9, 4, 10, 13, 3, 25, 7}; /* A B C up down left right, port 1 */

void pokitto_buttons_init(void) {
    for (unsigned i = 0; i < sizeof(pins); i++) {
        IOCON_SET(IOCON_PIO1(pins[i]), IOCON_FUNC(0), IOCON_MODE_PULLDOWN | IOCON_DIGITAL);
        GPIO_DIR(1) &= ~(1u << pins[i]);
    }
}

/* One read of the port (the byte pin registers would do as well; the
 * emulator in tools/meta-emu-sdl only answers aligned ones correctly). */
uint8_t pokitto_buttons(void) {
    uint32_t port = GPIO_PIN(1);
    uint8_t b = 0;
    for (unsigned i = 0; i < sizeof(pins); i++)
        if (port & (1u << pins[i])) b |= (uint8_t)(1u << i);
    return b;
}

int pokitto_c_held_at_boot(void) {
    IOCON_SET(IOCON_PIO1(10), IOCON_FUNC(0), IOCON_MODE_PULLDOWN | IOCON_DIGITAL);
    GPIO_DIR(1) &= ~(1u << 10);
    for (volatile uint32_t i = 0; i < 20000u; i++) {} /* let the pull settle */
    unsigned held = 0;
    for (unsigned i = 0; i < 8; i++) held += (GPIO_PIN(1) >> 10) & 1u;
    return held == 8;
}

static const uint32_t *find_bootinfo(void) {
    static const uint32_t places[4] = {0x3FFF4u, 0x3FF04u, 0x3FE04u, 0x3F004u};
    for (unsigned i = 0; i < 4; i++)
        if (*(const uint32_t *)places[i] == 0xB007AB1Eu) return (const uint32_t *)places[i];
    return 0;
}

void pokitto_enter_loader(void) {
    const uint32_t *boot = find_bootinfo();
    if (!boot) return; /* no loader installed */
    uint32_t app = boot[2];
    irq_disable();
    NVIC_ICER = 0xFFFFFFFFu;
    NVIC_ICPR = 0xFFFFFFFFu;
    GPIO_PIN_INT_IENR = 0;
    GPIO_PIN_INT_IENF = 0;
    SYST_CSR = 0;
    /* Back on the IRC, PLL off, peripherals in reset. */
    SYSCON_MAINCLKSEL = 0;
    SYSCON_MAINCLKUEN = 1;
    SYSCON_MAINCLKUEN = 0;
    SYSCON_MAINCLKUEN = 1;
    SYSCON_SYSPLLCTRL = 0;
    SYSCON_PRESETCTRL = 0;
    SYSCON_PDRUNCFG = (SYSCON_PDRUNCFG & PDRUN_VALID_BITS) | PDRUN_SYSPLL | PDRUN_RESERVED_ONE;
    SCB_VTOR = app;
    __asm__ volatile(
        "ldr r1, [%0, #0]\n"
        "mov sp, r1\n"
        "ldr r0, [%0, #4]\n"
        "bx r0\n"
        :
        : "r"(app)
        : "r0", "r1", "memory");
    for (;;) {}
}
