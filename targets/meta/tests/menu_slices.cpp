/* Same complete menu with 8/4/3/2/1 scratch rows, with guard words to catch
 * writes past the short buffer. Host only: no peripheral registers. */
#include "meta_menu.h"
#include "meta_display.h"
#include "meta_lcd.h"
#include <cassert>
#include <cstring>
#include <cstdio>
static uint16_t scratch[160 * 8 + 32], panel[128][160], reference_panel[128][160];
static unsigned capacity, calls, sends;
static unsigned ticks;
extern "C" void meta_watchdog_feed(void) {}
extern "C" uint32_t meta_micros(void) { return ++ticks * 1000; }
extern "C" uint16_t *meta_display_scratch(unsigned *rows) { *rows = capacity; return scratch + 16; }
extern "C" void meta_lcd_wait(void) {}
extern "C" void meta_lcd_end_frame(void) {}
extern "C" uint32_t meta_lcd_send_rows(const void *buf, uint8_t row, uint8_t rows) {
    assert(rows && rows <= capacity && row + rows <= 128);
    memcpy(panel[row], buf, rows * 160 * sizeof(uint16_t));
    ++sends; return sends;
}
static uint8_t buttons(void) { return calls++ == 1 ? META_MB_A : 0; }
int main(void) {
    uint8_t value = 1;
    const char *choices[] = {"Zero", "Un", "Deux"};
    const meta_menu_item items[] = {
        {"Reprendre", META_MENU_ACTION, 0, 0, 0},
        {"Volume", META_MENU_CHOICE, &value, 3, choices},
        {"Quitter", META_MENU_ACTION, 0, 0, 0}
    };
    const unsigned sizes[] = {8,4,3,2,1};
    for (unsigned rows : sizes) {
        capacity = rows; calls = sends = 0;
        for (auto &v : scratch) v = 0xBEEF;
        memset(panel, 0, sizeof(panel));
        assert(meta_menu_run("OPTIONS", items, 3, buttons, "59 ips, 41 vues") == 0);
        for (unsigned i = 0; i < 16; ++i) assert(scratch[i] == 0xBEEF);
        for (unsigned i = 16 + capacity * 160; i < sizeof(scratch)/sizeof(*scratch); ++i)
            assert(scratch[i] == 0xBEEF);
        if (rows == 8) memcpy(reference_panel, panel, sizeof(panel));
        else assert(memcmp(reference_panel, panel, sizeof(panel)) == 0);
    }
    puts("menu pixels identical with 8/4/3/2/1 scratch rows; guards intact");
}
