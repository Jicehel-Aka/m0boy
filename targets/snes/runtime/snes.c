#include "snes.h"

#include <string.h>

snes_t machine; /* one machine per firmware, as the other runtimes' own */

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons byte the host wrote (as in the NES
 * and Sega runtimes).  snes_reset references it: --gc-sections would drop
 * it otherwise. */
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    snes_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.cpu.mdr, 0 };

uint64_t snes_ctx_key(const snes_cpu_t *cpu) {
    /* the emitter's layout: K, DBR, the direct page, then M (bit 0),
     * X (bit 1), E (bit 2) */
    return (uint64_t)cpu->k | (uint64_t)cpu->dbr << 8 | (uint64_t)(cpu->d & 0xFF) << 16 |
           (uint64_t)(cpu->d >> 8) << 24 |
           (uint64_t)(((cpu->p & SNES_P_M ? 1u : 0u) | (cpu->p & SNES_P_X ? 2u : 0u) |
                       (cpu->e ? 4u : 0u))) << 32;
}

/* the cartridge's windows: LoROM banks repeat every $400000, HiROM every
 * $400000 as well, the mirrors of $40-$7D and $C0-$FF only in HiROM */
static uint32_t rom_offset(const snes_cpu_t *c, uint32_t at) {
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    if (c->hirom) return (uint32_t)(bank & 0x3F) * 0x10000u + off;
    return (uint32_t)(bank & 0x3F) * 0x8000u + (off & 0x7FFFu);
}

/* the I/O page $2000-$5FFF of banks $00-$3F and $80-$BF.  Phase 1: the
 * NMI register, the WRAM port; the PPU and APU ports write nowhere and
 * read open bus. */
static uint8_t io_read(uint16_t at) {
    snes_cpu_t *c = &machine.cpu;
    switch (at) {
    case 0x4016: case 0x4017: /* the controller ports: nothing pressed */
        return 0xFF;
    case 0x4210: { /* RDNMI: the VBlank flag (cleared by the read), CPU 2 */
        const uint8_t v = (uint8_t)((c->vblank_flag ? 0x80 : 0) | 0x02);
        c->vblank_flag = 0;
        return v;
    }
    case 0x4211: case 0x4212: return 0; /* the IRQ/time flags: no source */
    case 0x2180: { /* the WRAM port */
        const uint8_t v = machine.wram[c->wram_addr & 0x1FFFF];
        c->wram_addr = (c->wram_addr + 1) & 0x1FFFF;
        return v;
    }
    case 0x2181: return (uint8_t)c->wram_addr;
    case 0x2182: return (uint8_t)(c->wram_addr >> 8);
    case 0x2183: return (uint8_t)(c->wram_addr >> 16);
    case 0x4218: case 0x4219: case 0x421A: case 0x421B:
    case 0x421C: case 0x421D: case 0x421E: case 0x421F:
        return 0xFF; /* the joypad registers: all buttons released */
    default: return c->mdr; /* open bus */
    }
}

static void io_write(uint16_t at, uint8_t v) {
    snes_cpu_t *c = &machine.cpu;
    switch (at) {
    case 0x4200: /* NMI and the IRQ enables: only the NMI has a source */
        c->nmi_enable = (v & 0x80) != 0;
        if (!c->nmi_enable) c->nmi_pending = 0;
        break;
    case 0x2181: c->wram_addr = (c->wram_addr & 0x1FF00) | v; break;
    case 0x2182: c->wram_addr = (c->wram_addr & 0x100FF) | (uint32_t)v << 8; break;
    case 0x2183: c->wram_addr = (c->wram_addr & 0x0FFFF) | (uint32_t)(v & 1) << 16; break;
    default: break; /* the PPU, the APU, the DMA: written nowhere in phase 1 */
    }
}

uint8_t snes_bus_read(uint32_t at) {
    snes_cpu_t *c = &machine.cpu;
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    uint8_t v;
    if (bank >= 0x7E) { /* $7E0000-$7FFFFF, the whole 128 KiB */
        v = machine.wram[at & 0x1FFFF];
    } else if (off < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        v = machine.wram[at & 0x1FFF]; /* the WRAM mirror */
    } else if (off >= 0x2000 && off < 0x8000 &&
               (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        v = off < 0x6000 ? io_read(off) : c->mdr; /* $6000-$7FFF: SRAM, phase 1 open bus */
    } else if ((bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0) {
        v = c->hirom ? c->rom[rom_offset(c, at) % (c->rom_bytes ? c->rom_bytes : 1)] : c->mdr;
    } else { /* banks $00-$3F and $80-$BF from $8000: the cartridge */
        const uint32_t fo = rom_offset(c, at);
        v = fo < c->rom_bytes ? c->rom[fo] : c->mdr;
    }
    c->mdr = v;
    return v;
}

void snes_bus_write(uint32_t at, uint8_t v) {
    snes_cpu_t *c = &machine.cpu;
    c->mdr = v;
    const unsigned bank = at >> 16, off = at & 0xFFFF;
    if (bank >= 0x7E) {
        machine.wram[at & 0x1FFFF] = v;
    } else if (off < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        machine.wram[at & 0x1FFF] = v;
    } else if (off >= 0x2000 && off < 0x8000 &&
               (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        if (off < 0x6000) io_write(off, v);
    }
    /* cartridge writes: nothing in phase 1 */
}

void snes_init(const uint8_t *rom, uint32_t rom_bytes, int hirom) {
    snes_cpu_t *c = &machine.cpu;
    memset(&machine, 0, sizeof machine);
    c->wram = machine.wram;
    c->rom = rom;
    c->rom_bytes = rom_bytes;
    c->hirom = hirom;
    snes_reset();
}

void snes_reset(void) {
    snes_cpu_t *c = &machine.cpu;
    machine.frames = 0;
    __asm__ volatile("" : : "r"(&snes_input_probe));
    c->a = c->x = c->y = 0;
    c->s = 0x01FF;
    c->d = 0;
    c->k = 0;
    c->dbr = 0;
    c->p = SNES_P_I | SNES_P_M | SNES_P_X;
    c->e = 1;
    c->cycles = 0;
    c->mdr = 0;
    c->nmi_enable = 0;
    c->vblank_flag = 0;
    c->nmi_pending = 0;
    c->irq_line = 0;
    c->halted = 0;
    c->wram_addr = 0;
    /* the reset vector, always of bank $00 in emulation mode */
    c->pc = (uint16_t)(snes_bus_read(0x00FFFC) | snes_bus_read(0x00FFFD) << 8);
}

/* the interrupt sequence shared by the dispatcher and the interpreter:
 * the B bit on the stack is clear (BRK/COP/PHP push it set), D is
 * cleared, S stays in page $01 in the emulation mode */
void snes_take_interrupt(snes_cpu_t *c, uint16_t vector) {
    snes_bus_write(0x000000u | c->s, (uint8_t)(c->pc >> 8));
    c->s = c->e ? (uint16_t)(0x0100u | ((c->s - 1) & 0xFF)) : (uint16_t)(c->s - 1);
    snes_bus_write(0x000000u | c->s, (uint8_t)c->pc);
    c->s = c->e ? (uint16_t)(0x0100u | ((c->s - 1) & 0xFF)) : (uint16_t)(c->s - 1);
    snes_bus_write(0x000000u | c->s, (uint8_t)(c->e ? (c->p | 0x30) & ~SNES_P_X : (c->p & ~SNES_P_X)));
    c->s = c->e ? (uint16_t)(0x0100u | ((c->s - 1) & 0xFF)) : (uint16_t)(c->s - 1);
    c->p |= SNES_P_I;
    c->p &= (uint8_t)~SNES_P_D;
    if (c->e) c->s = (uint16_t)(0x0100u | (c->s & 0xFF));
    c->pc = (uint16_t)(snes_bus_read(0x000000u | vector) |
                       snes_bus_read(0x000000u | (uint32_t)(vector + 1)) << 8);
}

/* the NMI's edge, the frame's only interrupt source in phase 1: the flag
 * and the NMI land at the frame's end, the CPU takes them at its next
 * instruction boundary */
void snes_frame(void) {
    machine.frames++;
    snes_run(&machine.cpu, machine.cpu.cycles + SNES_FRAME_CYCLES);
    machine.cpu.vblank_flag = 1;
    if (machine.cpu.nmi_enable) machine.cpu.nmi_pending = 1;
}
