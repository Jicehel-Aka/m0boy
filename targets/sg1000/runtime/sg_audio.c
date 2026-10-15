/* SG-1000 PSG (SN76489AN): the sample generation shared with the Game Gear,
 * see targets/common/runtime/sg_psg.h. */
#include "sg1000.h"
#ifndef SG_SYSTEM_GG /* sg_config.h in generated projects */
#define SG_SYSTEM_GG 0
#endif
#include "sg_psg.h"
