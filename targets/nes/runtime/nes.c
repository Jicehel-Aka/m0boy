#include "nes.h"

nes_t machine; /* the CPU's bus lands here (one machine per firmware, as
                * the Z80 runtime's own machine) */

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons byte the host wrote (as in the SG-1000
 * and Game Gear runtimes), so a session played in gm0 replays on the host
 * harness (--input).  nes_reset references it: --gc-sections would drop it
 * otherwise. */
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    nes_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.input, 0 };

uint8_t nes_bus_read(uint16_t at) {
    nes_t *nes = &machine;
    if (at < 0x2000) return nes->ram[at & 0x7FF];
    if (at < 0x4000) return nes_ppu_read(&nes->ppu, at & 7);
    if (at == 0x4015) return nes_apu_read(&nes->apu);
    if (at == 0x4016 || at == 0x4017) {
        const unsigned pad = at & 1;
        if (nes->strobe) return (uint8_t)(nes->input & 1);
        const uint8_t bit = (uint8_t)(nes->shift[pad] & 1);
        nes->shift[pad] >>= 1;
        /* past the 8 buttons the pads read 0 (SMB keys its title timers
         * on that signature) */
        return bit;
    }
    if (at >= 0x8000) {
        /* NROM: a 32 KiB PRG fills $8000-$FFFF, a 16 KiB one mirrors */
        const uint32_t mask = nes->prg_size - 1; /* sizes are powers of two */
        return nes->prg[(uint32_t)(at - 0x8000) & mask];
    }
    return 0; /* the APU's registers and the expander read open bus */
}

void nes_bus_write(uint16_t at, uint8_t value) {
    nes_t *nes = &machine;
    if (at < 0x2000) { nes->ram[at & 0x7FF] = value; return; }
    if (at < 0x4000) { nes_ppu_write(&nes->ppu, at & 7, value); return; }
    if (at == 0x4014) { /* OAM DMA: 256 bytes, the CPU stalls for it */
        for (unsigned i = 0; i < 256; i++)
            nes->oam[(unsigned)((nes->ppu.oam_addr + i) & 0xFF)] = nes_bus_read((uint16_t)((value << 8) | i));
        nes6502_stall(&nes->cpu, 513);
        nes->ppu.oam_dirty = 1;
        return;
    }
    if (at == 0x4016) {
        if ((value & 1) && !nes->strobe) {
            nes->shift[0] = nes->input;
            nes->shift[1] = 0;
        }
        nes->strobe = value & 1;
        return;
    }
    if (at < 0x4018) { nes_apu_write(&nes->apu, at - 0x4000, value); return; }
}

#ifdef NES_PPU_REFERENCE
static void row_bridge(void *host, unsigned line) {
    nes_t *nes = (nes_t *)host;
    if (nes->row) nes->row(nes->host, line, nes->ppu.row);
}
#endif

void nes_init(const uint8_t *prg, uint32_t prg_bytes,
              const uint8_t *chr, uint32_t chr_bytes, int vertical_mirror) {
    nes_t *nes = &machine;
    for (unsigned i = 0; i < sizeof nes->ram; i++) nes->ram[i] = 0;
    for (unsigned i = 0; i < sizeof nes->oam; i++) nes->oam[i] = 0;
    static uint8_t nametable[2048], palette[32];
    for (unsigned i = 0; i < sizeof nametable; i++) nametable[i] = 0;
    for (unsigned i = 0; i < sizeof palette; i++) palette[i] = 0;
    nes->prg = prg;
    nes->prg_size = prg_bytes;
    nes->chr = chr_bytes >= 8192 ? chr : 0;
    /* the CPU's fast paths (6502.c): RAM and the NROM window */
    nes->cpu.ram = nes->ram;
    nes->cpu.prg = prg;
    nes->cpu.prg_mask = prg_bytes - 1;
    nes->ppu.nametable = nametable;
    nes->ppu.palette = palette;
    nes->ppu.oam = nes->oam;
    nes->ppu.chr = nes->chr;
    nes->ppu.vertical_mirror = vertical_mirror != 0;
#ifdef NES_PPU_REFERENCE
    nes->ppu.render_row = row_bridge;
    nes->row = 0;
#endif
    nes->ppu.panel = 0;
    nes_ppu_set_rows(&nes->ppu, 128); /* the frontend may pick another */
    nes->ppu.skip = 0;
    nes->ppu.host = nes;
    nes->host = 0;
    nes_reset();
}

void nes_reset(void) {
    nes_t *nes = &machine;
    __asm__ volatile("" : : "r"(&nes_input_probe));
    nes6502_reset(&nes->cpu);
    nes_ppu_reset(&nes->ppu);
    nes_apu_reset(&nes->apu);
    nes->strobe = 0;
    nes->shift[0] = nes->shift[1] = 0;
    nes->dots = 0;
    nes->line = 0;
}

void nes_frame(void) {
    nes_t *nes = &machine;
    nes->frames++;
    /* the strobe takes the pads' state once, at the frame's start */
    if (nes->strobe) { nes->shift[0] = nes->input; }
    for (unsigned line = 0; line < 262; line++) {
        /* 341 PPU dots = 113 CPU cycles and two thirds: the thirds add up
         * across lines, as they do on the machine (341 = 113*3 + 2, no
         * division: ARMv6-M would call __aeabi_uidiv) */
        nes->line = line;
        uint32_t step = 113u;
        nes->dots += 2u;
        if (nes->dots >= 3u) { nes->dots -= 3u; step++; }
        const uint32_t cpu_until = nes->cpu.cycles + step;
        nes6502_run(&nes->cpu, cpu_until);
        if (nes_ppu_line(&nes->ppu, line)) nes->cpu.nmi_edge = 1;
    }
}

void nes_audio(uint8_t *dst, unsigned n, unsigned rate) {
    nes_apu_render(&machine.apu, dst, n, rate);
}

/* the consoles' audio drivers (meta_audio.cpp and its like) call these two
 * without knowing the machine; the samples are rendered at the rate the
 * driver plays (the META's 22049 Hz: a frame asks for 367 of them) */
static unsigned audio_rate = 11025u;
void nes_audio_set_rate(unsigned rate) { audio_rate = rate ? rate : 11025u; }
void nes_audio_render(uint8_t *buffer, unsigned n) {
    nes_apu_render(&machine.apu, buffer, n, audio_rate);
}
