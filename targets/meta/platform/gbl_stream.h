/* ROM streaming from the SD card (GBL_ROM_STREAM projects). */
#ifndef META_STREAM_H
#define META_STREAM_H

#include <stdint.h>

#include "m0_card.h"

/* meta_card.h's codes: GBL_STREAM_NAME missing, in more than 16 pieces, or
 * only with another size or header than the ROM this firmware was built
 * for. */
enum {
    META_STREAM_OK = M0_CARD_OK,
    META_STREAM_NO_CARD = M0_CARD_NO_CARD,
    META_STREAM_NOT_FOUND = M0_CARD_NOT_FOUND,
    META_STREAM_FRAGMENTED = M0_CARD_FRAGMENTED,
    META_STREAM_WRONG_ROM = M0_CARD_WRONG_ROM,
};

/* Locate and check the ROM file; card_ok is m0_sd_init's result.  Uses
 * the stream cache as scratch, so call it before gbl_reset(). */
int gbl_stream_init(int card_ok);
extern uint32_t gbl_stream_errors; /* sectors that could not be read */

#endif
