/* espboy_flash.cpp on the host, against the mock flash: battery RAM copies,
 * power cuts, save states.  Built with the generated synthetic project
 * (8 KiB battery RAM). */
#include <Arduino.h>
#include <stdio.h>
extern "C" {
#include "gbl.h"
#include "gbl_config.h"
int gbl_platform_line_check(uint8_t, uint32_t) { return 0; }
uint16_t *gbl_platform_line_begin(uint8_t) { return 0; }
void gbl_platform_line_end(uint8_t) {}
void gbl_platform_vblank(void) {}
int gbl_platform_save_write(uint32_t, const uint8_t *) { return 0; }
int gbl_platform_rom_read(uint32_t, uint8_t *) { return 0; }
}
#include "gbl_espboy.h"
#include "meta_save.h"
#include "meta_state.h"

MockEsp ESP;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static uint8_t image[GBL_ERAM_SIZE];
static void fill(uint8_t seed) {
    for (unsigned i = 0; i < sizeof image; i++) image[i] = (uint8_t)(seed + i * 7u);
}
static int read_back(uint8_t seed) {
    uint8_t sec[512];
    fill(seed);
    for (unsigned s = 0; s * 512 < sizeof image; s++) {
        if (!gbl_platform_save_read(s, sec)) return 0;
        if (memcmp(sec, image + s * 512, 512)) return 0;
    }
    return 1;
}

int main(void) {
    memset(ESP.flash, 0xff, sizeof ESP.flash);
    uint8_t scratch[512] __attribute__((aligned(4)));
    CHECK(meta_save_init(0) == META_SAVE_NO_FILE);
    CHECK(!gbl_platform_save_read(0, scratch));
    /* first write, alternating copies, reboot in between */
    for (unsigned n = 0; n < 5; n++) {
        fill((uint8_t)(10 + n));
        CHECK(gbl_platform_save_image(image, sizeof image));
        CHECK(read_back((uint8_t)(10 + n)));
        CHECK(meta_save_init(0) == META_SAVE_OK);
        CHECK(read_back((uint8_t)(10 + n)));
    }
    /* power cut at every step of a write: the previous copy always survives */
    for (long cut = 0; cut < 16; cut++) {
        fill(99);
        ESP.fail_after = cut;
        int ok = gbl_platform_save_image(image, sizeof image);
        ESP.fail_after = -1;
        CHECK(meta_save_init(0) == META_SAVE_OK);
        if (ok) { CHECK(read_back(99)); fill(14); CHECK(gbl_platform_save_image(image, sizeof image)); }
        else CHECK(read_back(14));
        CHECK(read_back(14));
        fill(14);
    }
    /* a damaged copy is refused; with both damaged there is no save */
    fill(14);
    CHECK(gbl_platform_save_image(image, sizeof image)); /* both copies hold it now */
    CHECK(gbl_platform_save_image(image, sizeof image));
    CHECK(read_back(14));
    for (unsigned c = 0; c < 2; c++) {
        ESP.flash[ESPBOY_SAVE_OFF + c * 3 * 4096 + 100] ^= 1;
        CHECK(meta_save_init(0) == (c == 0 ? META_SAVE_OK : META_SAVE_NO_FILE));
    }

    /* save states */
    memset(ESP.flash + ESPBOY_SAVE_OFF, 0xff, ESPBOY_SAVE_BYTES);
    meta_save_init(0);
    gbl_reset();
    CHECK(meta_state_available() == 0); /* before meta_state_init */
    meta_state_init(0, scratch);
    CHECK(meta_state_available() && meta_state_used() == 0);
    extern uint8_t gbl_wram[];
    for (unsigned slot = 0; slot < GBL_STATE_SLOTS; slot++) {
        for (unsigned i = 0; i < 4096; i++) gbl_wram[i] = (uint8_t)(i * 3 + slot);
        CHECK(meta_state_save(slot, scratch));
        CHECK(meta_state_used() >> slot & 1u);
    }
    for (int slot = GBL_STATE_SLOTS - 1; slot >= 0; slot--) {
        memset(gbl_wram, 0, 4096);
        CHECK(meta_state_load((unsigned)slot, scratch));
        for (unsigned i = 0; i < 4096; i++) CHECK(gbl_wram[i] == (uint8_t)(i * 3 + slot));
    }
    /* overwriting a slot, then a power cut in the middle of another save */
    for (unsigned i = 0; i < 4096; i++) gbl_wram[i] = 0x5a;
    CHECK(meta_state_save(2, scratch));
    ESP.fail_after = 6;
    CHECK(!meta_state_save(3, scratch));
    ESP.fail_after = -1;
    meta_state_init(0, scratch);
    CHECK(!(meta_state_used() >> 3 & 1u) && (meta_state_used() >> 2 & 1u));
    CHECK(meta_state_load(2, scratch) && gbl_wram[100] == 0x5a);
    /* a damaged state is refused, the machine untouched */
    ESP.flash[ESPBOY_SAVE_OFF + 6 * 4096 + 2 * 7 * 4096 + 3 * 512 + 5] ^= 0x10;
    meta_state_init(0, scratch);
    memset(gbl_wram, 0, 4096);
    CHECK(!meta_state_load(2, scratch) && gbl_wram[100] == 0);
    printf("espboy flash ok: %u erases, %u programs\n", ESP.erases, ESP.programs);
    return 0;
}
