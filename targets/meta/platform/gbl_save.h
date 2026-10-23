/* Cartridge RAM saves on the SD card (GBL_BATTERY / GBL_ERAM_ON_SD). */
#ifndef META_SAVE_H
#define META_SAVE_H

#include <stdint.h>

enum {
    META_SAVE_OK = 0,
    META_SAVE_NO_FILE,  /* battery RAM kept in RAM only: this session is not saved */
    META_SAVE_MISSING,  /* cartridge RAM must live in the file and it is missing */
};

/* Locate GBL_SAVE_NAME (root or a first-level folder); card_ok is
 * m0_sd_init's result.  Uses gbl_save_scratch, so call before gbl_reset. */
int gbl_save_init(int card_ok);
/* Once per frame: write back changes the game has stopped making (2 s). */
void gbl_save_tick(void);
extern uint32_t gbl_save_writes, gbl_save_errors;

#endif
