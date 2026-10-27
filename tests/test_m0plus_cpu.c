#include "m0_cpu.h"
#include "m0_machine.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t flash[64];
    uint8_t ram[256];
    uint32_t elapsed;
} Board;
static uint32_t read_bus(void *context, uint32_t address, unsigned width) {
    Board *b = context;
    const uint8_t *p = 0;
    if (address >= 0x4000 && address + width <= 0x4000 + sizeof b->flash)
        p = b->flash + address - 0x4000;
    if (address >= 0x20000000 && address + width <= 0x20000000 + sizeof b->ram)
        p = b->ram + address - 0x20000000;
    assert(p);
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= (uint32_t)p[i] << (8 * i);
    return value;
}
static void write_bus(void *context, uint32_t address, uint32_t value, unsigned width) {
    Board *b = context;
    assert(address >= 0x20000000 && address + width <= 0x20000100);
    uint8_t *p = b->ram + address - 0x20000000;
    for (unsigned i = 0; i < width; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void event(void *context, M0Cpu *cpu, uint32_t cycles) {
    Board *b = context;
    b->elapsed += cycles;
    assert(b->elapsed == cpu->cycles);
}
static void half(Board *b, unsigned offset, uint16_t op) {
    b->flash[offset] = (uint8_t)op;
    b->flash[offset + 1] = (uint8_t)(op >> 8);
}
static void word(Board *b, unsigned offset, uint32_t value) {
    half(b, offset, (uint16_t)value);
    half(b, offset + 2, (uint16_t)(value >> 16));
}
int main(void) {
    Board b = {0}; M0Cpu m;
    word(&b, 0, 0x200000fc); word(&b, 4, 0x4009);
    half(&b, 8, 0x2005);  /* movs r0, #5 */
    half(&b, 10, 0x2103); /* movs r1, #3 */
    half(&b, 12, 0x1840); /* adds r0, r0, r1 */
    half(&b, 14, 0x3002); /* adds r0, #2 */
    half(&b, 16, 0x9000); /* str r0, [sp] */
    half(&b, 18, 0x9a00); /* ldr r2, [sp] */
    half(&b, 20, 0x2a0a); /* cmp r2, #10 */
    half(&b, 22, 0xd101); /* bne failure */
    half(&b, 24, 0xe7fe); /* b self */
    half(&b, 28, 0xbe00); /* failure: bkpt */
    m0_cpu_reset(&m, &b, read_bus, write_bus, event, 0x4000);
    assert(!m.fault && m.r[13] == 0x200000fc && m.r[15] == 0x4008);
    for (unsigned i = 0; i < 9; ++i) m0_cpu_step(&m);
    assert(!m.fault && m.r[0] == 10 && m.r[2] == 10 && m.z);
    assert(m.r[15] == 0x4018 && read_bus(&b, 0x200000fc, 4) == 10);
    m0_cpu_step(&m);
    assert(m.r[15] == 0x4018 && b.elapsed == m.cycles);
    /* SVC stacks the interrupted state, executes the vector, and returns
     * through BX LR. The handler changes saved r0 in its stack frame. */
    half(&b, 32, 0xdf00);
    word(&b, 44, 0x4031);
    half(&b, 48, 0x204d); /* movs r0, #77 */
    half(&b, 50, 0x9000); /* str r0, [sp] */
    half(&b, 52, 0x4770); /* bx lr */
    m0_cpu_reset(&m, &b, read_bus, write_bus, 0, 0x4000);
    m.r[15] = 0x4020;
    for (unsigned i = 0; i < 4; ++i) m0_cpu_step(&m);
    assert(!m.fault && m.exception == 0 && m.r[15] == 0x4022 &&
           m.r[13] == 0x200000fc && m.r[0] == 77);
    {
        uint8_t image[128] = {0};
        M0Machine machine;
        image[1] = 0x01; image[3] = 0x20; /* stack = 0x20000100 */
        image[4] = 9; image[5] = 0x40;   /* reset = 0x4009 */
        image[60] = 0x41; image[61] = 0x40; /* SysTick vector = 0x4041 */
        image[8] = 0x00; image[9] = 0xbf; /* NOP */
        image[64] = 0x2a; image[65] = 0x20; /* MOVS r0,#42 */
        image[66] = 0x00; image[67] = 0x90; /* STR r0,[sp] */
        image[68] = 0x70; image[69] = 0x47; /* BX LR */
        m0_machine_init(&machine, image, sizeof image, 0x4000, 0x20000000);
        m0_machine_write(&machine, 0xe000ed04, 1u << 26, 4);
        m0_cpu_step(&machine.cpu);
        assert(machine.cpu.exception == 15 && machine.cpu.r[15] == 0x4040);
        for (unsigned i = 0; i < 3; ++i) m0_cpu_step(&machine.cpu);
        assert(!machine.cpu.fault && machine.cpu.exception == 0 &&
               machine.cpu.r[0] == 42 && machine.cpu.r[15] == 0x400a);
    }
    puts("M0+ CPU basic bus/branch parity OK");
    return 0;
}
