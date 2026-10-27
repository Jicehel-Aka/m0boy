/* Bus mirrors, WRAM port and 65C816 interrupt stack frames. */
#include "../targets/snes/runtime/snes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static uint8_t rom[0x10000];
static uint8_t image[256 * 224 * 3];

static void video_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    /* DMA mode 1 interleaves VMDATAL/VMDATAH and advances the word address. */
    machine.wram[0x100] = 0x80;
    machine.wram[0x101] = 0x00;
    machine.wram[0x102] = 0x33;
    machine.wram[0x103] = 0x44;
    snes_bus_write(0x2115, 0x80);
    snes_bus_write(0x2116, 0x10);
    snes_bus_write(0x2117, 0);
    snes_bus_write(0x4300, 1);
    snes_bus_write(0x4301, 0x18);
    snes_bus_write(0x4302, 0);
    snes_bus_write(0x4303, 1);
    snes_bus_write(0x4304, 0x7e);
    snes_bus_write(0x4305, 4);
    snes_bus_write(0x4306, 0);
    snes_bus_write(0x420b, 1);
    CHECK(machine.vram[32] == 0x80 && machine.vram[33] == 0);
    CHECK(machine.vram[34] == 0x33 && machine.vram[35] == 0x44);
    CHECK(machine.dma[0][2] == 4 && machine.dma[0][3] == 1);
    CHECK(machine.dma[0][5] == 0 && machine.dma[0][6] == 0);
    snes_bus_write(0x2116, 0x10);
    snes_bus_write(0x2117, 0);
    CHECK(snes_bus_read(0x2139) == 0x80);
    CHECK(snes_bus_read(0x213a) == 0);
    /* Mode 1 BG1, tile 1 at the first map entry, palette color 1 red. */
    snes_bus_write(0x2116, 0);
    snes_bus_write(0x2117, 4);
    snes_bus_write(0x2118, 1);
    snes_bus_write(0x2119, 0);
    snes_bus_write(0x2121, 1);
    snes_bus_write(0x2122, 0x1f);
    snes_bus_write(0x2122, 0);
    snes_bus_write(0x2105, 1);
    snes_bus_write(0x2107, 4);
    snes_bus_write(0x212c, 1);
    snes_bus_write(0x2100, 15);
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);
    CHECK(image[3] == 0 && image[4] == 0 && image[5] == 0);
    snes_bus_write(0x2100, 0x8f);
    snes_render(image);
    CHECK(image[0] == 0 && image[1] == 0 && image[2] == 0);
}

static void object_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    for (unsigned i = 0; i < 128; i++) machine.oam[i * 4 + 1] = 224;

    /* Low OAM commits only after both writes; the high table mirrors. */
    snes_bus_write(0x2102, 0);
    snes_bus_write(0x2103, 0);
    snes_bus_write(0x2104, 7);
    CHECK(machine.oam[0] == 0);
    snes_bus_write(0x2104, 9);
    CHECK(machine.oam[0] == 7 && machine.oam[1] == 9);
    snes_bus_write(0x2102, 0x10);
    snes_bus_write(0x2103, 1);
    snes_bus_write(0x2104, 0x5a);
    CHECK(machine.oam[512] == 0x5a);
    snes_bus_write(0x2102, 0);
    snes_bus_write(0x2103, 1);
    CHECK(snes_bus_read(0x2138) == 0x5a);
    snes_bus_write(0x2102, 0);
    snes_bus_write(0x2103, 1);
    snes_bus_write(0x2104, 0); /* two small, positive-X objects */

    /* DMA two objects to OAM. Sprite 0 covers sprite 1 despite lower
     * background priority; both use a one-pixel 4bpp tile. */
    const uint8_t objects[] = {0,0,1,0x00, 0,0,1,0x32};
    for (unsigned i = 0; i < sizeof objects; i++) machine.wram[0x100 + i] = objects[i];
    snes_bus_write(0x2102, 0); snes_bus_write(0x2103, 0);
    snes_bus_write(0x4300, 0); snes_bus_write(0x4301, 4);
    snes_bus_write(0x4302, 0); snes_bus_write(0x4303, 1);
    snes_bus_write(0x4304, 0x7e); snes_bus_write(0x4305, 8); snes_bus_write(0x4306, 0);
    snes_bus_write(0x420b, 1);
    CHECK(machine.oam[0] == 0 && machine.oam[1] == 0 && machine.oam[6] == 1);
    machine.vram[32] = 0x80;
    machine.cgram[129 * 2] = 0x1f; /* red */
    machine.cgram[145 * 2] = 0xe0; machine.cgram[145 * 2 + 1] = 3; /* green */
    snes_bus_write(0x2100, 15); snes_bus_write(0x2105, 1); snes_bus_write(0x212c, 0x10);
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);
    /* Rotation selects sprite 1 first. */
    snes_bus_write(0x2102, 2); snes_bus_write(0x2103, 0x80);
    snes_render(image);
    CHECK(image[0] == 0 && image[1] == 255 && image[2] == 0);
    /* Horizontal flip moves the sole pixel from x=0 to x=7. */
    machine.oam[7] |= 0x40;
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);
    CHECK(image[7 * 3] == 0 && image[7 * 3 + 1] == 255);

    /* BG1 low priority hides OBJ priority 0; OBJ priority 3 hides BG1. */
    machine.oam[7] = 0x02;
    machine.oam[3] = 0;
    machine.vram[0x800] = 1;
    machine.cgram[2] = 0; machine.cgram[3] = 0x7c; /* blue */
    snes_bus_write(0x2107, 4); snes_bus_write(0x212c, 0x11);
    snes_bus_write(0x2102, 0); snes_bus_write(0x2103, 0);
    snes_render(image);
    CHECK(image[0] == 0 && image[1] == 0 && image[2] == 255);
    machine.oam[3] = 0x30;
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);

    /* Signed X and large 16x16 objects use the high OAM table. */
    machine.oam[5] = 224;
    machine.oam[0] = 252; machine.oam[1] = 0; machine.oam[2] = 1;
    machine.oam[512] = 1; /* x = -4 */
    machine.vram[32] = 0x08; /* pixel 4 */
    snes_bus_write(0x212c, 0x10);
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);
    machine.oam[0] = 8; machine.oam[512] = 2; /* large, x = 8 */
    machine.vram[32] = 0;
    machine.vram[18 * 32] = 0x80; /* bottom-right tile */
    snes_render(image);
    unsigned pixel = (8 * 256 + 16) * 3;
    CHECK(image[pixel] == 255 && image[pixel + 1] == 0 && image[pixel + 2] == 0);
}

static void interrupt_test(int emulation, int interpreter) {
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = emulation;
    machine.cpu.k = 0x81;
    machine.cpu.pc = 0x9234;
    machine.cpu.s = emulation ? 0x01ff : 0x1fff;
    machine.cpu.p = SNES_P_X | SNES_P_D;
    if (interpreter) {
        machine.cpu.nmi_pending = 1;
        snes_run(&machine.cpu, emulation ? 7 : 8);
    } else snes_take_interrupt(&machine.cpu, emulation ? 0xfffa : 0xffea);
    CHECK(machine.cpu.k == 0);
    CHECK(machine.cpu.pc == 0x8000);
    CHECK(machine.cpu.s == (emulation ? 0x01fc : 0x1ffb));
    CHECK(machine.wram[emulation ? 0x01ff : 0x1ffe] == 0x92);
    CHECK(machine.wram[emulation ? 0x01fe : 0x1ffd] == 0x34);
    CHECK(machine.wram[emulation ? 0x01fd : 0x1ffc] == (emulation ? 0x28 : 0x18));
    if (!emulation) CHECK(machine.wram[0x1fff] == 0x81);
    CHECK((machine.cpu.p & (SNES_P_I | SNES_P_D)) == SNES_P_I);
}

static void cpu_flow_test(void) {
    /* Fetch all JML operands from the original bank before switching. */
    rom[0] = 0x5c; rom[1] = 0x34; rom[2] = 0x92; rom[3] = 0x81;
    rom[0x8001] = 0x78; rom[0x8002] = 0x56;
    snes_init(rom, sizeof rom, 0, 0);
    snes_run(&machine.cpu, 4);
    CHECK(machine.cpu.k == 0x81 && machine.cpu.pc == 0x9234);
    /* JSL from bank 00 into its 80 mirror, then RTL returns to bank 00. */
    rom[0] = 0x22; rom[1] = 0x10; rom[2] = 0x80; rom[3] = 0x80;
    rom[0x10] = 0x6b;
    snes_init(rom, sizeof rom, 0, 0);
    snes_run(&machine.cpu, 8);
    CHECK(machine.cpu.k == 0x80 && machine.cpu.pc == 0x8010);
    CHECK(machine.cpu.s == 0x1fc);
    CHECK(machine.wram[0x1ff] == 0 && machine.wram[0x1fe] == 0x80 && machine.wram[0x1fd] == 3);
    snes_run(&machine.cpu, 14);
    CHECK(machine.cpu.k == 0 && machine.cpu.pc == 0x8004 && machine.cpu.s == 0x1ff);

    /* WAI remains asleep across slices and resumes after NMI at the next byte. */
    rom[0] = 0xcb; rom[1] = 0xea;
    snes_init(rom, sizeof rom, 0, 0);
    snes_run(&machine.cpu, 20);
    CHECK(machine.cpu.waiting && machine.cpu.pc == 0x8001);
    snes_run(&machine.cpu, 40);
    CHECK(machine.cpu.waiting && machine.cpu.pc == 0x8001);
    machine.cpu.nmi_pending = 1;
    snes_run(&machine.cpu, 47);
    CHECK(!machine.cpu.waiting && machine.cpu.pc == 0x8000);
    CHECK(machine.wram[0x1ff] == 0x80 && machine.wram[0x1fe] == 1);
}

static void joypad_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    snes_set_joypad(0x8940); /* B, up, right and X */
    snes_bus_write(0x4016, 1);
    CHECK((snes_bus_read(0x4016) & 1) == 1);
    CHECK((snes_bus_read(0x4016) & 1) == 1); /* strobe holds B */
    snes_bus_write(0x4016, 0);
    for (unsigned i = 0; i < 16; i++)
        CHECK((snes_bus_read(0x4016) & 1) == ((0x8940u >> (15 - i)) & 1));
    CHECK((snes_bus_read(0x4016) & 1) == 1);
    CHECK((snes_bus_read(0x4017) & 3) == 0);
    CHECK(snes_bus_read(0x4218) == 0 && snes_bus_read(0x4219) == 0);
    machine.cpu.halted = 1;
    snes_bus_write(0x4200, 1);
    snes_frame();
    CHECK(snes_bus_read(0x4218) == 0x40 && snes_bus_read(0x4219) == 0x89);
    CHECK((snes_bus_read(0x4212) & 1) == 0);
    snes_set_joypad(0x1000);
    CHECK(snes_bus_read(0x4219) == 0x89); /* auto result stays latched */
    snes_frame();
    CHECK(snes_bus_read(0x4218) == 0 && snes_bus_read(0x4219) == 0x10);
    /* Enabling NMI while the VBlank flag is set raises its edge. */
    snes_bus_write(0x4200, 0x81);
    CHECK(machine.cpu.nmi_pending);
}

static void cpu_index_test(void) {
    rom[0] = 0x9b; rom[1] = 0xbb; /* TXY, TYX */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0; machine.cpu.p = 0;
    machine.cpu.x = 0x8001; machine.cpu.y = 0x1234;
    snes_run(&machine.cpu, 2);
    CHECK(machine.cpu.x == 0x8001 && machine.cpu.y == 0x8001 && (machine.cpu.p & SNES_P_N));
    machine.cpu.y = 0;
    snes_run(&machine.cpu, 4);
    CHECK(machine.cpu.x == 0 && machine.cpu.y == 0 && (machine.cpu.p & SNES_P_Z));

    rom[0] = 0xb5; rom[1] = 0xf0; /* LDA $f0,X */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0; machine.cpu.p = SNES_P_M | SNES_P_X;
    machine.cpu.x = 0x30; machine.cpu.d = 0;
    machine.wram[0x20] = 0x11; machine.wram[0x120] = 0x22;
    snes_run(&machine.cpu, 4);
    CHECK((machine.cpu.a & 0xff) == 0x22); /* native crosses the page */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.x = 0x30;
    machine.wram[0x20] = 0x11; machine.wram[0x120] = 0x22;
    snes_run(&machine.cpu, 4);
    CHECK((machine.cpu.a & 0xff) == 0x11); /* emulation with aligned D wraps */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.x = 0x30; machine.cpu.d = 1;
    machine.wram[0x21] = 0x11; machine.wram[0x121] = 0x22;
    snes_run(&machine.cpu, 5);
    CHECK((machine.cpu.a & 0xff) == 0x22); /* unaligned D crosses */

    /* Wide read-modify-write must address the requested word. */
    rom[0] = 0x46; rom[1] = 0x18; rom[2] = 0x06; rom[3] = 0x1c;
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0; machine.cpu.p = 0;
    machine.wram[0x18] = 4; machine.wram[0x19] = 1; /* $0104 */
    machine.wram[0x1c] = 0x80; machine.wram[0x1d] = 1; /* $0180 */
    snes_run(&machine.cpu, 6);
    CHECK(machine.wram[0x18] == 0x82 && machine.wram[0x19] == 0);
    snes_run(&machine.cpu, 12);
    CHECK(machine.wram[0x1c] == 0 && machine.wram[0x1d] == 3);

    rom[0] = 0x04; rom[1] = 0x10; /* TSB: Z from A & old memory */
    rom[2] = 0x14; rom[3] = 0x10; /* TRB: same test before clearing */
    rom[4] = 0x89; rom[5] = 0;    /* BIT immediate leaves N and V */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.a = 1; machine.cpu.p |= SNES_P_N | SNES_P_V;
    machine.wram[0x10] = 2;
    snes_run(&machine.cpu, 5);
    CHECK(machine.wram[0x10] == 3 && (machine.cpu.p & SNES_P_Z));
    snes_run(&machine.cpu, 10);
    CHECK(machine.wram[0x10] == 2 && !(machine.cpu.p & SNES_P_Z));
    snes_run(&machine.cpu, 12);
    CHECK((machine.cpu.p & (SNES_P_N | SNES_P_V | SNES_P_Z)) == (SNES_P_N | SNES_P_V | SNES_P_Z));

    rom[0] = 0x54; rom[1] = 0x7e; rom[2] = 0; /* MVN destination, source */
    rom[0x10] = 0x61; rom[0x11] = 0x72;
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0; machine.cpu.p = 0;
    machine.cpu.a = 1; machine.cpu.x = 0x8010; machine.cpu.y = 0x234;
    snes_run(&machine.cpu, 14);
    CHECK(machine.wram[0x234] == 0x61 && machine.wram[0x235] == 0x72);
    CHECK(machine.cpu.dbr == 0x7e && machine.cpu.a == 0xffff);
    CHECK(machine.cpu.x == 0x8012 && machine.cpu.y == 0x236 && machine.cpu.pc == 0x8003);
}

/* The HDMA feeds one unit per line: a direct-mode table paints three red
 * colors then one blue into CGRAM, the repeat entry carrying three units. */
static void hdma_test(void) {
    static const uint8_t table[] = {
        0x83, 0x1f, 0x00, 0x1f, 0x00, 0x1f, 0x00, /* repeat, 3 lines of red */
        0x01, 0x00, 0x7c,                          /* one line of blue */
        0x00,                                      /* end */
    };
    memcpy(rom + 0x100, table, sizeof table);
    snes_init(rom, sizeof rom, 0, 0);
    uint8_t *d = machine.dma[0];
    d[0] = 0x02;              /* mode 2: two bytes to one register, A to B */
    d[1] = 0x22;              /* CGDATA */
    d[2] = 0x00; d[3] = 0x81; /* the table at $008100 */
    d[4] = 0x00;
    snes_bus_write(0x2121, 0); /* CGADD = 0 */
    snes_bus_write(0x420c, 1); /* channel 0 on */
    snes_frame();
    CHECK(machine.hdma_done[0] && !machine.hdma_run[0]);
    CHECK(d[8] == 0x0b && d[9] == 0x81); /* the table pointer ran through */
    CHECK(machine.cgram[0] == 0x1f && machine.cgram[1] == 0x00);
    CHECK(machine.cgram[2] == 0x1f && machine.cgram[3] == 0x00);
    CHECK(machine.cgram[4] == 0x1f && machine.cgram[5] == 0x00);
    CHECK(machine.cgram[6] == 0x00 && machine.cgram[7] == 0x7c);
    CHECK(machine.cgram[8] == 0 && machine.cgram[9] == 0);
    snes_bus_write(0x420c, 0);
}

int main(void) {
    rom[0] = 0x42;
    rom[0x7ffa] = rom[0x7fea] = rom[0x7ffc] = 0;
    rom[0x7ffb] = rom[0x7feb] = rom[0x7ffd] = 0x80;
    snes_init(rom, sizeof rom, 0, 0);
    CHECK(snes_bus_read(0x808000) == 0x42);
    hdma_test();
    /* the SRAM window: mirrored through the $6000-$7FFF offsets of the
     * LoROM banks, open bus when nothing is declared */
    snes_init(rom, sizeof rom, 0, 8192);
    CHECK(snes_bus_read(0x7f7000) == 0);
    snes_bus_write(0x006001, 0x5a);
    CHECK(machine.sram[1] == 0x5a && machine.sram_dirty);
    CHECK(snes_bus_read(0x806001) == 0x5a); /* the mirror of bank $80 */
    machine.sram_dirty = 0;
    snes_bus_write(0x7e6001, 0x99); /* bank $7E is WRAM, never the SRAM */
    CHECK(!machine.sram_dirty && machine.wram[0x6001] == 0x99);
    snes_init(rom, sizeof rom, 0, 2048);
    snes_bus_write(0x006801, 0x77);
    CHECK(machine.sram[1] == 0x77);
    CHECK(snes_bus_read(0x006001) == 0x77); /* the 2 KiB window mirrors */
    snes_init(rom, sizeof rom, 0, 0);
    snes_bus_write(0x006001, 0x44);
    CHECK(!machine.sram_dirty && machine.sram[1] == 0);
    snes_bus_write(0x800123, 0x56);
    CHECK(snes_bus_read(0x000123) == 0x56);
    snes_bus_write(0x7f0123, 0x78);
    CHECK(snes_bus_read(0x7f0123) == 0x78);
    CHECK(snes_bus_read(0x000123) == 0x56);
    snes_bus_write(0x2181, 0xff);
    snes_bus_write(0x2182, 0xff);
    snes_bus_write(0x2183, 1);
    snes_bus_write(0x2180, 0xa5);
    snes_bus_write(0x2180, 0x5a);
    CHECK(machine.wram[0x1ffff] == 0xa5 && machine.wram[0] == 0x5a);
    CHECK(machine.cpu.wram_addr == 1);
    interrupt_test(0, 0);
    interrupt_test(1, 0);
    interrupt_test(0, 1);
    interrupt_test(1, 1);
    cpu_flow_test();
    video_test();
    object_test();
    joypad_test();
    cpu_index_test();
    return 0;
}
