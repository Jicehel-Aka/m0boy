#include "md.h"

const uint32_t md_rom_size = 8;
const uint8_t md_rom[8] = {0, 0xff, 0xf0, 0, 0, 0, 2, 0};

int main(void) {
    Md m;
    md_reset(&m);
    if (md_read8(&m, 0xa10003u) != 0xffu) return 1;
    m.pad = MD_UP | MD_A | MD_START;
    if (md_read8(&m, 0xa10003u) != 0xfeu) return 2;
    md_write8(&m, 0xa10003u, 0x00u);
    if (md_read8(&m, 0xa10003u) != 0x82u) return 3;
    md_write8(&m, 0xa10009u, 0x40u);
    if (md_read8(&m, 0xa10009u) != 0x40u) return 4;
    if (m.fault) return 5;
    return 0;
}
