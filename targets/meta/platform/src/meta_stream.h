/* ROM streaming from the SD card (GBL_ROM_STREAM projects). */
#ifndef META_STREAM_H
#define META_STREAM_H

#include <stdint.h>

#include "meta_card.h"

/* meta_card.h's codes: GBL_STREAM_NAME missing, in more than 16 pieces, or
 * only with another size or header than the ROM this firmware was built
 * for. */
enum {
    META_STREAM_OK = META_CARD_OK,
    META_STREAM_NO_CARD = META_CARD_NO_CARD,
    META_STREAM_NOT_FOUND = META_CARD_NOT_FOUND,
    META_STREAM_FRAGMENTED = META_CARD_FRAGMENTED,
    META_STREAM_WRONG_ROM = META_CARD_WRONG_ROM,
};

/* Locate and check the ROM file; card_ok is meta_sd_init's result.  Uses
 * the stream cache as scratch, so call it before gbl_reset(). */
int meta_stream_init(int card_ok);
extern uint32_t meta_stream_errors; /* sectors that could not be read */

#endif
