/* Card cache shared by the consoles' runtimes (Game Boy GBL_CACHE_UNIFIED,
 * Game Gear, Master System): one pool of M0C_BLOCK-byte blocks for the ROM
 * sectors that are not mapped directly (on the SD card, or LZSS-coded in
 * flash) and for the cartridge RAM kept in its save file on the card.
 *
 * This file declares the pool and its state; m0_cache.inc, included once
 * the console's hooks are defined, the functions (static: each console
 * exports its own names).  The including file defines first:
 *   M0C_BLOCK, M0C_SHIFT  block size (16, 32, 64, 128, 256 or 512 bytes) and
 *                         its log2
 *   M0C_WAYS              blocks in the pool (2 to 255)
 *   M0C_BUF, M0C_TAG, M0C_ROM_LAST, M0C_MISSES
 *                         global names of the pool, its tags, the way of
 *                         the ROM block read last and the count of ROM
 *                         blocks loaded (an assembly fast path reads the
 *                         first three)
 *   M0C_SAVE              1: cartridge RAM blocks (save file)
 *   M0C_LEND              1: blocks lent to a menu (cache_lend)
 *   M0C_DYN               1: blocks beyond the M0C_WAYS of the array, up to
 *                         M0C_MAX_WAYS, in the RAM that follows it (the
 *                         frontend puts the array last, under the C stack,
 *                         and moves the boundary: cache_grow, cache_shrink)
 *   M0C_BUF_ATTR          optional: attributes of the array (its section)
 *   M0C_LOADS             optional with M0C_DYN: global name of the count
 *                         of blocks loaded (ROM and cartridge RAM)
 *   M0C_DYN_INLINE        M0C_DYN with all M0C_MAX_WAYS blocks in the array
 *                         (host tests)
 * Tags: the ROM block's index; CACHE_ETAG(index in the save file); -1 free;
 * CACHE_LENT lent. */
#ifndef M0_CACHE_H
#define M0_CACHE_H

#include <stdint.h>
#include <string.h>
#include "m0_copy.h"

#if M0C_BLOCK != (1 << M0C_SHIFT) || M0C_BLOCK < 16 || M0C_BLOCK > 512
#error "M0C_BLOCK: 16, 32, 64, 128, 256 or 512, M0C_SHIFT its log2"
#endif
#if M0C_WAYS < 2 || M0C_WAYS > 255
#error "M0C_WAYS: 2 to 255 blocks"
#endif
#ifndef M0C_DYN
#define M0C_DYN 0
#endif
#if !M0C_DYN
#undef M0C_MAX_WAYS
#define M0C_MAX_WAYS M0C_WAYS
#elif !defined(M0C_MAX_WAYS) || M0C_MAX_WAYS < M0C_WAYS || M0C_MAX_WAYS > 255
#error "M0C_MAX_WAYS: M0C_WAYS to 255 blocks"
#endif
#ifndef M0C_BUF_ATTR
#define M0C_BUF_ATTR
#endif

#define cache_buf M0C_BUF
#define cache_tag M0C_TAG
#define cache_rom_last M0C_ROM_LAST
#define CACHE_ETAG(b) ((int32_t)(0x40000000u | (uint32_t)(b)))
#define CACHE_IS_ETAG(t) ((t) >= 0x40000000)
#define CACHE_ETAG_BLK(t) ((uint32_t)(t) & 0x3FFFFFFFu)
#define CACHE_LENT (-2)

#if M0C_DYN && defined(M0C_DYN_INLINE)
uint8_t cache_buf[M0C_MAX_WAYS][M0C_BLOCK] __attribute__((aligned(4))) M0C_BUF_ATTR;
#else
uint8_t cache_buf[M0C_WAYS][M0C_BLOCK] __attribute__((aligned(4))) M0C_BUF_ATTR;
#endif
int32_t cache_tag[M0C_MAX_WAYS];
uint8_t cache_rom_last; /* way of the ROM block read last, never evicted */
#if M0C_DYN
/* Blocks in use: the array's, then those after it.  Their address hides
 * where it comes from, the compiler would take cache_buf's bounds for
 * theirs. */
static uint8_t cache_ways = M0C_WAYS;
#define CACHE_N cache_ways
static inline uint8_t *cache_blk(unsigned i) {
    uint8_t *p = &cache_buf[0][0];
    __asm__("" : "+r"(p));
    return p + i * M0C_BLOCK;
}
#define CACHE_BLK(i) cache_blk(i)
#else
#define CACHE_N M0C_WAYS
#define CACHE_BLK(i) cache_buf[i]
#endif
/* CLOCK replacement: the hand evicts the first way not referenced since it
 * last passed, never the newest cartridge RAM block (cache_mru) nor the
 * ROM block read last.  A console that shows referenced cartridge RAM
 * blocks elsewhere (the Game Boy's tables) only shows those: clearing a
 * way's referenced byte takes it out, and its next access goes through
 * cache_ram_block, which marks it again. */
static uint8_t cache_ref[M0C_MAX_WAYS], cache_hand;
static uint8_t cache_dirty[M0C_MAX_WAYS], cache_mru;

#endif
