/*
 * Transfer queue for display strips, shared by the DMA interrupt and the
 * main loop.  Plain C and hardware-free so it can be tested on the host:
 * the includer defines lcdq_start(const lcdq_entry *) (program the window
 * and start the transfer) after including this file.
 *
 * Every submitted transfer gets a sequence number; a strip buffer may be
 * reused once lcdq_done_seq() has reached the number it was submitted with.
 */
#ifndef META_LCD_QUEUE_H
#define META_LCD_QUEUE_H

#include <stdint.h>

#ifndef LCDQ_SIZE
#define LCDQ_SIZE 4 /* pending transfers, power of two */
#endif

typedef struct {
    const void *buf;
    uint8_t row0, rows;
} lcdq_entry;

static lcdq_entry lcdq_q[LCDQ_SIZE];
static volatile uint8_t lcdq_head, lcdq_tail; /* head: next free slot */
static volatile uint8_t lcdq_active;          /* a transfer is running */
static volatile uint32_t lcdq_submitted, lcdq_done;

static void lcdq_start(const lcdq_entry *e);

static inline uint32_t lcdq_done_seq(void) { return lcdq_done; }
static inline int lcdq_busy(void) { return lcdq_active || lcdq_head != lcdq_tail; }
static inline int lcdq_full(void) { return (uint8_t)(lcdq_head - lcdq_tail) >= LCDQ_SIZE; }

/* Interrupt context: the running transfer finished. */
static inline void lcdq_on_complete(void) {
    lcdq_done++;
    if (lcdq_head != lcdq_tail) {
        lcdq_entry e = lcdq_q[lcdq_tail & (LCDQ_SIZE - 1)];
        lcdq_tail++;
        lcdq_start(&e);
    } else {
        lcdq_active = 0;
    }
}

/* Main context, with interrupts disabled around the call.  Returns the
 * sequence number of this transfer, or 0 when the queue is full (retry after
 * a completion).  When *start_now is set nothing is running and the caller
 * must call lcdq_start() itself (after re-enabling interrupts). */
static inline uint32_t lcdq_submit_locked(const void *buf, uint8_t row0, uint8_t rows, int *start_now) {
    if (lcdq_full()) return 0;
    uint32_t seq = ++lcdq_submitted;
    *start_now = !lcdq_active;
    if (lcdq_active) {
        lcdq_entry e = {buf, row0, rows};
        lcdq_q[lcdq_head & (LCDQ_SIZE - 1)] = e;
        lcdq_head++;
    } else {
        lcdq_active = 1;
    }
    return seq;
}

#endif
