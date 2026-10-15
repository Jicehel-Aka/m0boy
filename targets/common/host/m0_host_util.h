/* Helpers shared by the SG-1000 and Game Gear host harnesses; the generator
 * copies this file next to them (like m0_explore.h for the generic ones).
 * C version of rules the recompiler states once (m0boy/sg1000.h), so a
 * cartridge is read the same way on both sides. */
#ifndef M0_HOST_UTIL_H
#define M0_HOST_UTIL_H

#include <stddef.h>

/* Cartridges dumped by some copiers and emulators start with a 512-byte
 * header: the ROM itself is then a whole number of 16 KiB pages.  Returns
 * how many bytes to skip before the cartridge. */
#define M0_COPIER_HEADER_BYTES 512u
static inline size_t m0_copier_skip(size_t file_bytes) {
    return (file_bytes & 0x3fffu) == M0_COPIER_HEADER_BYTES ? M0_COPIER_HEADER_BYTES : 0u;
}

/* Sega mappers bank in whole 16 KiB pages: a ROM occupies this many bytes
 * once padded (0xff beyond the end). */
static inline size_t m0_rom_page_bytes(size_t rom_bytes) {
    return (rom_bytes + 0x3fffu) & ~(size_t)0x3fffu;
}

#endif
