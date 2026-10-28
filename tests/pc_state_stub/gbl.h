/* Stub of the gbl runtime's state interface (test_pc_state): a 1 KiB
 * machine the check patterns itself. */
#pragma once
#include <stdint.h>

#define GBL_TITLE "pc state check"

uint32_t gbl_state_bytes(void);
void gbl_state_io(void (*fn)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
extern uint32_t gbl_frame;
