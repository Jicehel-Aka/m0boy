/* The consoles' pads, decoded once into the menu's META_MB_* order
 * (meta_menu.h: DOWN, LEFT, RIGHT, UP, A, B, MENU, HOME).  Every frontend
 * reads its raw bits (meta_pad_read, espboy_buttons, pokitto_buttons —
 * SG_PAD_RAW in the Sega and PICO-8 frontends), then maps the game's
 * buttons from META_PAD(raw): one mapping everywhere, and the genuinely
 * per-game cases (the Pokitto C's tap, the Game Boy's A/B swap, the Sega
 * START) read the same named bits.
 *
 * A macro on purpose, like sg_frontend.h's: the firmwares see the
 * statements they had, so a refactor keeps firmware.bin comparable with
 * the previous sources. */
#ifndef META_PAD_H
#define META_PAD_H

#include "meta_target.h"

#if META_TARGET_POKITTO
#include "pokitto_hw.h"
/* A, B, C, up, down, left, right (pokitto_buttons).  C plays the META's
 * HOME (the menu key, sg_menu.h); there is no MENU key: the C tap is each
 * frontend's own. */
#define META_PAD(raw) \
    __extension__({ \
        uint8_t pk_ = (uint8_t)(raw); \
        uint8_t mb_ = 0; \
        if (pk_ & POKITTO_A) mb_ |= META_MB_A; \
        if (pk_ & POKITTO_B) mb_ |= META_MB_B; \
        if (pk_ & POKITTO_C) mb_ |= META_MB_HOME; \
        if (pk_ & POKITTO_UP) mb_ |= META_MB_UP; \
        if (pk_ & POKITTO_DOWN) mb_ |= META_MB_DOWN; \
        if (pk_ & POKITTO_LEFT) mb_ |= META_MB_LEFT; \
        if (pk_ & POKITTO_RIGHT) mb_ |= META_MB_RIGHT; \
        mb_; \
    })
#else
/* META and ESPboy: the library's order, the META_MB_* values as the
 * drivers return them.  META_BUTTONS_LAPINOU_ORDER (gbl_board.h): the
 * homebrew pads read their own order out of the same shift register
 * (left, right, up, A, B, MENU, down, HOME), decoded here. */
#ifdef META_BUTTONS_LAPINOU_ORDER
#define META_PAD(raw) \
    __extension__({ \
        uint8_t pk_ = (uint8_t)(raw); \
        uint8_t mb_ = (uint8_t)(pk_ & 0x80u); \
        if (pk_ & 0x01u) mb_ |= META_MB_LEFT; \
        if (pk_ & 0x02u) mb_ |= META_MB_RIGHT; \
        if (pk_ & 0x04u) mb_ |= META_MB_UP; \
        if (pk_ & 0x08u) mb_ |= META_MB_A; \
        if (pk_ & 0x10u) mb_ |= META_MB_B; \
        if (pk_ & 0x20u) mb_ |= META_MB_MENU; \
        if (pk_ & 0x40u) mb_ |= META_MB_DOWN; \
        mb_; \
    })
#else
#define META_PAD(raw) ((uint8_t)(raw))
#endif
#endif

#endif
