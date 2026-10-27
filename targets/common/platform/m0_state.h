/* Save states: GBL_STATE_SLOTS slots in a file the converters create
 * (GBL_STATE_NAME), found at boot like the save file.  The PC keeps the
 * same slot area in a plain file of its own, created on the first save. */
#ifndef META_STATE_H
#define META_STATE_H

#include <stdint.h>

/* Locate the file (root or a first-level folder) and read which slots hold
 * a state of this firmware.  buf: 512 bytes of scratch. */
void m0_state_init(int card_ok, uint8_t *buf);
int m0_state_available(void);
uint16_t m0_state_used(void); /* bit per slot */
/* At an instruction boundary (gbl_platform_safe_point), buf: 512 bytes.
 * Load leaves the machine as saved (execution must start over from
 * gbr.pc); both return 0 on failure (a load then changes nothing). */
int m0_state_save(unsigned slot, uint8_t *buf);
int m0_state_load(unsigned slot, uint8_t *buf);

#endif
