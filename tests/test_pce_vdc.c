#include "vdc.h"

#include <assert.h>
#include <stdio.h>

static vdc_t v;

static void reg(unsigned index, unsigned value) {
    vdc_write(&v, 0, (uint8_t)index);
    vdc_write(&v, 2, (uint8_t)value);
    vdc_write(&v, 3, (uint8_t)(value >> 8));
}

static unsigned reference_bg_pixel(unsigned bx, unsigned by) {
    const unsigned bat_width = 64;
    const unsigned bat_y = ((by >> 3) & 31) * bat_width;
    const unsigned bat = v.vram[bat_y | ((bx >> 3) & (bat_width - 1))];
    const unsigned address = (bat & 0x0FFF) * 16 + (by & 7);
    const unsigned w0 = address < VDC_VRAM_WORDS ? v.vram[address] : 0;
    const unsigned w1 = address + 8 < VDC_VRAM_WORDS ? v.vram[address + 8] : 0;
    const unsigned bit = 7 - (bx & 7);
    return ((bat >> 8) & 0xF0) | ((w0 >> bit) & 1) |
        (((w0 >> (bit + 8)) & 1) << 1) |
        (((w1 >> bit) & 1) << 2) |
        (((w1 >> (bit + 8)) & 1) << 3);
}

static void test_background(void) {
    vdc_reset(&v);
    for (unsigned i = 0; i < VDC_VRAM_WORDS; i++)
        v.vram[i] = (uint16_t)(i * 273u + (i >> 3));
    v.mwr = 0x10; /* 64x32 BAT */
    v.cr = 0x80;
    v.hdr = 31; /* 256 pixels, including a partial first tile */
    for (unsigned shift = 0; shift < 8; shift++) {
        for (unsigned row = 0; row < 8; row++) {
            v.vphase = 2; /* VDW */
            v.vcounter = 100;
            v.burst_mode = 0;
            v.rcr_count = 1;
            v.bxr = (uint16_t)(1016 + shift); /* wrap virtual BAT width */
            v.bg_y = (int)row - 1;
            vdc_line(&v, 1);
            for (unsigned x = 0; x < v.row_width; x++)
                assert(v.linebuf[x] == reference_bg_pixel(v.bxr + x, row));
        }
    }
    /* A BAT character above fitted VRAM has no pattern data. */
    v.vram[0] = 0x5FFF;
    v.vphase = 2;
    v.vcounter = 100;
    v.burst_mode = 0;
    v.rcr_count = 1;
    v.bxr = 0;
    v.bg_y = -1;
    vdc_line(&v, 1);
    assert(v.linebuf[0] == 0x50);
}

static void test_vram_bounds(void) {
    vdc_reset(&v);
    assert(sizeof v.vram == 65536);
    reg(0x00, 0x7FFF);
    reg(0x02, 0x1234);
    assert(v.vram[0x7FFF] == 0x1234);
    reg(0x02, 0x5678); /* MAWR advanced into unfitted VRAM */
    assert(v.vram[0] == 0);
    reg(0x01, 0x8000);
    vdc_write(&v, 0, 0x02);
    assert(vdc_read(&v, 2) == 0);
    assert(vdc_read(&v, 3) == 0);

    /* A DMA crossing $7FFF writes neither the upper half nor the start. */
    v.vram[0x7FFE] = 0xBEEF;
    reg(0x10, 0x7FFE);
    reg(0x11, 0x7FFF);
    reg(0x12, 1); /* two words */
    assert(v.vram[0x7FFF] == 0xBEEF);
    assert(v.vram[0] == 0);
    v.vram[0x20] = 0xCAFE;
    reg(0x10, 0x8000);
    reg(0x11, 0x0020);
    reg(0x12, 0); /* an unfitted source reads as zero */
    assert(v.vram[0x20] == 0);

    /* The SAT copy stops at the fitted VRAM boundary. */
    v.vram[0x7FFF] = 0xABCD;
    v.sat[1] = 0x1234;
    v.dvssr = 0x7FFF;
    v.dcr = 0x01;
    v.sat_pending = 1;
    v.sat_test = 1;
    v.vphase = 3; /* outside VDW triggers the pending SAT DMA */
    v.vcounter = 100;
    vdc_line(&v, 1);
    assert(v.sat[0] == 0xABCD);
    assert(v.sat[1] == 0x1234);
}

static void test_sprite_line_bounds(void) {
    vdc_reset(&v);
    v.cr = 0x40; /* sprites on, background off */
    v.hdr = 31;
    v.vram[64] = 0xFFFF; /* all pixels in sprite pattern 1, plane 0 */
    v.sat[0] = 0x40; /* Y = 0, the line's sprite fetch count */
    v.sat[1] = 0x18; /* X = -8: only the right half is visible */
    v.sat[2] = 2; /* pattern 1 */
    v.vphase = 2;
    v.vcounter = 100;
    v.burst_mode = 0;
    vdc_line(&v, 1);
    for (unsigned x = 0; x < v.row_width; x++)
        assert(v.linebuf[x] == (x < 8 ? 0x101 : 0));

    /* An active SAT entry entirely outside the line changes no pixels. */
    v.sat[1] = 0x3FF;
    v.vphase = 2;
    v.vcounter = 100;
    v.burst_mode = 0;
    v.rcr_count = 0;
    vdc_line(&v, 1);
    for (unsigned x = 0; x < v.row_width; x++)
        assert(v.linebuf[x] == 0);
}

static void sprite_row(void) {
    v.vphase = 2;
    v.vcounter = 100;
    v.burst_mode = 0;
    v.rcr_count = 0;
    v.bg_y = 0;
    vdc_line(&v, 1);
}

static void test_sprite_halves_and_flip(void) {
    vdc_reset(&v);
    v.cr = 0x40;
    v.hdr = 3; /* 32 visible pixels */
    v.sat[0] = 0x40;
    v.sat[1] = 0x20;
    v.sat[2] = 4; /* pattern 2, followed by pattern 3 */
    v.sat[3] = 0x100; /* 32 wide */
    v.vram[2 * 64] = 0xFFFF;
    v.vram[3 * 64 + 16] = 0xFFFF;
    sprite_row();
    for (unsigned x = 0; x < 32; x++)
        assert(v.linebuf[x] == (x < 16 ? 0x101 : 0x102));

    v.sat[3] |= 0x800; /* reverse both the pattern order and bit order */
    sprite_row();
    for (unsigned x = 0; x < 32; x++)
        assert(v.linebuf[x] == (x < 16 ? 0x102 : 0x101));

    v.sat[1] = 0x20 + 24; /* clip the right-hand half at the line edge */
    sprite_row();
    for (unsigned x = 0; x < 32; x++)
        assert(v.linebuf[x] == (x >= 24 ? 0x102 : 0));
}

static void test_sprite_vertical_fetch(void) {
    vdc_reset(&v);
    v.cr = 0x40;
    v.hdr = 1;
    v.sat[0] = 0x41; /* sprite Y = 1 */
    v.sat[1] = 0x20;
    v.sat[2] = 2;
    v.vram[64] = 0x8000;
    sprite_row(); /* RCRCount advances to 1; fetch still sees line 0 */
    assert(v.linebuf[0] == 0);
    v.vphase = 2;
    v.vcounter = 100;
    v.burst_mode = 0;
    vdc_line(&v, 1); /* fetch sees line 1 */
    assert(v.linebuf[0] == 0x101);
}

static void test_sprite_priority_collision_and_limit(void) {
    vdc_reset(&v);
    v.hdr = 3;
    v.cr = 0xC3; /* BG, sprites, collision and overflow IRQ */
    v.vram[0] = 1; /* BAT cell 0 uses character 1 */
    v.vram[17] = 0xFFFF; /* row 1 of an opaque background tile */
    v.vram[2 * 64] = 0xFFFF; /* sprite 0 */
    v.vram[3 * 64 + 16] = 0xFFFF; /* sprite 1 */
    v.sat[0] = v.sat[4] = 0x40;
    v.sat[1] = v.sat[5] = 0x20;
    v.sat[2] = 4;
    v.sat[6] = 6;
    sprite_row();
    assert(v.linebuf[0] == 3); /* low-priority sprite stays behind BG */
    assert(v.status & VDCS_CR);

    v.sat[3] = 0x80; /* sprite 0 in front */
    v.status = 0;
    sprite_row();
    assert(v.linebuf[0] == 0x101);
    assert(v.status & VDCS_CR);

    vdc_reset(&v);
    v.cr = 0x42;
    v.hdr = 3;
    v.vram[2 * 64] = 0xFFFF;
    for (unsigned i = 0; i < 17; i++) {
        v.sat[i * 4] = 0x40;
        v.sat[i * 4 + 1] = 0x20 + (uint16_t)i;
        v.sat[i * 4 + 2] = 4;
    }
    sprite_row();
    assert(v.status & VDCS_OR);
    assert(v.linebuf[31] == 0); /* the 17th sprite was rejected */
}

static void test_scroll_and_live_vram(void) {
    vdc_reset(&v);
    v.cr = 0x80;
    v.hdr = 1;
    v.vram[0] = 1;
    v.vram[17] = 0x0080; /* left pixel on row 1 of character 1 */
    sprite_row();
    assert(v.linebuf[0] == 1);
    v.bxr = 1;
    sprite_row();
    assert(v.linebuf[0] == 0);
    v.bxr = 0;
    reg(0, 17); /* write a changed pattern through the VRAM port */
    reg(2, 0x0040);
    sprite_row();
    assert(v.linebuf[0] == 0 && v.linebuf[1] == 1);
}

static void test_sprite_pattern_bits(void) {
    for (unsigned flip = 0; flip < 2; flip++) {
        for (unsigned clipped = 0; clipped < 3; clipped++) {
            vdc_reset(&v);
            v.cr = 0x40;
            v.hdr = 3;
            v.sat[0] = 0x40;
            v.sat[1] = (uint16_t)(clipped == 0 ? 0x18 : clipped == 1 ? 0x20 : 0x38);
            v.sat[2] = 4;
            v.sat[3] = (uint16_t)(0x100 | (flip ? 0x800 : 0) | 5);
            for (unsigned half = 0; half < 2; half++)
                for (unsigned plane = 0; plane < 4; plane++)
                    v.vram[(2 + half) * 64 + plane * 16] =
                        (uint16_t)(0x9325u ^ (half * 0x6523u) ^ (plane * 0x319bu));
            sprite_row();
            const int left = (int)v.sat[1] - 0x20;
            for (unsigned x = 0; x < 32; x++) {
                const int sx = (int)x - left;
                unsigned expected = 0;
                if (sx >= 0 && sx < 32) {
                    const unsigned half = flip ? 1u - (unsigned)sx / 16 : (unsigned)sx / 16;
                    const unsigned bit = flip ? (unsigned)sx % 16 : 15 - (unsigned)sx % 16;
                    for (unsigned plane = 0; plane < 4; plane++)
                        expected |= ((v.vram[(2 + half) * 64 + plane * 16] >> bit) & 1) << plane;
                    if (expected) expected |= 0x150;
                }
                assert(v.linebuf[x] == expected);
            }
        }
    }
}

int main(void) {
    test_background();
    test_vram_bounds();
    test_sprite_line_bounds();
    test_sprite_halves_and_flip();
    test_sprite_vertical_fetch();
    test_sprite_priority_collision_and_limit();
    test_scroll_and_live_vram();
    test_sprite_pattern_bits();
    puts("ok: PCE VDC tiles, sprites, scroll and VRAM bounds");
    return 0;
}
