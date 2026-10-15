/* Helpers shared by the SG-1000 and Game Gear host harnesses; the generator
 * copies this file next to them (like m0_explore.h for the generic ones).
 * C version of rules the recompiler states once (m0boy/sg1000.h), plus the
 * parts of the two harnesses that are identical once the console's name is
 * set aside: cartridge loading, WAV captures, input scripts, run hashing
 * and the coverage dump. */
#ifndef M0_HOST_UTIL_H
#define M0_HOST_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Read a cartridge file (a FILE opened at its start) into memory: skip a
 * copier header, refuse more than max_bytes, and when pad_pages is set pad
 * to whole 16 KiB pages with 0xff.  Returns the buffer to free and its
 * length in *size, or 0 with nothing allocated. */
static inline uint8_t *m0_rom_load(FILE *file, size_t max_bytes, int pad_pages, size_t *size) {
    if (fseek(file, 0, SEEK_END)) return 0;
    const long file_bytes = ftell(file);
    if (file_bytes <= 0 || (unsigned long)file_bytes > max_bytes + M0_COPIER_HEADER_BYTES) return 0;
    const size_t skip = m0_copier_skip((size_t)file_bytes);
    const size_t bytes = (size_t)file_bytes - skip;
    if (!bytes || bytes > max_bytes) return 0;
    if (fseek(file, (long)skip, SEEK_SET)) return 0;
    const size_t allocated = pad_pages ? m0_rom_page_bytes(bytes) : bytes;
    uint8_t *buffer = malloc(allocated);
    if (!buffer) return 0;
    if (pad_pages) memset(buffer, 0xff, allocated);
    if (fread(buffer, 1, bytes, file) != bytes) {
        free(buffer);
        return 0;
    }
    *size = bytes;
    return buffer;
}

static inline void m0_le32(FILE *f, uint32_t v) { for (unsigned i = 0; i < 4; i++) fputc((v >> (8 * i)) & 255, f); }
static inline void m0_wav_header(FILE *f, uint32_t n) {
    fwrite("RIFF", 1, 4, f); m0_le32(f, 36 + n); fwrite("WAVEfmt ", 1, 8, f); m0_le32(f, 16);
    fputc(1, f); fputc(0, f); fputc(1, f); fputc(0, f); m0_le32(f, 22050); m0_le32(f, 22050);
    fputc(1, f); fputc(0, f); fputc(8, f); fputc(0, f); fwrite("data", 1, 4, f); m0_le32(f, n);
}
/* Close a WAV capture: pad the data chunk to an even size, patch the two
 * sizes at the start of the file.  Returns 0 when the file came out whole. */
static inline int m0_wav_finish(FILE *f, uint32_t samples) {
    int error = 0;
    if (samples & 1) fputc(0, f); /* RIFF chunks are padded to an even size */
    if (fseek(f, 0, SEEK_SET)) error = 1;
    m0_wav_header(f, samples);
    if (samples & 1) { fseek(f, 4, SEEK_SET); m0_le32(f, 36 + samples + 1); }
    if (ferror(f)) error = 1;
    if (fclose(f)) error = 1;
    return error;
}

/* --input "frame:mask:frames,...": buttons held from frame for frames */
static inline unsigned m0_scripted(const char *script, unsigned frame) {
    unsigned keys = 0;
    for (const char *p = script; p && *p;) {
        unsigned at = 0, mask = 0, len = 0;
        if (sscanf(p, "%u:%u:%u", &at, &mask, &len) == 3 && frame >= at && frame < at + len) keys |= mask;
        p = strchr(p, ',');
        if (p) p++;
    }
    return keys;
}

/* --random seed: buttons redrawn every 16 frames; extra_mask may add the
 * console's START, which the SG-1000 pad has not. */
static unsigned m0_random_keys;
static inline unsigned m0_randomized(uint32_t *state, unsigned frame, unsigned extra_mask) {
    if (!*state) return 0;
    if (frame % 16 == 0) {
        *state = *state * 1103515245u + 12345u;
        m0_random_keys = (*state >> 16) & 63;
        if (((*state >> 24) & 15) == 0) m0_random_keys |= extra_mask;
    }
    return m0_random_keys;
}

/* FNV-1a: run_hash() folds CPU, memory, devices and the picture into one
 * value the interpreter and the recompiled build must agree on. */
#define M0_RUN_HASH_INIT 2166136261u
static inline uint32_t m0_hash_bytes(uint32_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
static inline uint32_t m0_hash_u16s(uint32_t h, const uint16_t *registers, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        uint8_t bytes[] = { (uint8_t)registers[i], (uint8_t)(registers[i] >> 8) };
        h = m0_hash_bytes(h, bytes, 2);
    }
    return h;
}

/* Coverage dump (v2 files): "address count" lines, hottest first then
 * address order, so the recompiler ranks heat across sessions instead of
 * file order.  locations is the address space, width the console's address
 * width in hex digits, order scratch of `locations` entries.  Returns 0, or
 * -1 when writing failed. */
static const uint32_t *m0_coverage_hits;
static inline int m0_hot_first(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    if (m0_coverage_hits[x] != m0_coverage_hits[y]) return m0_coverage_hits[x] > m0_coverage_hits[y] ? -1 : 1;
    return x < y ? -1 : x > y;
}
static inline int m0_coverage_dump(FILE *f, const uint32_t *hits, uint32_t *order, uint32_t locations, unsigned width) {
    uint32_t n = 0;
    for (uint32_t loc = 0; loc < locations; loc++) if (hits[loc]) order[n++] = loc;
    m0_coverage_hits = hits;
    qsort(order, n, sizeof *order, m0_hot_first);
    for (uint32_t i = 0; i < n; i++)
        if (fprintf(f, "%0*x %u\n", (int)width, order[i], hits[order[i]]) < 0) return -1;
    return 0;
}

#endif
