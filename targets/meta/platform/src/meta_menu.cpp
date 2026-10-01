/*
 * Option menu: fgb's menu model (typed lines, a cursor, left/right to change
 * a value) drawn with a Gdl font in its 4-bit agf format (meta_font.h, from
 * the greenfont of dstar).  The screen is drawn in 8-pixel bands through the
 * display's strip buffers, redrawn only when something changes.
 */
#include "meta_menu.h"

#include <Arduino.h>
#include <string.h>

#include "meta_clock.h"
#include "meta_display.h"
#include "meta_font.h"
#include "meta_lcd.h"

#define RGB565_BE(rgb)                                                                         \
    ((uint16_t)(((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) >> 8 | \
                ((((rgb) >> 8) & 0xF800u) | (((rgb) >> 5) & 0x07E0u) | (((rgb) >> 3) & 0x001Fu)) << 8))

static const uint16_t bg = RGB565_BE(0x081820);
static const uint16_t bar = RGB565_BE(0x24524A);   /* selected line */
static const uint16_t title_bg = RGB565_BE(0x346856);

/* delay() assumes a 48 MHz SysTick. */
static void wait_ms(uint32_t ms) {
    uint32_t t = meta_micros();
    while (meta_micros() - t < ms * 1000u) {}
}

static uint16_t to565(uint32_t rgb) { return (uint16_t)RGB565_BE(rgb); }

/* One agf glyph (Gdl's 4-bit RLE: "agf", colours, sx, sy, palette, then per
 * line a run count and runs of jump, size, two pixels a byte), clipped to
 * the band [band_y, band_y + 8). */
static void draw_glyph(uint16_t *band, int band_y, int x, int y, const uint8_t *g) {
    unsigned colours = g[3];
    unsigned sy = g[10] | g[11] << 8;
    const uint8_t *p = g + 16;
    uint16_t pal[16];
    for (unsigned i = 0; i < colours && i < 16; i++, p += 4)
        pal[i] = to565((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16);
    for (unsigned l = 0; l < sy; l++) {
        unsigned runs = *p++;
        int row = y + (int)l - band_y;
        int px = x;
        while (runs--) {
            unsigned jump = p[0], size = p[1];
            p += 2;
            px += (int)jump;
            for (unsigned i = 0; i < size; i++) {
                unsigned c = (i & 1) ? (p[i >> 1] & 15u) : (p[i >> 1] >> 4);
                if (row >= 0 && row < 8 && px + (int)i >= 0 && px + (int)i < 160 && c < colours)
                    band[row * 160 + px + (int)i] = pal[c];
            }
            p += (size + 1) >> 1;
            px += (int)size;
        }
    }
}

static int text_width(const char *s) {
    int w = 0;
    for (; *s; s++) {
        unsigned c = (uint8_t)*s - META_FONT_FIRST;
        w += c < META_FONT_COUNT ? meta_font[c][8] : 8;
    }
    return w;
}

static void draw_text(uint16_t *band, int band_y, int x, int y, const char *s) {
    for (; *s; s++) {
        unsigned c = (uint8_t)*s - META_FONT_FIRST;
        if (c >= META_FONT_COUNT) c = '*' - META_FONT_FIRST;
        draw_glyph(band, band_y, x, y, meta_font[c]);
        x += meta_font[c][8];
    }
}

/* Lines: the title on band 0, item i on band 2 + 2i, the hint on band 15. */
static void draw(uint16_t *buf, const char *title, const meta_menu_item *items, unsigned n,
                 unsigned sel) {
    for (int b = 0; b < 16; b++) {
        int y = b * 8;
        uint16_t fill = b == 0 ? title_bg : bg;
        int item = (b >= 2 && !(b & 1)) ? (b - 2) / 2 : -1;
        if (item >= (int)n) item = -1;
        if (item >= 0 && (unsigned)item == sel) fill = bar;
        for (int i = 0; i < 160 * 8; i++) buf[i] = fill;
        if (b == 0) draw_text(buf, y, (160 - text_width(title)) / 2, y, title);
        if (b == 15) draw_text(buf, y, 4, y, "A:ok   B:retour");
        if (item >= 0) {
            const meta_menu_item *it = &items[item];
            draw_text(buf, y, 4, y, it->label);
            if (it->type == META_MENU_CHOICE) {
                const char *v = it->choices[*it->value];
                int w = text_width(v);
                if ((unsigned)item == sel) {
                    draw_text(buf, y, 160 - 4 - w - 16, y, "<");
                    draw_text(buf, y, 160 - 4 - 8, y, ">");
                }
                draw_text(buf, y, 160 - 4 - 8 - 4 - w, y, v);
            }
        }
        meta_lcd_send_rows(buf, (uint8_t)y, 8);
        meta_lcd_wait(); /* the buffer is reused for the next band */
    }
    meta_lcd_end_frame();
}

int meta_menu_run(const char *title, const meta_menu_item *items, unsigned n,
                  uint8_t (*buttons)(void)) {
    unsigned rows;
    uint16_t *buf = meta_display_scratch(&rows);
    meta_lcd_wait();
    if (rows < 8) return -1;
    unsigned sel = 0;
    uint8_t held = buttons(); /* ignore what is already down (HOME) */
    int redraw = 1, result = -1;
    for (;;) {
        if (WDT->CTRL.reg & WDT_CTRL_ENABLE) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
        if (redraw) {
            draw(buf, title, items, n, sel);
            redraw = 0;
        }
        wait_ms(15);
        uint8_t now = buttons();
        uint8_t press = (uint8_t)(now & ~held);
        held = now;
        if (!press) continue;
        const meta_menu_item *it = &items[sel];
        if (press & (META_MB_B | META_MB_HOME)) break;
        if (press & META_MB_UP) sel = sel ? sel - 1 : n - 1;
        if (press & META_MB_DOWN) sel = sel + 1 < n ? sel + 1 : 0;
        if (it->type == META_MENU_CHOICE) {
            if ((press & META_MB_LEFT) && *it->value) (*it->value)--;
            if ((press & META_MB_RIGHT) && *it->value + 1 < it->count) (*it->value)++;
            if (press & META_MB_A) *it->value = (uint8_t)((*it->value + 1) % it->count);
        } else if (press & META_MB_A) {
            result = (int)sel;
            break;
        }
        redraw = 1;
    }
    /* Let go of everything before the game reads the buttons again. */
    while (buttons()) {
        if (WDT->CTRL.reg & WDT_CTRL_ENABLE) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
        wait_ms(10);
    }
    return result;
}
