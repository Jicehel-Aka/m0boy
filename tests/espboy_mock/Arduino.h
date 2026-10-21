/* Just enough of the Arduino core for espboy_flash.cpp on the host: a 4 MiB
 * flash which is strict about what the real chip needs (4-byte alignment,
 * erased bytes before programming). */
#ifndef MOCK_ARDUINO_H
#define MOCK_ARDUINO_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

struct MockEsp {
    uint8_t flash[4 * 1024 * 1024];
    long fail_after = -1; /* program/erase calls left before every one fails (power cut) */
    unsigned erases = 0, programs = 0;
    bool step() {
        if (fail_after == 0) return false;
        if (fail_after > 0) fail_after--;
        return true;
    }
    bool flashRead(uint32_t a, uint32_t *d, size_t n) {
        if ((a & 3) || (n & 3) || ((uintptr_t)d & 3) || a + n > sizeof flash) abort();
        memcpy(d, flash + a, n);
        return true;
    }
    bool flashEraseSector(uint32_t sector) {
        if (!step()) return false;
        erases++;
        memset(flash + sector * 4096, 0xff, 4096);
        return true;
    }
    bool flashWrite(uint32_t a, const uint32_t *d, size_t n) {
        if ((a & 3) || (n & 3) || ((uintptr_t)d & 3) || a + n > sizeof flash) abort();
        if (!step()) return false;
        programs++;
        for (size_t i = 0; i < n; i++)
            if (flash[a + i] != 0xff) abort(); /* programming over data that was not erased */
        memcpy(flash + a, d, n);
        return true;
    }
};
extern MockEsp ESP;
#endif
