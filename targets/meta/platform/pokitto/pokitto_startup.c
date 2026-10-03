/*
 * Pokitto startup: vector table, reset handler, early loader check.
 *
 * Bare metal, no mbed: the reset handler copies .data (which also holds
 * the runtime's SRAM functions, GBL_PPU_RAMFUNC), clears .bss and the two
 * small RAM banks, sets the clocks (pokitto_clock.c) and calls main().
 *
 * The Pokitto loader (LOADER.BIN flashed at 0x39000 by the community
 * loader) is reached the way PokittoLib does it: C held at power-on, or the
 * option menu's "Quitter".  The boot ROM only starts code whose first eight
 * vector words sum to zero; the Makefile writes that checksum into word 7
 * of firmware.bin after linking.
 */
#include <stdint.h>

#include "lpc11u6x.h"
#include "pokitto_hw.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
extern uint32_t _sram1_bss, _eram1_bss, _susbram_bss, _eusbram_bss;
extern uint32_t __StackTop;

int main(void);
void Reset_Handler(void);
void __libc_init_array(void);

static void default_handler(void) {
    for (;;) {}
}

/* A fault stops on a red screen rather than running wild. */
static void hard_fault(void) {
    pokitto_fault_screen();
}

void SysTick_Handler(void);
void CT32B0_IRQHandler(void);

typedef void (*vector_fn)(void);

/* 16 core vectors and 32 LPC11U6x interrupts.  Word 7 is the boot ROM's
 * checksum, filled in after linking. */
__attribute__((section(".isr_vector"), used)) const vector_fn pokitto_vectors[48] = {
    (vector_fn)&__StackTop,
    Reset_Handler,
    default_handler, /* NMI */
    hard_fault,
    0, 0, 0,
    0,               /* checksum (Makefile) */
    0, 0, 0,
    default_handler, /* SVCall */
    0, 0,
    default_handler, /* PendSV */
    SysTick_Handler,
    /* IRQ 0..31 */
    default_handler, default_handler, default_handler, default_handler, /* PIN_INT0..3 */
    default_handler, default_handler, default_handler, default_handler, /* PIN_INT4..7 */
    default_handler, default_handler, default_handler, default_handler, /* GINT0/1, I2C1, USART1_4 */
    default_handler, default_handler, default_handler, default_handler, /* USART2_3, SCT0_1, SSP1, I2C0 */
    default_handler, default_handler,                                   /* CT16B0, CT16B1 */
    CT32B0_IRQHandler,                                                  /* 18: audio */
    default_handler, default_handler, default_handler, default_handler, /* CT32B1, SSP0, USART0, USB */
    default_handler, default_handler, default_handler, default_handler, /* USB_FIQ, ADC_A, RTC, BOD_WDT */
    default_handler, default_handler, default_handler, default_handler, /* FLASH, DMA, ADC_B, USBWAKEUP */
    default_handler,
};

/* __libc_init_array calls these (no crti/crtn with -nostartfiles). */
void _init(void) {}
void _fini(void) {}

static void zero(uint32_t *p, uint32_t *end) {
    while (p < end) *p++ = 0;
}

void Reset_Handler(void) {
    /* The loader may leave its own table in VTOR. */
    SCB_VTOR = 0;
    /* GPIO, IOCON and the two small RAM banks clocked. */
    SYSCON_SYSAHBCLKCTRL |= AHB_GPIO | AHB_IOCON | AHB_RAM1 | AHB_USBRAM;

    /* C held at power-on: the loader, before anything else changes. */
    if (pokitto_c_held_at_boot()) pokitto_enter_loader();

    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    zero(&_sbss, &_ebss);
    zero(&_sram1_bss, &_eram1_bss);
    zero(&_susbram_bss, &_eusbram_bss);

    pokitto_clock_boot();
    __libc_init_array();
    main();
    for (;;) {}
}
