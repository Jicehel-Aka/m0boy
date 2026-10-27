#include "m0_machine.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t m0_rom[];
extern const uint32_t m0_rom_size;
extern int m0_native_step(M0Cpu *);
#ifndef M0_FLASH_BASE
#error "M0_FLASH_BASE missing"
#endif
#ifndef M0_RAM_BASE
#error "M0_RAM_BASE missing"
#endif

static M0Machine machine;
static uint32_t hash_word(uint32_t h, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) { h = (h ^ (v & 255u)) * 0x01000193u; v >>= 8; }
    return h;
}
int main(int argc, char **argv) {
    uint32_t steps = 1000;
    int interpret = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--interp") == 0) interpret = 1;
        else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc)
            steps = (uint32_t)strtoul(argv[++i], 0, 0);
        else { fprintf(stderr, "usage: %s [--interp] [--steps N]\n", argv[0]); return 2; }
    }
    m0_machine_init(&machine, m0_rom, m0_rom_size, M0_FLASH_BASE, M0_RAM_BASE);
    uint32_t done = 0, native = 0;
    for (; done < steps && !machine.cpu.fault; ++done) {
        if (!interpret && m0_native_step(&machine.cpu)) ++native;
        else m0_cpu_step(&machine.cpu);
    }
    uint32_t hash = 0x811c9dc5u;
    for (unsigned i = 0; i < 16; ++i) hash = hash_word(hash, machine.cpu.r[i]);
    hash = hash_word(hash, machine.cpu.cycles);
    for (unsigned i = 0; i < sizeof machine.ram; ++i)
        hash = (hash ^ machine.ram[i]) * 0x01000193u;
    printf("run_hash=%08x steps=%u native=%u pc=%08x fault=%u bus_fault=%u\n",
           hash, done, native, machine.cpu.r[15], machine.cpu.fault, machine.bus_fault);
    return machine.cpu.fault ? 1 : 0;
}
