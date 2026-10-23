/* Cartridge identification and Super Game 150's six 8 KiB windows.
 * Hardware mapping documented by the cartridge's dumper:
 * https://www.smspower.org/forums/19752-DumpingSuperGame150
 * Kept independent of the CPU and storage backend (host, SD, ESP flash). */
#ifndef M0_SEGA_MAPPER_H
#define M0_SEGA_MAPPER_H
#include <stddef.h>
#include <stdint.h>

enum { M0_MAPPER_SEGA = 0, M0_MAPPER_CODEMASTERS = 1, M0_MAPPER_KOREAN150 = 2 };

static inline unsigned m0_sega_mapper(const uint8_t *rom, size_t size) {
    if (size == 0x200000u) {
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < size; i++) {
            crc ^= rom[i];
            for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        if ((crc ^ 0xffffffffu) == 0x1b8956d1u) return M0_MAPPER_KOREAN150;
    }
    if (size >= 0x8000u) {
        unsigned sum = rom[0x7fe6] | (unsigned)rom[0x7fe7] << 8;
        unsigned inverse = rom[0x7fe8] | (unsigned)rom[0x7fe9] << 8;
        if (sum && !((sum + inverse) & 0xffffu)) return M0_MAPPER_CODEMASTERS;
    }
    return M0_MAPPER_SEGA;
}

static inline uint8_t m0_korean150_page(unsigned mode, unsigned bank, unsigned slot) {
    unsigned base = ((mode & 0x10u) << 3) + ((bank & 0x3eu) << 1);
    unsigned pair = (bank & 1u) << 1;
    switch (mode & 0xe0u) {
    case 0x00: return slot < 4 ? (uint8_t)(base + pair + (slot & 1u)) : 0xff;
    case 0x20: return slot < 4 ? (uint8_t)(base + slot) : 0xff;
    case 0x40:
        if (slot < 2) return (uint8_t)(0x80u + slot);
        return slot < 4 ? (uint8_t)(base + pair + slot - 2) : 0xff;
    case 0x60:
        if (slot < 2) return (uint8_t)(0x80u + slot);
        return slot < 4 ? (uint8_t)(0xfeu + slot - 2) : (uint8_t)(base + pair + slot - 4);
    case 0x80:
        if (slot < 2) return (uint8_t)(0x80u + slot);
        return (uint8_t)(base + pair + (slot == 3 || slot == 4));
    case 0xa0:
        return slot < 2 ? (uint8_t)(0x80u + slot) : (uint8_t)(base + ((slot - 2) ^ pair));
    default: return 0xff;
    }
}
#endif
