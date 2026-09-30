/*
 * Host test for the META display transfer queue (platform/src/meta_lcd_queue.h):
 * random submissions against a fake DMA engine whose completions arrive at
 * random times, as the interrupt would.  Checks FIFO order, a single active
 * transfer, sequence numbers matching completions, and no lost transfer.
 */
#include <stdio.h>
#include <stdlib.h>

#define LCDQ_SIZE 4
#include "meta_lcd_queue.h"

static int active_id = -1, started = 0, failures = 0;

static void lcdq_start(const lcdq_entry *e) {
    int id = (int)(intptr_t)e->buf;
    if (active_id >= 0) { failures++; printf("start while busy\n"); }
    if (id != started) { failures++; printf("out of order: %d expected %d\n", id, started); }
    started++;
    active_id = id;
}

int main(void) {
    srand(7);
    int submitted = 0;
    uint32_t last_seq = 0;
    for (int step = 0; step < 200000; step++) {
        if (rand() % 3 == 0 && submitted < 50000) {
            int start_now;
            uint32_t seq = lcdq_submit_locked((const void *)(intptr_t)submitted, 0, 4, &start_now);
            if (seq) {
                if (seq != last_seq + 1) { failures++; printf("sequence gap\n"); }
                last_seq = seq;
                if (start_now) {
                    lcdq_entry e = {(const void *)(intptr_t)submitted, 0, 4};
                    lcdq_start(&e);
                }
                submitted++;
            }
        }
        if (active_id >= 0 && rand() % 4 == 0) {
            int done = active_id;
            active_id = -1;
            lcdq_on_complete();
            if (lcdq_done_seq() != (uint32_t)done + 1) { failures++; printf("done seq mismatch\n"); }
        }
    }
    while (active_id >= 0) { active_id = -1; lcdq_on_complete(); }
    if (started != submitted || lcdq_done_seq() != (uint32_t)submitted || lcdq_busy()) {
        failures++;
        printf("lost transfers: submitted %d started %d done %u\n", submitted, started, lcdq_done_seq());
    }
    if (failures) return 1;
    printf("lcd_queue: %d transfers in order, one at a time, none lost\n", submitted);
    return 0;
}
