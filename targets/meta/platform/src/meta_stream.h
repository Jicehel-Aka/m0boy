/* ROM streaming from the SD card (GBL_ROM_STREAM projects). */
#ifndef META_STREAM_H
#define META_STREAM_H

#include <stdint.h>

enum {
    META_STREAM_OK = 0,
    META_STREAM_NO_CARD,     /* card or FAT volume unreadable */
    META_STREAM_NOT_FOUND,   /* GBL_STREAM_NAME not in the root or a first-level folder */
    META_STREAM_FRAGMENTED,  /* file in more than 16 pieces: copy it again */
    META_STREAM_WRONG_ROM,   /* size or header differs from the ROM this firmware was built for */
};

/* Locate and check the ROM file; card_ok is meta_sd_init's result.  Uses
 * the stream cache as scratch, so call it before gbl_reset(). */
int meta_stream_init(int card_ok);
extern uint32_t meta_stream_errors; /* sectors that could not be read */

#endif
