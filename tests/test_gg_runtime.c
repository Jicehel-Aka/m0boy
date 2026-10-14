/* GG/SMS renderer against colour indices and a component-wise filter;
 * line-counter catch-up against a literal per-line hardware model. */
#include "gg.h"
#include "gg_display.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(v) do { if (!(v)) { fprintf(stderr, "line %d: %s\n", __LINE__, #v); return 1; } } while (0)
static uint8_t rom[0x4000];
static uint32_t random_state = 12345;
static uint32_t random_word(void) { return random_state = random_state * 1664525u + 1013904223u; }
void gg_rom_trace(uint32_t offset) { (void)offset; }

static void reference_counter(void) {
    while (machine.lc_line < GG_LINES &&
           (int32_t)(machine.cyc - (machine.frame_start + machine.lc_line * GG_LINE_CYCLES + GG_LINE_IRQ_AT)) >= 0) {
        if (machine.lc_line <= GG_ACTIVE) {
            if (!machine.lc_count) { machine.lc_count = machine.vdp[10]; machine.line_irq = 1; }
            else machine.lc_count--;
        } else machine.lc_count = machine.vdp[10];
        machine.lc_line++;
    }
}

static int counters(void) {
    gg_init(rom, sizeof rom, 0);
    for (unsigned i = 0; i < 2000000; i++) {
        machine.frame_start = random_word();
        const int32_t delta = (int32_t)(random_word() % 160000) - 80000;
        machine.cyc = machine.frame_start + (uint32_t)delta;
        machine.lc_line = (uint16_t)(random_word() % (GG_LINES + 1));
        machine.lc_count = (uint8_t)random_word();
        machine.vdp[10] = (uint8_t)(random_word() >> 16);
        machine.line_irq = (uint8_t)(random_word() & 1);
        const unsigned old_line = machine.lc_line, old_count = machine.lc_count, old_irq = machine.line_irq;
        reference_counter();
        const unsigned line = machine.lc_line, count = machine.lc_count, irq = machine.line_irq;
        machine.lc_line = (uint16_t)old_line; machine.lc_count = (uint8_t)old_count; machine.line_irq = (uint8_t)old_irq;
        gg_line_counter_catchup();
        REQUIRE(machine.lc_line == line && machine.lc_count == count && machine.line_irq == irq);
    }
    /* IRQ at line 192 precedes VBlank at line 193; R10 writes do not
     * reload an already-running counter until its next underflow. */
    machine.frame_start = 0xfffffff0u; machine.lc_line = 192;
    machine.lc_count = 0; machine.vdp[10] = 9; machine.line_irq = 0;
    machine.cyc = machine.frame_start + 192 * GG_LINE_CYCLES + GG_LINE_IRQ_AT - 1;
    gg_line_counter_catchup(); REQUIRE(machine.lc_line == 192 && !machine.line_irq);
    machine.cyc++; gg_line_counter_catchup();
    REQUIRE(machine.lc_line == 193 && machine.line_irq && machine.lc_count == 9);
    return 0;
}

static void palette_byte(unsigned i, uint8_t value) {
    gg_vdp_control_write((uint8_t)i); gg_vdp_control_write(0xc0); gg_vdp_data_write(value);
}

static uint16_t blend(uint16_t a, uint16_t b, unsigned wa, unsigned wb, unsigned divisor, unsigned round) {
    const unsigned r = (wa * (a >> 11) + wb * (b >> 11) + round) / divisor;
    const unsigned g = (wa * ((a >> 5) & 63) + wb * ((b >> 5) & 63) + round) / divisor;
    const unsigned blue = (wa * (a & 31) + wb * (b & 31) + round) / divisor;
    return (uint16_t)(r << 11 | g << 5 | blue);
}

static int rendering(int sms) {
    for (unsigned round = 0; round < 16; round++) {
        gg_init_system(rom, sizeof rom, 0, sms);
        for (unsigned i = 0; i < sizeof machine.vram; i++) machine.vram[i] = (uint8_t)(random_word() >> 16);
        machine.vdp[0] = (uint8_t)(6 | ((round & 7) << 5));
        machine.vdp[1] = (uint8_t)(0x40 | (round & 3));
        machine.vdp[2] = 0x0e; machine.sat_base = 0x3f00;
        machine.vdp[8] = (uint8_t)random_word(); machine.vscroll = (uint8_t)(random_word() % 224);
        for (unsigned i = 0; i < 64; i++) machine.vram[0x3f00 + i] = (uint8_t)(random_word() % 192);
        machine.vram[0x3f00 + (round & 1 ? 48 : 0)] = 0xd0;
        machine.sprites_dirty = 1;
        for (unsigned i = 0; i < (sms ? 32u : 64u); i++) palette_byte(i, (uint8_t)random_word());
        for (unsigned y = sms ? 0 : GG_TOP; y < (sms ? GG_ACTIVE : GG_TOP + GG_HEIGHT); y++) {
            uint8_t indices[256];
            uint16_t output[GG_ROW_STRIDE] = { 0 };
            gg_render_indices(y, indices); gg_render_line(y, output + GG_ROW_PAD);
            if (!sms) {
                for (unsigned x = 0; x < GG_WIDTH; x++) REQUIRE(output[GG_ROW_PAD + x] == machine.pal[indices[GG_LEFT + x]]);
            } else {
                static const unsigned left[5] = { 0, 1, 3, 5, 6 }, right[5] = { 1, 2, 4, 6, 7 };
                static const unsigned wa[5] = { 5, 3, 1, 5, 3 }, wb[5] = { 3, 5, 1, 3, 5 };
                for (unsigned g = 0; g < 32; g++)
                    for (unsigned x = 0; x < 5; x++) {
                        const uint16_t a = machine.pal[indices[8 * g + left[x]]], b = machine.pal[indices[8 * g + right[x]]];
                        REQUIRE(output[GG_ROW_PAD + 5 * g + x] == blend(a, b, wa[x], wb[x], x == 2 ? 2 : 8, x == 2 ? 0 : 4));
                    }
            }
        }
    }
    return 0;
}

int main(void) {
    REQUIRE(!counters()); REQUIRE(!rendering(0)); REQUIRE(!rendering(1));
    unsigned shown = 0;
    for (unsigned y = 0; y < GG_ACTIVE; y++) {
        REQUIRE(gg_sms_display_row(y) == y * 5 / 8);
        REQUIRE(gg_sms_line_shown(y) == (y == ((y * 5 / 8) * 8 + 4) / 5));
        shown += gg_sms_line_shown(y);
    }
    REQUIRE(shown == 120);
    for (unsigned y = GG_TOP; y < GG_TOP + GG_HEIGHT; y++) {
        const unsigned r = (y - GG_TOP) * 8 / 9;
        const int mode = y + 1 < GG_TOP + GG_HEIGHT && (y + 1 - GG_TOP) * 8 / 9 == r ? 3
                       : y > GG_TOP && (y - 1 - GG_TOP) * 8 / 9 == r ? 2 : 1;
        REQUIRE(gg_meta_display_row(y) == r && gg_meta_draw_mode(y) == mode);
    }
    puts("GG/SMS: counter, priority, sprite/filter composition and line policies match references");
    return 0;
}
