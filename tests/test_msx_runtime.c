/* MSX runtime on the synthetic cartridge of tests/msx_fixture.py (path given
 * as the argument): what the simulated BIOS, the slots, the PPI, the AY
 * registers and the VDP leave in the machine after 30 frames with the pad's
 * UP and button 1 held. */
#include "sg1000.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int frames;
static uint16_t out_row[SG_OUT_WIDTH];
static void frame(void *c) { (void)c; if (++frames >= 30) machine.stop = 1; }
static void row(void *c, unsigned y, uint16_t *p) { (void)c; (void)y; (void)p; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint8_t rom[0x8000];
    const size_t size = fread(rom, 1, sizeof rom, f);
    fclose(f);
    for (size_t i = size; i < sizeof rom; i++) rom[i] = 0xff;
    sg1000_init(rom, size, 0);
    CHECK(machine.msx_base == 0x4000 && machine.msx_init == 0x4010 && machine.cpu.pc == 0x4010, "layout %04x init %04x", machine.msx_base, machine.msx_init);
    CHECK(sg1000_read(0x4000) == 'A' && sg1000_read(0x8000) == 0xff && sg1000_read(0x0006) == 0x98, "boot mapping");
    sg1000_write(0x4000, 0); /* ROM */
    sg1000_write(0xe000, 0x5a);
    CHECK(sg1000_read(0x4000) == 'A' && sg1000_read(0xe000) == 0x5a, "ROM is read-only, RAM is not");
    sg1000_reset();
    machine.row = row; machine.row_out = out_row; machine.frame = frame;
    machine.buttons[0] = 0x11;
    sg1000_run();
    CHECK(sg1000_read(0xe004) == 1, "GTSTCK up: %u", sg1000_read(0xe004));
    CHECK(sg1000_read(0xe005) == 0xff, "GTTRIG pressed: %02x", sg1000_read(0xe005));
    CHECK(sg1000_read(0xe006) == 0xde, "SNSMAT row 8 (space, up): %02x", sg1000_read(0xe006));
    CHECK(sg1000_read(0xe008) == 0xde, "PPI row 8: %02x", sg1000_read(0xe008));
    CHECK(sg1000_read(0xe007) == 0xee, "PSG R14 joystick: %02x", sg1000_read(0xe007));
    CHECK(sg1000_read(0xe010) == 0xf4 && sg1000_read(0xe012) == 0xf4, "slot register");
    CHECK(sg1000_read(0xe011) == 0xff, "an empty page reads FF");
    CHECK(sg1000_read(0xe013) == 15 && sg1000_read(0xe014) == 0x98 && sg1000_read(0xe016) == 0x5a, "variables, CALSLT");
    CHECK(sg1000_read(0xe020) == 1, "RDVRM");
    CHECK((sg1000_read(0xe030) | sg1000_read(0xe031) << 8) == 0x1234 && (sg1000_read(0xe032) | sg1000_read(0xe033) << 8) == 0x5678,
          "IX and IY survive the interrupt routine: %02x%02x %02x%02x", sg1000_read(0xe031), sg1000_read(0xe030), sg1000_read(0xe033), sg1000_read(0xe032));
    const unsigned jiffy = sg1000_read(0xfc9e) | sg1000_read(0xfc9f) << 8;
    CHECK(jiffy >= 20 && jiffy <= 30 && sg1000_read(0xe002) == (jiffy & 255), "JIFFY %u, hook count %u", jiffy, sg1000_read(0xe002));
    CHECK(sg1000_read(0xe003) & 0x80, "STATFL keeps the frame flag: %02x", sg1000_read(0xe003));
    CHECK(sg1000_read(0xfcaf) == 2 && machine.vdp[0] == 2 && machine.vdp[1] == 0xe0 && machine.vdp[2] == 0x06 && machine.vdp[3] == 0xff && machine.vdp[4] == 3 && machine.vdp[5] == 0x36 && machine.vdp[6] == 7, "INIGRP registers");
    CHECK(machine.vram[8] == 0xaa && machine.vram[0x2008] == 0xf1 && machine.vram[0x1800 + 33] == 1 && machine.vram[0x1840 + 39] == 1 && machine.vram[0x1840 + 40] == 104, "VRAM through LDIRVM, WRTVRM, FILVRM");
    CHECK(machine.vram[0x1b00] == 60 && machine.vram[0x1b02] == 0, "sprite attributes");
    CHECK(machine.ay_reg[0] == 0xfe && machine.ay_reg[3] == 1 && machine.ay_reg[7] == 0xb8 && machine.ay_reg[13] == 0x0e && machine.ay_reg[15] == 0x8f, "AY registers");
    CHECK(machine.ppi_a == 0xf4, "PPI A");
    if (argc > 2) { /* the text fixture: CHPUT, POSIT and the font */
        f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 2; }
        const size_t text_size = fread(rom, 1, sizeof rom, f);
        fclose(f);
        for (size_t i = text_size; i < sizeof rom; i++) rom[i] = 0xff;
        frames = 0;
        machine.stop = 0;
        sg1000_init(rom, text_size, 0);
        machine.row = row; machine.row_out = out_row; machine.frame = frame;
        sg1000_run();
        CHECK(machine.vram[0x1800] == 'H' && machine.vram[0x1801] == 'I', "CHPUT at the cursor");
        CHECK(machine.vram[0x1800 + 32] == 'O' && machine.vram[0x1800 + 33] == 'K', "CR LF");
        CHECK(machine.vram[0x1800 + 2 * 32 + 4] == 'Z', "POSIT then CHPUT");
        CHECK(machine.vram['H' * 8 + 2] == 0xf8 && machine.vram['H' * 8 + 3] == 0x88, "the font is loaded: %02x %02x", machine.vram['H' * 8 + 2], machine.vram['H' * 8 + 3]);
        CHECK(sg1000_read(0xf3dd) == 6 && sg1000_read(0xf3dc) == 3, "cursor variables");
    }
    printf(failures ? "msx runtime: %d failure(s)\n" : "msx runtime: ok\n", failures);
    return failures != 0;
}
