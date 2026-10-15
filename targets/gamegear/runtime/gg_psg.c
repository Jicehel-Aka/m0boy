/* Game Gear PSG (the SN76489 inside the Sega VDP): the sample generation
 * shared with the SG-1000, see targets/common/runtime/sg_psg.h. */
#include "gg.h"
#ifndef SG_SYSTEM_GG /* sg_config.h in generated projects */
#define SG_SYSTEM_GG 1
#endif
#include "sg_psg.h"
