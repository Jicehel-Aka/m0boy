/* MSX cartridge identification: where a plain ROM sits in the Z80 address
 * space.  Shared by the recompiler (cartridge inspection, code generation)
 * and the runtime (page tables), so both agree on the same rule.
 *
 * A MSX BIOS recognises a cartridge by "AB" at the start of its page; the
 * header holds INIT (offset 2) and STATEMENT/DEVICE/TEXT (4, 6, 8).  This
 * profile covers unbanked ROMs, the kind that make up the first MSX1 titles:
 *   - up to 16 KiB: the page of INIT (4000-7FFF, or 8000-BFFF when INIT is
 *     there);
 *   - 16 to 32 KiB: 4000-BFFF.
 * Larger cartridges use a mapper (ASCII8/16, Konami): not covered. */
#ifndef M0_MSX_LAYOUT_H
#define M0_MSX_LAYOUT_H
#include <stddef.h>
#include <stdint.h>

#define M0_MSX_MAX_ROM 0x8000u
#define M0_MSX_PAGE 0x2000u /* granularity of the runtime's page tables */

typedef struct {
    int valid;
    uint16_t base;  /* address of the first ROM byte */
    uint16_t init;  /* INIT routine, 0: none (the cartridge only extends BASIC) */
} m0_msx_layout_t;

static inline m0_msx_layout_t m0_msx_layout(const uint8_t *rom, size_t size) {
    m0_msx_layout_t l = { 0, 0x4000, 0 };
    if (size < 16 || size > M0_MSX_MAX_ROM || rom[0] != 'A' || rom[1] != 'B') return l;
    l.init = (uint16_t)(rom[2] | rom[3] << 8);
    l.base = size <= 0x4000u && l.init >= 0x8000u && l.init < 0xc000u ? 0x8000 : 0x4000;
    /* INIT must point into the cartridge, or be absent */
    if (l.init && (l.init < l.base || l.init >= l.base + ((size + M0_MSX_PAGE - 1) & ~(M0_MSX_PAGE - 1)))) return l;
    l.valid = 1;
    return l;
}

/* Rounded up to whole runtime pages. */
static inline size_t m0_msx_padded(size_t size) { return (size + M0_MSX_PAGE - 1) & ~(size_t)(M0_MSX_PAGE - 1); }
#endif
