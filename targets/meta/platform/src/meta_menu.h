/* Option menu of the Gamebuino META frontend, after the menus of fgb (the
 * GP32 port of gnuboy): a list of lines, each either a choice among values
 * (left/right) or an action (A). */
#ifndef META_MENU_H
#define META_MENU_H

#include <stdint.h>

enum { META_MENU_CHOICE, META_MENU_ACTION };

typedef struct {
    const char *label;
    uint8_t type;
    uint8_t *value;               /* META_MENU_CHOICE: index into choices */
    uint8_t count;
    const char *const *choices;
} meta_menu_item;

/* Buttons as meta_menu_run wants them (1 = pressed). */
enum {
    META_MB_DOWN = 0x01, META_MB_LEFT = 0x02, META_MB_RIGHT = 0x04, META_MB_UP = 0x08,
    META_MB_A = 0x10, META_MB_B = 0x20, META_MB_MENU = 0x40, META_MB_HOME = 0x80,
};

/* Runs the menu until B or HOME closes it (returns -1) or A picks an
 * action (returns its line).  Draws over the whole screen with the strip
 * buffers of the display pipeline: emulation must be stopped.  Up to
 * thirteen lines; info (may be NULL) is shown under them. */
int meta_menu_run(const char *title, const meta_menu_item *items, unsigned n,
                  uint8_t (*buttons)(void), const char *info);

#endif
