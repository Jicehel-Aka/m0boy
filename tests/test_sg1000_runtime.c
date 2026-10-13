/* SG-1000 runtime: bus, VDP ports, input, interrupts, rendering (device
 * renderer against the reference), HALT timing and PSG output. */
#include "sg1000.h"
#include <stdio.h>
#include <string.h>
#define REQUIRE(v) do { if (!(v)) { fprintf(stderr, "line %d: %s\n", __LINE__, #v); return 1; } } while (0)

static uint8_t rom[16384], cart[SG_CART_RAM_SIZE];
static unsigned stop_at;
static void frame_stop(void *ctx) { (void)ctx; if (machine.frames >= stop_at) machine.stop = 1; }
static void reg(unsigned n, uint8_t value) { sg1000_out(0xbf, value); sg1000_out(0xbf, (uint8_t)(0x80 | n)); }
static void address(unsigned a, unsigned write) { sg1000_out(0xbf, (uint8_t)a); sg1000_out(0xbf, (uint8_t)((a >> 8) | (write ? 0x40 : 0))); }
static void run_frames(unsigned n) { stop_at = machine.frames + n; machine.stop = 0; machine.frame = frame_stop; sg1000_run(); }
static uint32_t lcg = 12345;
static uint8_t rnd(void) { lcg = lcg * 1103515245u + 12345u; return (uint8_t)(lcg >> 16); }

/* Device line = reference line sampled at tile pixels 0 1 3 4 6. */
static int out_matches(unsigned y) {
    static const uint8_t sample[5] = { 0, 1, 3, 4, 6 };
    uint8_t ref[SG_WIDTH];
    uint16_t out[SG_OUT_WIDTH];
    sg1000_render_line(y, ref);
    sg1000_render_out(y, out);
    for (unsigned i = 0; i < SG_OUT_WIDTH; i++)
        if (out[i] != machine.palette[ref[(i / 5) * 8 + sample[i % 5]]]) return 0;
    return 1;
}

int main(void) {
    /* bus: ROM mirrored, RAM mirrored, ROM writes ignored */
    rom[0] = 0x76; rom[1] = 0x42;
    sg1000_init(rom, sizeof rom, 0);
    REQUIRE(sg1000_read(0x4001) == 0x42);
    sg1000_write(0xc012, 0x34); REQUIRE(sg1000_read(0xfc12) == 0x34);
    sg1000_write(0, 0); REQUIRE(sg1000_read(0) == 0x76);
    static uint8_t irregular[40960];
    irregular[1] = 7;
    sg1000_init(irregular, sizeof irregular, 0);
    REQUIRE(sg1000_read(1) == 7 && sg1000_read(0xa001) == 0xff);
    /* cartridge RAM: 8 KiB mirrored through 8000-BFFF */
    sg1000_init(rom, sizeof rom, cart);
    sg1000_write(0x8123, 0x5a); REQUIRE(sg1000_read(0xa123) == 0x5a && machine.cart_ram_used);

    /* VDP data port with read-ahead, control latch */
    sg1000_init(rom, sizeof rom, 0);
    address(0x3fff, 1); sg1000_out(0xbe, 0x12); sg1000_out(0xbe, 0x34);
    REQUIRE(machine.vram[0x3fff] == 0x12 && machine.vram[0] == 0x34);
    address(0x3fff, 0); REQUIRE(sg1000_in(0xbe) == 0x12 && sg1000_in(0xbe) == 0x34);
    sg1000_out(0xbf, 0); REQUIRE(machine.vdp_pending); (void)sg1000_in(0xbf); REQUIRE(!machine.vdp_pending);
    /* input, pause edge */
    sg1000_buttons(0, 0x11); sg1000_buttons(1, 0x25);
    REQUIRE(sg1000_in(0xdc) == 0xae && sg1000_in(0xdd) == 0xf6);
    sg1000_pause(1); REQUIRE(machine.cpu.nmi); machine.cpu.nmi = 0; sg1000_pause(1); REQUIRE(!machine.cpu.nmi);
    sg1000_pause(0); sg1000_pause(1); REQUIRE(machine.cpu.nmi);

    /* VBlank IRQ, acknowledged by a status read; register 1 gates it */
    sg1000_reset(); reg(1, 0x60);
    machine.cyc = SG_VBLANK_CYCLES; sg_sync();
    REQUIRE(machine.cpu.irq && (machine.vdp_status & 128) && machine.render_line == SG_HEIGHT);
    REQUIRE(sg1000_in(0xbf) & 128); REQUIRE(!machine.cpu.irq && !(machine.vdp_status & 128));
    machine.vdp_status |= 128; reg(1, 0x40); REQUIRE(!machine.cpu.irq); reg(1, 0x60); REQUIRE(machine.cpu.irq);

    /* reference renderer, sprite priority and coincidence */
    sg1000_reset(); reg(1, 0x40); reg(2, 6); reg(3, 0x80); reg(4, 0); reg(5, 0x36); reg(7, 1);
    machine.vram[0] = 0x80; machine.vram[0x2000] = 0xf2; machine.vram[0x1b00] = 208; machine.sprites_dirty = 1;
    uint8_t pixels[SG_WIDTH];
    sg1000_render_line(0, pixels); REQUIRE(pixels[0] == 15 && pixels[1] == 2);
    REQUIRE(out_matches(0));
    machine.vram[0x1b00] = 255; machine.vram[0x1b01] = 0; machine.vram[0x1b02] = 1; machine.vram[0x1b03] = 3;
    machine.vram[0x1b04] = 255; machine.vram[0x1b05] = 0; machine.vram[0x1b06] = 1; machine.vram[0x1b07] = 4;
    machine.vram[0x1b08] = 208; machine.vram[8] = 128; machine.sprites_dirty = 1;
    sg1000_render_line(0, pixels); REQUIRE(pixels[0] == 3);
    REQUIRE(out_matches(0));
    /* status comes from lines rendered by the catch-up, drawn or not */
    machine.vdp_status = 31; machine.cyc = SG_LINE_CYCLES; sg1000_vdp_catchup();
    REQUIRE(machine.render_line == 1 && (machine.vdp_status & 32));
    /* a sprite moved through the data port is seen on the next line */
    address(0x1b00, 1); sg1000_out(0xbe, 20);
    REQUIRE(machine.sprites_dirty);
    sg1000_render_line(21, pixels); REQUIRE(pixels[0] == 3);

    /* device renderer = reference, Graphics I and II, random VRAM */
    for (unsigned mode = 0; mode < 2; mode++) {
        sg1000_reset();
        for (unsigned i = 0; i < 16384; i++) machine.vram[i] = rnd();
        for (unsigned i = 0; i < 32; i++) machine.vram[0x3b00 + i * 4] = (uint8_t)(rnd() % 200);
        machine.vram[0x3b00 + 32 * 4] = 208;
        reg(0, mode ? 2 : 0); reg(1, 0x63); reg(2, 0x0e); reg(3, mode ? 0xff : 0x80);
        reg(4, mode ? 3 : 0); reg(5, 0x76); reg(6, 0); reg(7, 0x14);
        for (unsigned y = 0; y < SG_HEIGHT; y++) REQUIRE(out_matches(y));
    }

    /* HALT with interrupts off: whole refresh periods up to each event */
    rom[0x66] = 0xed; rom[0x67] = 0x45; /* NMI handler: RETN */
    sg1000_init(rom, sizeof rom, 0);
    run_frames(1);
    REQUIRE(machine.cpu.halted && machine.cyc == SG_FRAME_CYCLES && machine.cpu.r == ((SG_FRAME_CYCLES / 4) & 127));
    machine.cpu.nmi = 1; machine.next_evt = machine.cyc; machine.stop = 0;
    REQUIRE(!sg_event(1) && machine.cpu.pc == 1 && !machine.cpu.halted && machine.cyc == SG_FRAME_CYCLES + 11 + 14);

    /* PSG: samples follow CPU time; ~440 Hz; a mid-frame mute is not retroactive */
    sg1000_init(rom, sizeof rom, 0); sg1000_audio_rate(22050);
    sg1000_out(0x7f, 0x8e); sg1000_out(0x7f, 15); sg1000_out(0x7f, 0x90);
    uint8_t audio[512];
    unsigned total = 0, edges = 0;
    uint8_t previous = 128;
    for (unsigned f = 0; f < 60; f++) {
        run_frames(1);
        unsigned n = sg1000_audio_available(); total += n;
        sg1000_audio_render(audio, n);
        for (unsigned i = 0; i < n; i++) { if (previous < 128 && audio[i] > 128) edges++; previous = audio[i]; }
    }
    REQUIRE(total == (uint64_t)machine.cyc * 22050 / 3579545);
    REQUIRE(edges >= 437 && edges <= 442);
    sg1000_init(rom, sizeof rom, 0); sg1000_audio_rate(22050);
    sg1000_out(0x7f, 0x8e); sg1000_out(0x7f, 15); sg1000_out(0x7f, 0x90);
    machine.cyc = 100 * SG_LINE_CYCLES;
    sg1000_out(0x7f, 0x9f); /* catch-up to line 100, then mute */
    unsigned audible = sg1000_audio_available();
    run_frames(1);
    unsigned n = sg1000_audio_available(); sg1000_audio_render(audio, n);
    REQUIRE(audible > 0 && n > audible);
    for (unsigned i = 0; i < audible; i++) REQUIRE(audio[i] != 128);
    for (unsigned i = audible; i < n; i++) REQUIRE(audio[i] == 128);
    /* noise: deterministic reset, both output levels */
    sg1000_init(rom, sizeof rom, 0); sg1000_audio_rate(22050);
    sg1000_out(0x7f, 0xe4); sg1000_out(0x7f, 0xf0);
    run_frames(1);
    n = sg1000_audio_available(); sg1000_audio_render(audio, n);
    unsigned low = 0, high = 0;
    for (unsigned i = 0; i < n; i++) { low += audio[i] < 128; high += audio[i] > 128; }
    REQUIRE(low && high);
    sg1000_out(0x7f, 0xe0); REQUIRE(machine.audio_lfsr == 0x10000);
    sg1000_reset(); REQUIRE(machine.audio_head == 0 && machine.audio_tail == 0);
    sg1000_audio_render(audio, 512);
    for (unsigned i = 0; i < 512; i++) REQUIRE(audio[i] == 128);
    puts("SG-1000 bus, VDP ports, IRQ, input, rendering, HALT and PSG: ok");
    return 0;
}
