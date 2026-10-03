/*
 * Pokitto display: ST7775-type 220x176 panel on a 16-bit parallel bus.
 *
 * The CPU drives the bus (PokittoLib's HWLCD wiring, 4-layer board 2.1):
 * data P2_3..P2_18, CD P0_2 (0 command, 1 data), WR P1_12 (data latched on
 * the rising edge), RD P1_24 (kept high), RES P1_0, backlight P2_2 (on).
 * MASK[2] stays set to the data lines, so a word written to MPIN[2]
 * changes them alone (the audio DAC shares port 2, P2_20..23).
 *
 * GRAM register 0x20 addresses the panel's 176-row axis and 0x21 its
 * 220-column one; entry mode 0x1038 makes writes advance along a row.  The
 * Game Boy picture is centred, 160x144 at (30, 16), or enlarged to 200x176
 * as gm0 does (pokitto_lcd_row_zoom); rows are written as the PPU completes
 * them, so the meta_lcd.h transfer API is synchronous here.
 */
#include "meta_lcd.h"

#include "lpc11u6x.h"
#include "pokitto_hw.h"

#define WR_BIT (1u << 12) /* P1_12 */
#define CD_BIT (1u << 2)  /* P0_2 */
#define RD_BIT (1u << 24) /* P1_24 */
#define RES_BIT (1u << 0) /* P1_0 */
#define DATA_MASK (0xFFFFu << 3)

/* Pixel pushes in Thumb-1 from SRAM (pokitto_lcd_asm.S); 0 keeps the C
 * loops below, the reference they must match. */
#ifndef POKITTO_LCD_ASM
#define POKITTO_LCD_ASM 1
#endif
#if POKITTO_LCD_ASM
void pokitto_push160(const uint16_t *px);
void pokitto_push200(const uint16_t *px);
#endif

static uint32_t seq;

static inline void strobe(void) {
    GPIO_CLR(1) = WR_BIT;
    __asm__ volatile("nop");
    GPIO_SET(1) = WR_BIT;
}

static void command(uint16_t c) {
    GPIO_CLR(0) = CD_BIT;
    GPIO_MPIN(2) = (uint32_t)c << 3;
    strobe();
    GPIO_SET(0) = CD_BIT;
}

static void data(uint16_t d) {
    GPIO_MPIN(2) = (uint32_t)d << 3;
    strobe();
}

static void reg(uint16_t c, uint16_t d) {
    command(c);
    data(d);
}

/* GRAM write from (x, y) on: the pixels follow as data. */
static void at(unsigned x, unsigned y) {
    reg(0x20, (uint16_t)y);
    reg(0x21, (uint16_t)x);
    command(0x22);
}

#if !POKITTO_LCD_ASM
static void push(const uint16_t *px, unsigned n) {
    volatile uint32_t *mpin = &GPIO_MPIN(2), *clr = &GPIO_CLR(1), *set = &GPIO_SET(1);
    while (n--) {
        *mpin = (uint32_t)*px++ << 3;
        *clr = WR_BIT;
        __asm__ volatile("nop");
        *set = WR_BIT;
    }
}
#endif

static void push_same(uint16_t c, uint32_t n) {
    GPIO_MPIN(2) = (uint32_t)c << 3;
    while (n--) strobe();
}

void pokitto_lcd_row(unsigned row, const uint16_t *px) {
    at(POKITTO_GB_X, POKITTO_GB_Y + row);
#if POKITTO_LCD_ASM
    pokitto_push160(px);
#else
    push(px, 160);
#endif
}

/* gm0's 5:4 stretch: in each group of four pixels the second one is
 * latched twice (a second WR strobe, the bus unchanged), 160 -> 200. */
void pokitto_lcd_row_zoom(unsigned y, const uint16_t *px) {
    at(POKITTO_ZOOM_X, y);
#if POKITTO_LCD_ASM
    pokitto_push200(px);
#else
    volatile uint32_t *mpin = &GPIO_MPIN(2), *clr = &GPIO_CLR(1), *set = &GPIO_SET(1);
    for (unsigned g = 0; g < 40; g++, px += 4) {
        *mpin = (uint32_t)px[0] << 3;
        *clr = WR_BIT;
        __asm__ volatile("nop");
        *set = WR_BIT;
        *mpin = (uint32_t)px[1] << 3;
        *clr = WR_BIT;
        __asm__ volatile("nop");
        *set = WR_BIT;
        *clr = WR_BIT;
        __asm__ volatile("nop");
        *set = WR_BIT;
        *mpin = (uint32_t)px[2] << 3;
        *clr = WR_BIT;
        __asm__ volatile("nop");
        *set = WR_BIT;
        *mpin = (uint32_t)px[3] << 3;
        *clr = WR_BIT;
        __asm__ volatile("nop");
        *set = WR_BIT;
    }
#endif
}

void pokitto_lcd_fill_all(uint16_t color) {
    at(0, 0);
    push_same(color, 220u * 176u);
}

void pokitto_fault_screen(void) {
    pokitto_lcd_fill_all(0xF800);
    for (;;) {}
}

static void pins(void) {
    IOCON_SET(IOCON_PIO0(2), IOCON_FUNC(0), IOCON_MODE_NONE);
    IOCON_SET(IOCON_PIO1(0), IOCON_FUNC(0), IOCON_MODE_NONE);
    IOCON_SET(IOCON_PIO1(12), IOCON_FUNC(0), IOCON_MODE_NONE);
    IOCON_SET(IOCON_PIO1(24), IOCON_FUNC(0), IOCON_MODE_NONE);
    for (unsigned n = 2; n <= 18; n++) IOCON_SET(IOCON_PIO2(n), IOCON_FUNC(0), IOCON_MODE_NONE);
    GPIO_DIR(0) |= CD_BIT;
    GPIO_DIR(1) |= WR_BIT | RD_BIT | RES_BIT;
    GPIO_DIR(2) |= DATA_MASK | (1u << 2);
    GPIO_SET(1) = WR_BIT | RD_BIT;
    GPIO_SET(0) = CD_BIT;
    GPIO_MASK(2) = ~DATA_MASK;
    GPIO_SET(2) = 1u << 2; /* backlight fully on */
}

void meta_lcd_init(void) {
    pins();
    GPIO_SET(1) = RES_BIT;
    delay(10);
    GPIO_CLR(1) = RES_BIT;
    delay(10);
    GPIO_SET(1) = RES_BIT;
    delay(10);
    /* PokittoLib's lcdInit sequence. */
    reg(0x01, 0x011C); /* driver output: SS, 220 lines */
    reg(0x02, 0x0100); /* line inversion */
    reg(0x03, 0x1030); /* entry mode */
    reg(0x08, 0x0808); /* porches */
    reg(0x0C, 0x0000); /* RGB interface off */
    reg(0x0F, 0x0001); /* oscillator */
    reg(0x20, 0x0000);
    reg(0x21, 0x0000);
    reg(0x10, 0x0000); /* power on */
    reg(0x11, 0x1000);
    delay(10);
    reg(0x30, 0x0000); /* gate scan from G1 */
    reg(0x31, 0x00DB); /* scroll: whole screen, off */
    reg(0x32, 0x0000);
    reg(0x33, 0x0000);
    reg(0x34, 0x00DB); /* partial screen: all */
    reg(0x35, 0x0000);
    reg(0x36, 0x00AF); /* window: rows 0..175 */
    reg(0x37, 0x0000);
    reg(0x38, 0x00DB); /* columns 0..219 */
    reg(0x39, 0x0000);
    delay(10);
    reg(0xFF, 0x0003); /* gamma */
    reg(0x50, 0x0203);
    reg(0x51, 0x0A09);
    reg(0x52, 0x0005);
    reg(0x53, 0x1021);
    reg(0x54, 0x0602);
    reg(0x55, 0x0003);
    reg(0x56, 0x0703);
    reg(0x57, 0x0507);
    reg(0x58, 0x1021);
    reg(0x59, 0x0703);
    reg(0xB0, 0x2501);
    reg(0xFF, 0x0000);
    reg(0x07, 0x1017); /* display on */
    delay(200);
    reg(0x03, 0x1038); /* writes advance along a row (lcdPrepareRefresh) */
    pokitto_lcd_fill_all(0);
}

uint32_t meta_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *px = (const uint16_t *)buf;
    for (unsigned r = 0; r < rows; r++, px += 160) pokitto_lcd_row(row0 + r, px);
    return ++seq;
}

uint32_t meta_lcd_done_seq(void) { return seq; }
int meta_lcd_busy(void) { return 0; }
void meta_lcd_wait(void) {}
int meta_lcd_release(void) { return 1; }
void meta_lcd_end_frame(void) {}

void meta_lcd_fill(uint16_t color) { pokitto_lcd_fill_all(color); }
