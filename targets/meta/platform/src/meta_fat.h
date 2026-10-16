/*
 * Minimal read-only FAT16/FAT32 lookup for a streamed ROM on the META's SD
 * card.  At boot it finds one file by its 8.3 name, in the root directory or
 * one directory below, and turns its cluster chain into a short list of
 * contiguous sector runs; after that, reading the file needs no FAT access.
 * Portable C with a sector-read callback, so the host tests it on real FAT
 * images.
 */
#ifndef META_FAT_H
#define META_FAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read 512-byte sector lba into buf; return 0 on failure. */
typedef int (*meta_fat_read_fn)(uint32_t lba, uint8_t *buf);

typedef struct {
    uint32_t lba;     /* first sector of the run */
    uint32_t sectors; /* run length */
} meta_extent;

enum {
    META_FAT_OK = 0,
    META_FAT_IO = -1,         /* sector read failed */
    META_FAT_NO_FS = -2,      /* no FAT16/FAT32 volume with 512-byte sectors */
    META_FAT_NOT_FOUND = -3,
    META_FAT_FRAGMENTED = -4, /* more runs than the caller's table */
};

/* Find name83 (11 bytes, space padded, e.g. "MQ      GB "); buf is a 512-byte
 * work buffer.  Fills ext[0..*n_ext) and *size. */
/* Looks name83 up in meta_fat_dir: the game's folder once the frontend
 * has found it (meta_fat_game_dir), else the root and the folders one
 * level below. */
int meta_fat_find(meta_fat_read_fn rd, uint8_t *buf, const char *name83, meta_extent *ext,
                  int max_ext, int *n_ext, uint32_t *size);
extern uint32_t meta_fat_dir;

/* Several game folders hold a file of the same name (ROM.GG, ROM.SAV...):
 * accept, when not NULL, is called on each candidate with the sector of its
 * first cluster and its size; it may read into buf (the lookup reads its
 * directory sector again) and returns 1 to take the file, 0 to look
 * further.  *dir: META_FAT_ANY_DIR searches the root and the folders one
 * level below; any other value searches that directory only.  On success it
 * receives the directory the file was found in. */
#define META_FAT_ANY_DIR 0xFFFFFFFFu
typedef int (*meta_fat_accept_fn)(uint32_t first_lba, uint32_t size, uint8_t *buf, void *ctx);
int meta_fat_find_in(meta_fat_read_fn rd, uint8_t *buf, const char *name83, meta_fat_accept_fn accept,
                     void *ctx, uint32_t *dir, meta_extent *ext, int max_ext, int *n_ext, uint32_t *size);

/* Card sector holding file sector `sector` (0 if beyond the runs). */
uint32_t meta_fat_lba(const meta_extent *ext, int n_ext, uint32_t sector);

/* The running game's folder: the directory holding bin83 whose first sector
 * is the firmware's own (app, its first 512 bytes in flash; the loader
 * copies the .BIN there).  META_FAT_ANY_DIR when there is none (a firmware
 * flashed over USB that differs from the card's). */
uint32_t meta_fat_game_dir(meta_fat_read_fn rd, uint8_t *buf, const char *bin83, const uint8_t *app);

#ifdef __cplusplus
}
#endif

#endif
