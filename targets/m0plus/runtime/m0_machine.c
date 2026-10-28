#include "m0_machine.h"
#include <string.h>

static uint32_t load_le(const uint8_t *p, unsigned width) {
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= (uint32_t)p[i] << (i * 8);
    return value;
}
static void store_le(uint8_t *p, uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
/* SysTick as it stands now: block-accumulated cycles are on the CPU clock
 * before the machine has ticked them, so a read counts them in. */
static uint32_t systick_now(const M0Machine *m, int *wrapped) {
    uint64_t delta = m->cpu.cycles - m->accounted_cycles;
    uint32_t val = m->systick_val;
    *wrapped = 0;
    if (!(m->systick_ctrl & 1u) || !delta) return val;
    const uint64_t rem = val ? val : (uint64_t)m->systick_load + 1u;
    if (delta < rem) return (uint32_t)(rem - delta);
    delta -= rem;
    *wrapped = 1;
    if (!delta) return 0;
    delta %= (uint64_t)m->systick_load + 1u;
    return delta ? (uint32_t)(m->systick_load + 1u - delta) : 0;
}
static uint32_t scs_word(M0Machine *m, uint32_t address) {
    int wrapped;
    switch (address) {
    case 0xe000e010u: systick_now(m, &wrapped); return m->systick_ctrl | (uint32_t)wrapped << 16;
    case 0xe000e014u: return m->systick_load;
    case 0xe000e018u: return systick_now(m, &wrapped);
    case 0xe000e100u: return m->nvic_enable;
    case 0xe000e180u: return m->nvic_enable;
    case 0xe000e200u: return m->nvic_pending;
    case 0xe000e280u: return m->nvic_pending;
    case 0xe000ed04u:
        return (m->systick_pending ? 1u << 26 : 0) |
               (m->pendsv_pending ? 1u << 28 : 0) | m->cpu.exception;
    case 0xe000ed08u: return m->cpu.vector_base;
    case 0xe000ed0cu: return 0xfa050000u;
    case 0xe000ed20u: return (uint32_t)m->pendsv_priority << 16 |
                              (uint32_t)m->systick_priority << 24;
    default:
        if (address >= 0xe000e400u && address < 0xe000e420u) {
            uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value |= (uint32_t)m->irq_priority[address - 0xe000e400u + i] << (8 * i);
            return value;
        }
        return 0;
    }
}
static void scs_write(M0Machine *m, uint32_t address, uint32_t value, uint32_t mask) {
    switch (address) {
    case 0xe000e010u: m->systick_ctrl = (m->systick_ctrl & ~mask) | (value & mask & 7u); break;
    case 0xe000e014u: m->systick_load = value & 0xffffffu; break;
    case 0xe000e018u: m->systick_val = 0; m->systick_ctrl &= ~(1u << 16); break;
    case 0xe000e100u: m->nvic_enable |= value; break;
    case 0xe000e180u: m->nvic_enable &= ~value; break;
    case 0xe000e200u: m->nvic_pending |= value; break;
    case 0xe000e280u: m->nvic_pending &= ~value; break;
    case 0xe000ed04u:
        if (value & (1u << 26)) m->systick_pending = 1;
        if (value & (1u << 25)) m->systick_pending = 0;
        if (value & (1u << 28)) m->pendsv_pending = 1;
        if (value & (1u << 27)) m->pendsv_pending = 0;
        break;
    case 0xe000ed08u: m->cpu.vector_base = value & ~0x7fu; break;
    case 0xe000ed20u:
        m->pendsv_priority = (uint8_t)(value >> 16);
        m->systick_priority = (uint8_t)(value >> 24);
        break;
    default:
        if (address >= 0xe000e400u && address < 0xe000e420u)
            for (unsigned i = 0; i < 4; ++i)
                if (mask & (255u << (8 * i)))
                    m->irq_priority[address - 0xe000e400u + i] = (uint8_t)(value >> (8 * i));
        break;
    }
}

uint32_t m0_machine_read(void *context, uint32_t address, unsigned width) {
    M0Machine *m = context;
    if (width != 1 && width != 2 && width != 4) { m->bus_fault = m->cpu.fault = 1; return 0; }
    if (address >= m->flash_base && (uint64_t)address + width <= (uint64_t)m->flash_base + m->flash_size)
        return load_le(m->flash + address - m->flash_base, width);
    if (address >= m->ram_base && (uint64_t)address + width <= (uint64_t)m->ram_base + sizeof m->ram)
        return load_le(m->ram + address - m->ram_base, width);
    if (address >= 0xe000e000u && address < 0xe0010000u) {
        unsigned shift = (address & 3u) * 8u;
        uint32_t value = scs_word(m, address & ~3u);
        if ((address & ~3u) == 0xe000e010u)
            m->systick_ctrl &= ~(1u << 16); /* COUNTFLAG clears on read */
        return (value >> shift) & (width == 4 ? 0xffffffffu : (1u << (width * 8)) - 1u);
    }
    if (m->peripheral_read) return m->peripheral_read(m->peripheral, address, width);
    m->bus_fault = 1;
    m->cpu.fault = 1;
    return 0;
}
void m0_machine_write(void *context, uint32_t address, uint32_t value, unsigned width) {
    M0Machine *m = context;
    if (width != 1 && width != 2 && width != 4) { m->bus_fault = m->cpu.fault = 1; return; }
    if (address >= m->ram_base && (uint64_t)address + width <= (uint64_t)m->ram_base + sizeof m->ram) {
        store_le(m->ram + address - m->ram_base, value, width);
        return;
    }
    if (address >= 0xe000e000u && address < 0xe0010000u) {
        unsigned shift = (address & 3u) * 8u;
        uint32_t mask = width == 4 ? 0xffffffffu : ((1u << (width * 8)) - 1u) << shift;
        scs_write(m, address & ~3u, value << shift, mask);
        return;
    }
    if (m->peripheral_write) { m->peripheral_write(m->peripheral, address, value, width); return; }
    m->bus_fault = 1;
    m->cpu.fault = 1;
}
void m0_machine_irq(M0Machine *m, unsigned irq) {
    if (irq < 32) m->nvic_pending |= 1u << irq;
}
/* The exception the next tick would take (0: none), by the NVIC rules: not
 * under PRIMASK, and only above the priority of a live handler. */
unsigned m0_machine_pending_exception_slow(const M0Machine *m) {
    const M0Cpu *cpu = &m->cpu;
    if (cpu->primask || cpu->fault) return 0;
    /* nothing pending and enabled: no exception to choose (the common case) */
    if (!(m->systick_pending | m->pendsv_pending | (m->nvic_pending & m->nvic_enable))) return 0;
    unsigned selected = 0; uint8_t priority = 255;
    if (m->systick_pending && m->systick_priority < priority)
        selected = 15, priority = m->systick_priority;
    if (m->pendsv_pending && m->pendsv_priority < priority)
        selected = 14, priority = m->pendsv_priority;
    uint32_t ready = m->nvic_pending & m->nvic_enable;
    for (uint32_t bits = ready; bits; bits &= bits - 1) {
        unsigned irq = (unsigned)__builtin_ctz(bits);
        if (m->irq_priority[irq] < priority) selected = irq + 16, priority = m->irq_priority[irq];
    }
    if (!selected) return 0;
    /* A live handler may only be preempted by a higher-priority exception.
     * The board can refine this with device-specific NVIC priority bits. */
    if (cpu->exception) {
        uint8_t active = cpu->exception == 15 ? m->systick_priority :
                         cpu->exception == 14 ? m->pendsv_priority :
                         cpu->exception >= 16 ? m->irq_priority[cpu->exception - 16] : 0;
        if (priority >= active) return 0;
    }
    return selected;
}

void m0_machine_tick(void *context, M0Cpu *cpu, uint32_t cycles) {
    M0Machine *m = context;
    /* CPU exceptions and block-accumulated cycles reach the clock without a
     * callback: account from the absolute counter. */
    uint64_t delta = cpu->cycles - m->accounted_cycles;
    (void)cycles;
    m->accounted_cycles += delta;
    if (m->peripheral_tick) m->peripheral_tick(m->peripheral, (uint32_t)delta);
    if ((m->systick_ctrl & 1u) && m->systick_val > delta) {
        m->systick_val -= (uint32_t)delta;                    /* no wrap: the common case */
    } else if (m->systick_ctrl & 1u) {
        while (delta) {
            uint32_t remaining = m->systick_val ? m->systick_val : m->systick_load + 1u;
            if (delta < remaining) { m->systick_val = remaining - (uint32_t)delta; break; }
            delta -= remaining;
            m->systick_val = 0;
            m->systick_ctrl |= 1u << 16;
            if (m->systick_ctrl & 2u) m->systick_pending = 1;
        }
    }
    unsigned selected = m0_machine_pending_exception(m);
    if (!selected) return;
    if (selected == 15) m->systick_pending = 0;
    else if (selected == 14) m->pendsv_pending = 0;
    else m->nvic_pending &= ~(1u << (selected - 16));
    m0_cpu_exception(cpu, selected);
}
void m0_machine_init(M0Machine *m, const uint8_t *flash, size_t flash_size,
                     uint32_t flash_base, uint32_t ram_base) {
    memset(m, 0, sizeof *m);
    m->flash = flash; m->flash_size = flash_size;
    m->flash_base = flash_base; m->ram_base = ram_base;
    memset(m->ram, 0, sizeof m->ram);
    m0_cpu_reset(&m->cpu, m, m0_machine_read, m0_machine_write,
                 m0_machine_tick, flash_base);
}
