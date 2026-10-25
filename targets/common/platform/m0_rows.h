/* Panel rows as the line renderers' frontends send them (SG-1000, Game
 * Gear, PICO-8, NES): two buffers, or two batches of rows, in turn.  On the
 * META the DMA sends one while the other is filled (m0_lcd_send_rows
 * returns at once with a sequence number); elsewhere the send is done when
 * it returns and the wait is a compare.  Where a renderer writes the next
 * rows matters to the caller only: m0_rows_send says which buffer is free.
 *
 *   static m0_rows rows;
 *   ... fill buffer[rows.slot] ...
 *   const unsigned free_slot = m0_rows_send(&rows, buffer[rows.slot], row0, n);
 *
 * The Pokitto's CPU writes its panel itself: its frontends push a row
 * where it is drawn (pokitto_lcd_row_zoom, 160 -> 200 wide), on every
 * panel row a growing plan gives it (m0_rows_zoom). */
#ifndef M0_ROWS_H
#define M0_ROWS_H

#include <stdint.h>
#include "m0_target.h"
#include "m0_lcd.h"
#if M0_TARGET_POKITTO
#include "pokitto_hw.h"
#endif

typedef struct {
    uint32_t seq[2]; /* each buffer's last send */
    unsigned slot;   /* the buffer being filled */
} m0_rows;

/* until buffer slot is sent */
static inline void m0_rows_wait(const m0_rows *r, unsigned slot) {
    while ((int32_t)(m0_lcd_done_seq() - r->seq[slot]) < 0) {}
}

/* n rows of buf (the current slot's) from panel row row0; returns the slot
 * to fill next, once it is free */
static inline unsigned m0_rows_send(m0_rows *r, const void *buf, unsigned row0, unsigned n) {
    r->seq[r->slot] = m0_lcd_send_rows(buf, (uint8_t)row0, (uint8_t)n);
    r->slot ^= 1;
    m0_rows_wait(r, r->slot);
    return r->slot;
}

#if M0_TARGET_POKITTO
/* a 160-pixel row on panel rows first .. end - 1, 200 wide from x = 10 */
static inline void m0_rows_zoom(const uint16_t *px, unsigned first, unsigned end) {
    for (; first < end; first++) pokitto_lcd_row_zoom(first, px);
}
#endif

#endif
