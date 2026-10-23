/* espboy_flash.cpp built for a Sega frontend (no gbl.h): the ROM sector
 * mapping of rom.bin, the cartridge RAM saved in flash, and the save states
 * after its copies (a stand-in machine for gg_state_io). */
#include <Arduino.h>
#include <stdio.h>
#include "gbl_espboy.h"
#include "espboy_hw.h"
#include "m0_state.h"
#include "sg_config.h"

static uint8_t fake_machine[25536];
static uint32_t fake_frame;
extern "C" uint32_t gg_state_bytes(void) { return sizeof fake_machine; }
extern "C" void gg_state_io(void (*io)(void *, uint32_t, void *), void *ctx, int) { io(fake_machine, sizeof fake_machine, ctx); }
extern "C" uint32_t gg_state_frame(void) { return fake_frame; }

MockEsp ESP;
extern "C" int gg_platform_rom_read(uint32_t sector, uint8_t *dst);
extern "C" int espboy_rom_read(uint32_t offset, uint8_t *dst, uint32_t bytes);

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void) {
    memset(ESP.flash, 0xff, sizeof ESP.flash);
    /* rom.bin at 0x100000 holds the ROM from page 1: sector 32 is its first */
    for (unsigned i = 0; i < 4096; i++) ESP.flash[ESPBOY_ROM_OFF + i] = (uint8_t)(i * 5 + 1);
    uint8_t sec[512] __attribute__((aligned(4)));
    CHECK(!gg_platform_rom_read(31, sec));
    CHECK(gg_platform_rom_read(32, sec) && sec[0] == 1 && sec[511] == (uint8_t)(511 * 5 + 1));
    CHECK(gg_platform_rom_read(34, sec) && sec[3] == (uint8_t)((1024 + 3) * 5 + 1));
    uint8_t odd[100];
    CHECK(espboy_rom_read(3, odd, 100) && odd[0] == (uint8_t)(3 * 5 + 1) && odd[99] == (uint8_t)(102 * 5 + 1));

    static uint8_t ram[8192] __attribute__((aligned(4)));
    memset(ram, 0x77, sizeof ram);
    espboy_ram_load(ram, sizeof ram); /* nothing saved yet: the RAM stays as it was */
    CHECK(ram[0] == 0x77);
    unsigned frame = 0;
    ram[10] = 1;
    for (; frame < 100; frame++) espboy_ram_tick(ram, sizeof ram, frame);
    CHECK(ESP.programs == 0); /* changing for less than two seconds: not written */
    for (; frame < 400; frame++) espboy_ram_tick(ram, sizeof ram, frame);
    const unsigned written = ESP.programs;
    CHECK(written > 0);
    for (; frame < 800; frame++) espboy_ram_tick(ram, sizeof ram, frame);
    CHECK(ESP.programs == written); /* still: nothing more */
    ram[20] = 2; /* changes every frame: never written while it moves */
    for (unsigned i = 0; i < 400; i++, frame++) { ram[21] = (uint8_t)i; espboy_ram_tick(ram, sizeof ram, frame); }
    CHECK(ESP.programs == written);
    ram[21] = 9;
    espboy_ram_flush(ram, sizeof ram);
    CHECK(ESP.programs > written);
    /* reboot: the saved RAM is back, and only for this game */
    static uint8_t again[8192] __attribute__((aligned(4)));
    memset(again, 0, sizeof again);
    espboy_ram_load(again, sizeof again);
    CHECK(!memcmp(again, ram, sizeof ram));
    for (unsigned i = 0; i < 4096; i++) ESP.flash[ESPBOY_SAVE_OFF + i] ^= 1; /* damage a data block */
    for (unsigned i = 0; i < 4096; i++) ESP.flash[ESPBOY_SAVE_OFF + 3 * 4096 + i] ^= 1;
    memset(again, 0, sizeof again);
    espboy_ram_load(again, sizeof again);
    CHECK(again[10] == 0); /* both copies damaged: nothing is loaded */

    /* save states, after the RAM's two copies */
    espboy_ram_flush(ram, sizeof ram);
    memset(again, 0, sizeof again);
    espboy_ram_load(again, sizeof again);
    CHECK(!memcmp(again, ram, sizeof ram));
    m0_state_init(1, sec);
    CHECK(m0_state_available() && m0_state_used() == 0);
    for (unsigned i = 0; i < sizeof fake_machine; i++) fake_machine[i] = (uint8_t)(i * 7 + 3);
    fake_frame = 1234;
    CHECK(m0_state_save(GG_STATE_SLOTS - 1, sec));
    CHECK(m0_state_used() == 1u << (GG_STATE_SLOTS - 1));
    memset(fake_machine, 0, sizeof fake_machine);
    CHECK(!m0_state_load(0, sec)); /* an empty slot */
    CHECK(m0_state_load(GG_STATE_SLOTS - 1, sec) && fake_machine[1] == 10 && fake_machine[25535] == (uint8_t)(25535 * 7 + 3));
    memset(again, 0, sizeof again);
    espboy_ram_load(again, sizeof again); /* the last slot did not reach the RAM's copies */
    CHECK(!memcmp(again, ram, sizeof ram));
    m0_state_init(1, sec); /* reboot: the slot is still there */
    CHECK(m0_state_used() == 1u << (GG_STATE_SLOTS - 1));
    printf("espboy sega flash ok: %u erases, %u programs, %u state slots\n", ESP.erases, ESP.programs, (unsigned)GG_STATE_SLOTS);
    return 0;
}
