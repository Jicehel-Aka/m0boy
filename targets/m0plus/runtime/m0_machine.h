#ifndef M0BOY_M0_MACHINE_H
#define M0BOY_M0_MACHINE_H
#include "m0_cpu.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { M0_MACHINE_RAM_BYTES = 40960 };
typedef struct M0Machine M0Machine;
typedef uint32_t (*M0PeripheralRead)(void *, uint32_t, unsigned);
typedef void (*M0PeripheralWrite)(void *, uint32_t, uint32_t, unsigned);
typedef void (*M0PeripheralTick)(void *, uint32_t);

/* Common flash/SRAM/SysTick/NVIC bus. A SAMD21 or LPC11U68 adapter supplies
 * the remaining memory-mapped registers and display/card/audio devices. */
struct M0Machine {
    M0Cpu cpu;
    const uint8_t *flash;
    size_t flash_size;
    uint32_t flash_base, ram_base;
    /* optional copy of a window of hot code in fast memory: starts read here, [code_lo, code_lo+code_len) */
    const uint8_t *code_win;
    uint32_t code_lo, code_len;
    uint8_t ram[M0_MACHINE_RAM_BYTES];
    void *peripheral;
    M0PeripheralRead peripheral_read;
    M0PeripheralWrite peripheral_write;
    M0PeripheralTick peripheral_tick;
    uint32_t systick_ctrl, systick_load, systick_val;
    uint32_t nvic_enable, nvic_pending;
    uint32_t systick_pending, pendsv_pending;
    uint8_t irq_priority[32], systick_priority, pendsv_priority;
    uint64_t accounted_cycles;
    uint8_t bus_fault;
};

void m0_machine_init(M0Machine *, const uint8_t *flash, size_t flash_size,
                     uint32_t flash_base, uint32_t ram_base);
uint32_t m0_machine_read(void *machine, uint32_t address, unsigned width);
void m0_machine_write(void *machine, uint32_t address, uint32_t value, unsigned width);
void m0_machine_tick(void *machine, M0Cpu *cpu, uint32_t cycles);
unsigned m0_machine_pending_exception_slow(const M0Machine *);
/* The exception the next tick would take (0: none). */
static inline unsigned m0_machine_pending_exception(const M0Machine *m) {
    if (!(m->systick_pending | m->pendsv_pending | (m->nvic_pending & m->nvic_enable))) return 0;
    return m0_machine_pending_exception_slow(m);
}
void m0_machine_irq(M0Machine *, unsigned irq);

#ifdef __cplusplus
}
#endif
#endif
