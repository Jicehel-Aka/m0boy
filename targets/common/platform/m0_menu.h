/* Option menu of the Gamebuino META frontend, after the menus of fgb (the
 * GP32 port of gnuboy): a list of lines, each either a choice among values
 * (left/right) or an action (A).  C names: the menu is drawn by C display
 * drivers too (meta_display.c, pokitto_display.c). */
#ifndef META_MENU_H
#define META_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { META_MENU_CHOICE, META_MENU_ACTION };

typedef struct {
    const char *label;
    uint8_t type;
    uint8_t *value;               /* META_MENU_CHOICE: index into choices */
    uint8_t count;
    const char *const *choices;
} meta_menu_item;

/* Buttons as m0_menu_run wants them (1 = pressed). */
enum {
    M0_MB_DOWN = 0x01, M0_MB_LEFT = 0x02, M0_MB_RIGHT = 0x04, M0_MB_UP = 0x08,
    M0_MB_A = 0x10, M0_MB_B = 0x20, M0_MB_MENU = 0x40, M0_MB_HOME = 0x80,
};

/* Runs the menu until B or HOME closes it (returns -1) or A picks an
 * action (returns its line).  Draws over the whole screen with the strip
 * buffers of the display pipeline: emulation must be stopped.  Up to
 * fourteen lines without the info line, thirteen with it; info (may be
 * NULL) is shown under them. */
int m0_menu_run(const char *title, const meta_menu_item *items, unsigned n,
                  uint8_t (*buttons)(void), const char *info);

/* The buffer the menu draws through: rows of 160 pixels, *rows of them at
 * least 1 (bands of 8 rows are sliced to it).  Each frontend lends its own:
 * the display pipeline's strips (meta_display.c, pokitto_display.c) or the
 * line buffers (the Sega and PICO-8 frontends). */
uint16_t *m0_menu_scratch(unsigned *rows);

#ifdef __cplusplus
}
#endif

#endif
