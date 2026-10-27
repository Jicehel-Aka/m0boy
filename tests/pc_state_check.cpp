/* m0_state.cpp's PC store on the host: GBL_TARGET_PC keeps the state slots
 * in the plain file GBL_STATE_NAME of the working directory, created on the
 * first save (reads never create it).  Slots, boot scan, round trip, a save
 * cut short and a damaged one.  Built against the stub headers in
 * pc_state_stub/ (a 1 KiB machine); runs in a throwaway directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "m0_state.h"

extern "C" {
uint32_t gbl_frame;
static uint8_t machine[1024];
uint32_t gbl_state_bytes(void) { return sizeof machine; }
/* The machine itself streams: the same call serves the save and the load. */
void gbl_state_io(void (*fn)(void *data, uint32_t bytes, void *ctx), void *ctx, int load) {
    (void)load;
    fn(machine, sizeof machine, ctx);
}
}

#define STATE_FILE "pc_state_check.STA"
#define SLOT_BYTES (4u * 4u * 512u)

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void fill(uint8_t seed) {
    for (unsigned i = 0; i < sizeof machine; i++) machine[i] = (uint8_t)(seed + i * 3u);
}
static int is_pattern(uint8_t seed) {
    for (unsigned i = 0; i < sizeof machine; i++)
        if (machine[i] != (uint8_t)(seed + i * 3u)) return 0;
    return 1;
}
static long file_size(void) {
    FILE *f = fopen(STATE_FILE, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fclose(f);
    return n;
}

int main(void) {
    uint8_t buf[512];
    char dir[] = "/tmp/pc_state_checkXXXXXX";
    CHECK(mkdtemp(dir));
    CHECK(chdir(dir) == 0);

    /* nothing yet: the store is offered, no slot used, no file created */
    m0_state_init(0, buf);
    CHECK(m0_state_available());
    CHECK(m0_state_used() == 0);
    CHECK(file_size() < 0);
    CHECK(!m0_state_load(0, buf)); /* no slot 0: refused, still no file */
    CHECK(file_size() < 0);

    /* first save creates the whole slot area */
    fill(0x11);
    CHECK(m0_state_save(0, buf));
    CHECK(file_size() == (long)SLOT_BYTES);
    CHECK(m0_state_used() == 1u);

    /* a boot finds it and the round trip gives the machine back */
    m0_state_init(0, buf);
    CHECK(m0_state_used() == 1u);
    fill(0x22);
    CHECK(m0_state_load(0, buf));
    CHECK(is_pattern(0x11));

    /* a second slot, then slot 0 overwritten */
    fill(0x33);
    CHECK(m0_state_save(1, buf));
    fill(0x44);
    CHECK(m0_state_save(0, buf));
    m0_state_init(0, buf);
    CHECK(m0_state_used() == 3u);
    fill(0x55);
    CHECK(m0_state_load(1, buf) && is_pattern(0x33));
    CHECK(m0_state_load(0, buf) && is_pattern(0x44));

    /* a save cut after the data, before the header: the slot still names
     * the old state, whose checksum refuses the load, machine untouched */
    fill(0x66);
    CHECK(m0_state_save(2, buf));
    {
        FILE *f = fopen(STATE_FILE, "r+b");
        CHECK(f != NULL);
        fseek(f, (long)(2u * 4u * 512u + 512u + 7u), SEEK_SET);
        fputc(0x55, f);
        fclose(f);
    }
    m0_state_init(0, buf);
    CHECK(m0_state_used() == 7u);
    fill(0x77);
    CHECK(!m0_state_load(2, buf));
    CHECK(is_pattern(0x77));

    /* a damaged header: the boot no longer counts the slot */
    {
        FILE *f = fopen(STATE_FILE, "r+b");
        CHECK(f != NULL);
        fseek(f, (long)(2u * 4u * 512u), SEEK_SET);
        fputc('X', f);
        fclose(f);
    }
    m0_state_init(0, buf);
    CHECK(m0_state_used() == 3u);
    CHECK(!m0_state_load(2, buf));

    /* slots out of range are refused */
    CHECK(!m0_state_save(4, buf));
    CHECK(!m0_state_load(4, buf));

    printf("pc state file store ok (%s)\n", dir);
    return 0;
}
