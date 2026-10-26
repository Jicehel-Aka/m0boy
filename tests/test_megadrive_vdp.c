#include "md.h"
#include <assert.h>
#include <stdlib.h>

const uint8_t md_rom[512] = {0};
const uint32_t md_rom_size = sizeof md_rom;

static void vdp_address(Md *m, unsigned address, unsigned code) {
    md_write16(m, 0xc00004u, (uint16_t)((address & 0x3fffu) | ((code & 3u) << 14)));
    md_write16(m, 0xc00004u, (uint16_t)(((address >> 14) & 3u) | ((code & 12u) << 2)));
}

int main(void) {
    Md *m = calloc(1, sizeof *m);
    uint16_t *pixels = malloc(320u * 224u * sizeof *pixels);
    assert(m && pixels);
    md_reset(m);
    md_write8(m, 0xff0000u, 0x12);
    md_write8(m, 0xfffffeu, 0x34);
    assert(md_read8(m, 0xff0000u) == 0x12);
    assert(md_read8(m, 0xfffffeu) == 0x34);
    /* CRAM colour 1: maximum red. */
    vdp_address(m, 2, 3);
    md_write16(m, 0xc00000u, 0x000e);
    /* Tile 1, first row: index 1, then index 0. */
    vdp_address(m, 32, 1);
    md_write16(m, 0xc00000u, 0x1000);
    /* Plane A at $c000, first entry points at tile 1. */
    md_write16(m, 0xc00004u, 0x8230);
    vdp_address(m, 0xc000, 1);
    md_write16(m, 0xc00000u, 1);
    md_write16(m, 0xc00004u, 0x8140);
    md_render_rgb565(m, pixels, 320);
    assert(!m->fault);
    assert(pixels[0] == 0xf800);
    assert(pixels[1] == 0);
    assert(pixels[320] == 0);
    free(pixels);
    free(m);
    return 0;
}
