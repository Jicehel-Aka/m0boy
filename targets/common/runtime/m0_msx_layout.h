/* MSX cartridge identification: where a ROM sits in the Z80 address space and
 * how its banks are selected.  Shared by the recompiler (cartridge
 * inspection, code generation) and the runtime (page tables), so both agree
 * on the same rules.
 *
 * A MSX BIOS recognises a cartridge by "AB" at the start of its page; the
 * header holds INIT (offset 2) and STATEMENT/DEVICE/TEXT (4, 6, 8).
 *   - Unbanked ROMs, up to 32 KiB: up to 16 KiB at the page of INIT (4000-7FFF,
 *     or 8000-BFFF when INIT is there), 16 to 32 KiB at 4000-BFFF.
 *   - Larger ROMs use a mapper, the ROM seen as 8 KiB units through four
 *     windows 4000-5FFF, 6000-7FFF, 8000-9FFF and A000-BFFF:
 *       ASCII8     registers 6000, 6800, 7000, 7800 select the unit of each window
 *       ASCII16    registers 6000 and 7000 select a 16 KiB bank (two units) of
 *                  4000-7FFF and 8000-BFFF
 *       Konami     4000-5FFF is unit 0; writes to 6000-7FFF, 8000-9FFF and
 *                  A000-BFFF select the unit of that window
 *       Konami SCC registers 5000, 7000, 9000 and B000 (the sound chip is not emulated)
 *     Windows come up on units 0 1 2 3 (Konami), 0 (the others). */
#ifndef M0_MSX_LAYOUT_H
#define M0_MSX_LAYOUT_H
#include <stddef.h>
#include <stdint.h>

#define M0_MSX_MAX_UNBANKED 0x8000u
#define M0_MSX_MAX_ROM 0x100000u
#define M0_MSX_PAGE 0x2000u /* an 8 KiB unit: the granularity of the runtime's page tables */

enum { M0_MSX_PLAIN = 0, M0_MSX_ASCII8 = 1, M0_MSX_ASCII16 = 2, M0_MSX_KONAMI = 3, M0_MSX_KONAMI_SCC = 4 };

static inline const char *m0_msx_mapper_name(unsigned mapper) {
    static const char *const names[] = { "plain", "ascii8", "ascii16", "konami", "konami-scc" };
    return mapper < 5 ? names[mapper] : "?";
}

/* A cartridge larger than 32 KiB: the mapper whose registers the code writes
 * to most (counted over LD (nn),A, the usual way to select a bank). */
static inline unsigned m0_msx_mapper_guess(const uint8_t *rom, size_t size) {
    if (size <= M0_MSX_MAX_UNBANKED) return M0_MSX_PLAIN;
    unsigned hits[5][4] = {{0}};
    for (size_t i = 0; i + 2 < size; i++) {
        if (rom[i] != 0x32) continue;
        const unsigned a = rom[i + 1] | rom[i + 2] << 8;
        static const uint16_t table[5][4] = {
            {0, 0, 0, 0}, {0x6000, 0x6800, 0x7000, 0x7800}, {0x6000, 0x7000, 0x77ff, 0},
            {0x6000, 0x8000, 0xa000, 0}, {0x5000, 0x7000, 0x9000, 0xb000} };
        for (unsigned m = 1; m < 5; m++)
            for (unsigned r = 0; r < 4; r++) if (table[m][r] && a == table[m][r]) hits[m][r]++;
    }
    unsigned best = M0_MSX_KONAMI, best_score = 0;
    static const unsigned order[4] = { M0_MSX_ASCII16, M0_MSX_KONAMI, M0_MSX_ASCII8, M0_MSX_KONAMI_SCC }; /* ties: this order */
    for (unsigned k = 0; k < 4; k++) {
        const unsigned m = order[k];
        unsigned score = 0, distinct = 0;
        for (unsigned r = 0; r < 4; r++) { score += hits[m][r]; distinct += hits[m][r] != 0; }
        score += distinct * 4; /* several registers written: more telling than one written often */
        if (score > best_score) { best_score = score; best = m; }
    }
    return best_score ? best : M0_MSX_ASCII16;
}

/* The unit (8 KiB) a window shows at power on; w is the window, 2..5. */
static inline unsigned m0_msx_initial_unit(unsigned mapper, unsigned w) {
    return mapper == M0_MSX_KONAMI || mapper == M0_MSX_KONAMI_SCC ? w - 2 : mapper == M0_MSX_ASCII16 ? (w - 2) & 1 : 0;
}

/* Does a write to this address reach a bank register? */
static inline int m0_msx_is_register(unsigned mapper, unsigned a) {
    switch (mapper) {
    case M0_MSX_ASCII8: return a >= 0x6000 && a < 0x8000;
    case M0_MSX_ASCII16: return (a >= 0x6000 && a < 0x6800) || (a >= 0x7000 && a < 0x7800);
    case M0_MSX_KONAMI: return a >= 0x6000 && a < 0xc000;
    case M0_MSX_KONAMI_SCC: return (a & 0x1800) == 0x1000 && a >= 0x5000 && a < 0xc000;
    default: return 0;
    }
}

/* A write of v to a: which windows change to which unit.  Fills window[2..5]
 * (-1: unchanged) from the unit count of the cartridge and returns 1 when a
 * register was written. */
static inline int m0_msx_bank_write(unsigned mapper, unsigned a, unsigned v, unsigned units, int window[6]) {
    for (unsigned w = 0; w < 6; w++) window[w] = -1;
    if (!units) return 0;
    switch (mapper) {
    case M0_MSX_ASCII8:
        if (a < 0x6000 || a >= 0x8000) return 0;
        window[2 + ((a >> 11) & 3)] = (int)(v % units);
        return 1;
    case M0_MSX_ASCII16: {
        if (a < 0x6000 || a >= 0x8000 || ((a >> 11) & 1)) return 0;
        const unsigned base = (a & 0x1000) ? 4 : 2, bank = (v * 2) % units;
        window[base] = (int)bank;
        window[base + 1] = (int)((bank + 1) % units);
        return 1;
    }
    case M0_MSX_KONAMI:
        if (a < 0x6000 || a >= 0xc000) return 0;
        window[a >> 13] = (int)(v % units);
        return 1;
    case M0_MSX_KONAMI_SCC:
        if (a < 0x5000 || a >= 0xc000 || (a & 0x1800) != 0x1000) return 0;
        window[a >> 13] = (int)(v % units);
        return 1;
    default: return 0;
    }
}

typedef struct {
    int valid;
    uint16_t base;   /* address of the first ROM byte (4000 for mapped ROMs) */
    uint16_t init;   /* INIT routine, 0: none (the cartridge only extends BASIC) */
    unsigned mapper; /* M0_MSX_* */
} m0_msx_layout_t;

static inline m0_msx_layout_t m0_msx_layout(const uint8_t *rom, size_t size) {
    m0_msx_layout_t l = { 0, 0x4000, 0, M0_MSX_PLAIN };
    if (size < 16 || size > M0_MSX_MAX_ROM || rom[0] != 'A' || rom[1] != 'B') return l;
    l.init = (uint16_t)(rom[2] | rom[3] << 8);
    l.mapper = m0_msx_mapper_guess(rom, size);
    if (l.mapper == M0_MSX_PLAIN) {
        l.base = size <= 0x4000u && l.init >= 0x8000u && l.init < 0xc000u ? 0x8000 : 0x4000;
        /* INIT must point into the cartridge, or be absent */
        if (l.init && (l.init < l.base || l.init >= l.base + ((size + M0_MSX_PAGE - 1) & ~(M0_MSX_PAGE - 1)))) return l;
    } else if (l.init && (l.init < 0x4000 || l.init >= 0xc000)) return l;
    l.valid = 1;
    return l;
}

/* Rounded up to whole runtime pages. */
static inline size_t m0_msx_padded(size_t size) { return (size + M0_MSX_PAGE - 1) & ~(size_t)(M0_MSX_PAGE - 1); }
#endif
