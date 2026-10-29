#ifndef M0BOY_M0_CPU_H
#define M0BOY_M0_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ARMv6-M architectural state. r[15] is the address of the next instruction;
 * an instruction that reads PC observes its own address plus four. The board
 * owns the bus, IRQ scheduler and device timing. Both translated and fallback
 * instructions enter m0_cpu_execute, so they have identical side effects. */
typedef struct M0Cpu M0Cpu;
typedef uint32_t (*M0Read)(void *board, uint32_t address, unsigned width);
typedef void (*M0Write)(void *board, uint32_t address, uint32_t value, unsigned width);
typedef void (*M0Event)(void *board, M0Cpu *cpu, uint32_t cycles);

struct M0Cpu {
    uint32_t r[16];
    uint64_t cycles;                    /* the board's clock: peripherals read it directly */
    uint32_t dead_below;                /* stores under this address (the watched loop's SP) are scratch */
    uint32_t stores;                    /* writes and peripheral accesses so far (a loop that changes nothing leaves it alone) */
    uint8_t n, z, c, v, primask, fault, exception;
    uint32_t vector_base;
    void *board;
    M0Read read;
    M0Write write;
    M0Event event;
};

void m0_cpu_reset(M0Cpu *cpu, void *board, M0Read read, M0Write write,
                  M0Event event, uint32_t vector_base);
void m0_cpu_step(M0Cpu *cpu);
/* Enter a vector after the current instruction. The board chooses pending
 * interrupts and priorities; SVC is taken by the CPU itself. */
void m0_cpu_exception(M0Cpu *cpu, unsigned vector);
/* Execute a predecoded flash instruction. op2 is fetched only for 32-bit
 * encodings; a generated C call may pass a literal, while the interpreter
 * obtains it from the bus. */
void m0_cpu_execute(M0Cpu *cpu, uint32_t address, uint16_t op, uint16_t op2);

/* Take the exception return named by an EXC_RETURN value (0xfffffff1/9). */
void m0_cpu_exc_return(M0Cpu *cpu, uint32_t code);

/* Small instruction primitives used by generated C regions. */
static inline void m0_native_nz(M0Cpu *m, uint32_t value) {
    m->n = (uint8_t)(value >> 31);
    m->z = (uint8_t)(value == 0);
}
static inline uint32_t m0_native_add(M0Cpu *m, uint32_t a, uint32_t b, unsigned carry) {
    uint64_t wide = (uint64_t)a + b + carry;
    uint32_t value = (uint32_t)wide;
    m->c = (uint8_t)(wide >> 32);
    m->v = (uint8_t)((~(a ^ b) & (a ^ value)) >> 31);
    m0_native_nz(m, value);
    return value;
}
static inline uint32_t m0_native_sub(M0Cpu *m, uint32_t a, uint32_t b) {
    return m0_native_add(m, a, ~b, 1);
}
static inline int m0_native_cond(const M0Cpu *m, unsigned cc) {
    switch (cc) {
    case 0: return m->z; case 1: return !m->z;
    case 2: return m->c; case 3: return !m->c;
    case 4: return m->n; case 5: return !m->n;
    case 6: return m->v; case 7: return !m->v;
    case 8: return m->c && !m->z; case 9: return !m->c || m->z;
    case 10: return m->n == m->v; case 11: return m->n != m->v;
    case 12: return !m->z && m->n == m->v;
    case 13: return m->z || m->n != m->v;
    default: return 0;
    }
}
static inline void m0_native_finish(M0Cpu *m, uint32_t cycles) {
    m->cycles += cycles;
    if (m->event) m->event(m->board, m, cycles);
}

#ifdef __cplusplus
}
#endif
#endif
