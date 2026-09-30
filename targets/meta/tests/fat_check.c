/*
 * fat_check IMAGE NAME83 EXPECTED_FILE [MAX_EXTENTS]
 *
 * Looks NAME83 up in the FAT image with meta_fat.c, reads the whole file
 * through the extent list and compares it with EXPECTED_FILE.  Prints the
 * number of extents; exits 0 on a match.  With EXPECTED_FILE "-" it prints
 * the lookup's return code instead (for the expected-failure cases).
 */
#include "meta_fat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *img;

static int rd(uint32_t lba, uint8_t *buf) {
    if (fseek(img, (long)lba * 512L, SEEK_SET)) return 0;
    return fread(buf, 1, 512, img) == 512;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: fat_check IMAGE NAME83 EXPECTED [MAX_EXT]\n"); return 2; }
    img = fopen(argv[1], "rb");
    if (!img) { perror(argv[1]); return 2; }
    char name[12];
    memset(name, ' ', 11);
    memcpy(name, argv[2], strlen(argv[2]) < 11 ? strlen(argv[2]) : 11);
    int max_ext = argc > 4 ? atoi(argv[4]) : 16;
    meta_extent ext[64];
    uint8_t buf[512];
    int n = 0;
    uint32_t size = 0;
    int r = meta_fat_find(rd, buf, name, ext, max_ext, &n, &size);
    if (!strcmp(argv[3], "-")) { printf("result %d\n", r); return 0; }
    if (r) { printf("lookup failed: %d\n", r); return 1; }
    FILE *ef = fopen(argv[3], "rb");
    if (!ef) { perror(argv[3]); return 2; }
    fseek(ef, 0, SEEK_END);
    long want = ftell(ef);
    fseek(ef, 0, SEEK_SET);
    if ((long)size != want) { printf("size %u, expected %ld\n", size, want); return 1; }
    for (uint32_t s = 0; s * 512u < size; s++) {
        uint8_t exp[512];
        size_t k = fread(exp, 1, 512, ef);
        uint32_t lba = meta_fat_lba(ext, n, s);
        if (!lba || !rd(lba, buf) || memcmp(buf, exp, k)) { printf("mismatch at sector %u\n", s); return 1; }
    }
    printf("ok extents %d size %u\n", n, size);
    return 0;
}
