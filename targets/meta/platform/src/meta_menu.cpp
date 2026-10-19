/*
 * Option menu: fgb's menu model (typed lines, a cursor, left/right to change
 * a value) drawn with dstar's greenfont, converted from Gdl's agf format to
 * 2 bits a pixel by tools/font/agf2meta.py (meta_font.h). Eight-row text bands
 * are sliced to the display scratch size, redrawn only on changes.
 */
#include "meta_menu.h"

#include "meta_target.h"
#include <string.h>
#if META_TARGET_ESPBOY
#include "espboy_hw.h"
#elif !META_TARGET_POKITTO
#include "meta_hw.h"
#endif

#include "meta_clock.h"
#include "meta_display.h"
#include "meta_font.h"
#include "meta_lcd.h"

static const uint16_t bg = META_COLOR(0x081820);
static const uint16_t bar = META_COLOR(0x24524A);   /* selected line */
static const uint16_t title_bg = META_COLOR(0x346856);

/* Bands of 8 rows over the picture: 16 on the META, 18 on the Pokitto. */
#define BANDS (META_SCREEN_ROWS / 8)

static inline void feed_watchdog(void) {
#if !META_TARGET_POKITTO
    meta_watchdog_feed();
#endif
}

static void wait_ms(uint32_t ms) {
    uint32_t t = meta_micros();
    while (meta_micros() - t < ms * 1000u) {}
}

static uint16_t to565(uint32_t rgb) { return (uint16_t)META_COLOR(rgb); }

static uint16_t font_colour[3];

/* A vertical glyph slice (2 bits a pixel, 0 transparent) at buffer row 0. */
static void draw_glyph(uint16_t *band, int x, unsigned c, unsigned first, unsigned rows) {
    for (unsigned r = first; r < first + rows; r++) {
        uint32_t bits = meta_font_rows[c][r];
        uint16_t *d = band + (r - first) * 160 + x;
        for (int i = 0; bits; i++, bits = (bits << 2) & 0xFFFFu) {
            unsigned v = bits >> 14;
            if (v && x + i >= 0 && x + i < 160) d[i] = font_colour[v - 1];
        }
    }
}

static unsigned glyph(char ch) {
    unsigned c = (uint8_t)ch - META_FONT_FIRST;
    return c < META_FONT_COUNT ? c : (unsigned)('*' - META_FONT_FIRST);
}

static int text_width(const char *s) {
    int w = 0;
    for (; *s; s++) w += meta_font_width[glyph(*s)];
    return w;
}

static void draw_text(uint16_t *band, int x, const char *s, unsigned first, unsigned rows) {
    for (; *s; s++) {
        unsigned c = glyph(*s);
        draw_glyph(band, x, c, first, rows);
        x += meta_font_width[c];
    }
}

/* Bands of 8 rows: the title on band 0, item i on band 1 + i, the
 * information line on the band before last, the hint on the last. */
static void draw(uint16_t *buf, unsigned capacity, const char *title, const meta_menu_item *items, unsigned n,
                 unsigned sel, const char *info) {
    for (int b = 0; b < BANDS; b++) {
        int item = b - 1;
        if (item >= (int)n) item = -1;
        uint16_t fill = b == 0 ? title_bg : (item >= 0 && (unsigned)item == sel) ? bar : bg;
        for (unsigned first = 0; first < 8;) {
            unsigned rows = capacity < 8 - first ? capacity : 8 - first;
            for (int i = 0; i < 160 * (int)rows; i++) buf[i] = fill;
            if (b == 0) draw_text(buf, (160 - text_width(title)) / 2, title, first, rows);
            if (b == BANDS - 2 && info) draw_text(buf, (160 - text_width(info)) / 2, info, first, rows);
            if (b == BANDS - 1) draw_text(buf, 4, "A:ok   B:retour", first, rows);
            if (item >= 0) {
                const meta_menu_item *it = &items[item];
                draw_text(buf, 4, it->label, first, rows);
                if (it->type == META_MENU_CHOICE) {
                    const char *v = it->choices[*it->value];
                    int w = text_width(v);
                    if ((unsigned)item == sel) {
                        draw_text(buf, 160 - 4 - w - 16, "<", first, rows);
                        draw_text(buf, 160 - 4 - 8, ">", first, rows);
                    }
                    draw_text(buf, 160 - 4 - 8 - 4 - w, v, first, rows);
                }
            }
            meta_lcd_send_rows(buf, (uint8_t)(b * 8 + first), (uint8_t)rows);
            meta_lcd_wait(); /* the buffer is reused for the next band */
            first += rows;
        }
    }
    meta_lcd_end_frame();
}

int meta_menu_run(const char *title, const meta_menu_item *items, unsigned n,
                  uint8_t (*buttons)(void), const char *info) {
    unsigned rows;
    for (unsigned i = 0; i < 3; i++) font_colour[i] = to565(meta_font_colours[i]);
    uint16_t *buf = meta_display_scratch(&rows);
    meta_lcd_wait();
    if (!rows) return -1;
    unsigned sel = 0;
    uint8_t held = buttons(); /* ignore what is already down (HOME) */
    int redraw = 1, result = -1;
    for (;;) {
        feed_watchdog();
        if (redraw) {
            draw(buf, rows, title, items, n, sel, info);
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
        feed_watchdog();
        wait_ms(10);
    }
    return result;
}
