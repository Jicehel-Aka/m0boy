/* Save states for the Gamebuino META: GBL_STATE_SLOTS slots in a file the
 * converters create (GBL_STATE_NAME), found at boot like the save file. */
#ifndef META_STATE_H
#define META_STATE_H

#include <stdint.h>

/* Locate the file (root or a first-level folder) and read which slots hold
 * a state of this firmware.  buf: 512 bytes of scratch. */
void meta_state_init(int card_ok, uint8_t *buf);
int meta_state_available(void);
uint16_t meta_state_used(void); /* bit per slot */
/* At an instruction boundary (gbl_platform_safe_point), buf: 512 bytes.
 * Load leaves the machine as saved (execution must start over from
 * gbr.pc); both return 0 on failure (a load then changes nothing). */
int meta_state_save(unsigned slot, uint8_t *buf);
int meta_state_load(unsigned slot, uint8_t *buf);

#endif
