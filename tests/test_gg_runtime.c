/* GG/SMS renderer against colour indices and a component-wise filter;
 * line-counter catch-up against a literal per-line hardware model; OUTI /
 * OUTD from RAM against the generic interpreter. */
#include "gg.h"
#include "gg_display.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(v) do { if (!(v)) { fprintf(stderr, "line %d: %s\n", __LINE__, #v); return 1; } } while (0)
static uint8_t rom[0x4000];
static uint32_t random_state = 12345;
static uint32_t random_word(void) { return random_state = random_state * 1664525u + 1013904223u; }
void gg_rom_trace(uint32_t offset) { (void)offset; }
void gg_rom_trace_fetch(uint32_t offset) { (void)offset; }
void gg_rom_trace_insn(uint32_t offset) { (void)offset; }
void gg_cart_trace(uint32_t offset, unsigned write) { (void)offset; (void)write; }

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

/* OUTI / OUTD run from RAM by sg_interp_block (its fast path) against
 * sg_step (the generic decode), from the same state: the whole machine. */
static int ram_outx(void) {
    static gg_t before, fast;
    static const uint8_t ports[] = { 0x7f, 0xbe, 0x06, 0x7e, 0xdd };
    for (unsigned round = 0; round < 2000; round++) {
        gg_init(rom, sizeof rom, 0);
        for (unsigned i = 0; i < sizeof machine.ram; i++) machine.ram[i] = (uint8_t)(random_word() >> 16);
        const unsigned start = 0xc100 + (random_word() >> 16) % 0x3e00, count = 1 + (random_word() >> 16) % 40;
        unsigned a = start;
        for (unsigned k = 0; k < count; k++, a += 2) {
            machine.ram[a & 8191] = 0xed;
            machine.ram[(a + 1) & 8191] = (random_word() >> 16) & 1 ? 0xab : 0xa3;
        }
        machine.ram[a & 8191] = 0x18; machine.ram[(a + 1) & 8191] = 0xfe; /* JR $ */
        z80_t *c = &machine.cpu;
        c->pc = (uint16_t)start;
        c->hl = (uint16_t)random_word();
        c->bc = (uint16_t)((random_word() & 0xff00) | ports[(random_word() >> 16) % sizeof ports]);
        c->f = (uint8_t)random_word(); c->r = (uint8_t)random_word();
        c->iff1 = 0; c->ei_delay = (uint8_t)(random_word() & 1); c->irq = (uint8_t)((random_word() >> 4) & 1);
        before = machine;
        const uint32_t done = machine.interpreted;
        sg_interp_block();
        fast = machine;
        const uint32_t n = machine.interpreted - done;
        REQUIRE(n >= 1);
        machine = before;
        while (machine.interpreted - done < n) (void)sg_step(0);
        REQUIRE(!memcmp(&machine, &fast, sizeof machine));
    }
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
        machine.vdp[1] = (uint8_t)(0x40 | ((round >> 1) & 3)); /* sprites in odd rounds: all sizes */
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

/* Korean 150-in-1: its MSX games draw in the TMS9918's Graphics I/II.  The
 * Master System VDP shows those with its fixed palette (white 3F, black 00,
 * dark red 01): a tile of four white then four black pixels, the filter's
 * five output pixels of the group, and a sprite over it. */
static void vdp_register(unsigned r, unsigned v) { gg_vdp_control_write((uint8_t)v); gg_vdp_control_write((uint8_t)(0x80 | r)); }
static void vram_write(unsigned address, uint8_t v) {
    gg_vdp_control_write((uint8_t)address); gg_vdp_control_write((uint8_t)(0x40 | (address >> 8)));
    gg_vdp_data_write(v);
}
static int tms_modes(void) {
    for (unsigned graphics2 = 0; graphics2 < 2; graphics2++) {
        gg_init_system(rom, sizeof rom, 0, 1);
        machine.mapper = M0_MAPPER_KOREAN150;
        gg_reset();
        vdp_register(0, graphics2 ? 0x02 : 0x00); /* no M4: legacy modes */
        vdp_register(1, 0xe0);                    /* display, 16K, 8x8 sprites */
        vdp_register(2, 0x0e); vdp_register(5, 0x76); vdp_register(7, 0x04);
        if (graphics2) { vdp_register(3, 0xff); vdp_register(4, 0x03); }
        else { vdp_register(3, 0x80); vdp_register(4, 0x00); }
        for (unsigned i = 0; i < 0x4000; i++) vram_write(i, 0);
        vram_write(0x3800, 0); /* tile 0 at the top left */
        const unsigned pattern = 0x0000, colours = 0x2000;
        vram_write(pattern + 1, 0xf0);                         /* row 1: four on, four off */
        vram_write(colours + (graphics2 ? 1 : 0), 0xf1);       /* white on black */
        vram_write(0x3b00, 208);                               /* sprite table: terminator first */
        uint16_t out[GG_ROW_STRIDE] = { 0 };
        gg_render_line(1, out + GG_ROW_PAD);
        const uint16_t *o = out + GG_ROW_PAD;
        REQUIRE(o[0] == 0xffffu && o[1] == 0xffffu); /* host panel order */
        REQUIRE(o[3] == 0 && o[4] == 0);
        REQUIRE(o[2] != 0 && o[2] != 0xffffu);
        vdp_register(1, 0xa0); /* display off: the backdrop, dark blue */
        gg_render_line(1, out + GG_ROW_PAD);
        REQUIRE(o[0] != 0 && o[0] == o[7] && o[0] == o[159]);
    }
    return 0;
}

static uint8_t korean_rom[0x200000];
static uint8_t korean_state[sizeof(gg_t)];
static int korean_load_state;
static void korean_io(void *data, uint32_t bytes, void *ctx) {
    (void)ctx;
    if (korean_load_state) memcpy(data, korean_state, bytes);
    else memcpy(korean_state, data, bytes);
}

static int korean150(void) {
    /* An independent table of the hardware's six windows. Test mode and
     * bank masks, both parity patterns, vectors and every slot boundary. */
    static const char *layout[8][2] = {
        {"0101zz", "2323zz"}, {"0123zz", "0123zz"},
        {"uv01zz", "uv23zz"}, {"uvyz01", "uvyz23"},
        {"uv0110", "uv2332"}, {"uv0123", "uv2301"},
        {"zzzzzz", "zzzzzz"}, {"zzzzzz", "zzzzzz"}
    };
    for (unsigned p = 0; p < 256; p++)
        for (unsigned b = 0; b < 8192; b++) korean_rom[p * 8192 + b] = (uint8_t)(p ^ b);
    gg_init_system(korean_rom, sizeof korean_rom, 0, 1);
    machine.mapper = M0_MAPPER_KOREAN150;
    gg_reset();
    REQUIRE(machine.page[0] == 0 && machine.page[1] == 1 && machine.page[2] == 0 && machine.page[5] == 255);
    for (unsigned mode = 0; mode < 256; mode++) for (unsigned bank = 0; bank < 256; bank++) {
        gg_write(0xfffc, (uint8_t)mode);
        gg_write(0xfff3, (uint8_t)bank);
        REQUIRE(machine.ram[0x1ffc] == mode && machine.ram[0x1ff3] == bank);
        const char *pattern = layout[mode >> 5][bank & 1];
        const unsigned base = ((mode & 0x10) << 3) + ((bank & 0x3e) << 1);
        for (unsigned slot = 0; slot < 6; slot++) {
            char c = pattern[slot];
            unsigned page = c >= '0' && c <= '3' ? base + c - '0'
                          : c == 'u' ? 0x80 : c == 'v' ? 0x81 : c == 'y' ? 0xfe : 0xff;
            REQUIRE(machine.page[slot] == page);
            for (unsigned offset = 0; offset <= 0x1fff; offset += 0x1fff) {
                uint16_t address = (uint16_t)(slot * 8192 + offset);
                REQUIRE(gg_read(address) == (uint8_t)(page ^ offset));
                REQUIRE(gg_location(address) == (page << 16 | address));
            }
        }
        unsigned selected = machine.page[0];
        gg_write(0xfffd, 77); /* Sega bank registers are ordinary RAM here. */
        REQUIRE(machine.page[0] == selected);
    }
    gg_write(0xfffc, 0xa0); gg_write(0xfff3, 0x21);
    uint8_t pages[6]; memcpy(pages, machine.page, 6);
    korean_load_state = 0; gg_state_io(korean_io, 0, korean_load_state);
    gg_write(0xfffc, 0); gg_write(0xfff3, 0);
    korean_load_state = 1; gg_state_io(korean_io, 0, korean_load_state);
    REQUIRE(!memcmp(pages, machine.page, 6));
    for (unsigned s = 0; s < 6; s++) REQUIRE(gg_read((uint16_t)(s * 8192)) == pages[s]);
    return 0;
}

int main(void) {
    REQUIRE(!korean150()); REQUIRE(!tms_modes());
    REQUIRE(!counters()); REQUIRE(!rendering(0)); REQUIRE(!rendering(1)); REQUIRE(!ram_outx());
    unsigned shown = 0;
    for (unsigned y = 0; y < GG_ACTIVE; y++) {
        REQUIRE(gg_sms_display_row(y) == y * 5 / 8 && gg_sms_display_row(y) == m0_row_of(y, 192, 120));
        REQUIRE(gg_sms_line_shown(y) == (y == ((y * 5 / 8) * 8 + 4) / 5));
        shown += gg_sms_line_shown(y);
    }
    REQUIRE(shown == 120);
    for (unsigned y = GG_TOP; y < GG_TOP + GG_HEIGHT; y++) {
        const unsigned r = m0_row_of(y - GG_TOP, 144, 128);
        const int mode = y + 1 < GG_TOP + GG_HEIGHT && m0_row_of(y + 1 - GG_TOP, 144, 128) == r ? 3
                       : y > GG_TOP && m0_row_of(y - 1 - GG_TOP, 144, 128) == r ? 2 : 1;
        REQUIRE(gg_meta_display_row(y) == r && gg_meta_draw_mode(y) == mode);
    }
    for (unsigned l = 0; l <= GG_HEIGHT; l++) REQUIRE(gg_pokitto_rows[l] == m0_row_first(l, 144, 176));
    puts("GG/SMS: counter, priority, sprite/filter composition and line policies match references");
    return 0;
}
