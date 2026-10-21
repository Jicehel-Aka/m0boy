/* Functions of the card cache (m0_cache.h), included once after it.  The
 * including file defines, besides m0_cache.h's settings:
 *   M0C_ROM_SECTOR(sec, dst)   ROM sector sec into dst, 0 on failure
 *   M0C_SAVE_READ(sec, dst), M0C_SAVE_WRITE(sec, src)   save file sectors
 *                              (M0C_SAVE), 0 on failure
 *   M0C_PENDING                lvalue: changes not yet written back
 *   M0C_NOTE_CHANGE()          a cartridge RAM byte changed (time of it)
 *   M0C_SHOWN()                optional: the referenced or dirty cartridge
 *                              RAM blocks changed (rebuild what shows them)
 *   M0C_HOOK_WRITEBACK(i), M0C_HOOK_LOAD(tag)   optional (debug builds)
 *   M0C_LEND_KEEP              blocks a menu leaves to the cache (M0C_LEND)
 *   M0C_FLUSH                  optional: the console's exported name of
 *                              cache_flush (one function, also called here)
 *   M0C_FLUSH_NEXT             optional: the exported name of cache_flush_next
 *
 * CLOCK over every block: the hand passes the newest cartridge RAM block,
 * the ROM block read last and the blocks lent to a menu; it clears
 * referenced bytes, and evicts the first block not referenced since its
 * last pass, writing it back first when it is dirty cartridge RAM. */
#ifndef M0C_SHOWN
#define M0C_SHOWN() ((void)0)
#endif
#ifndef M0C_HOOK_WRITEBACK
#define M0C_HOOK_WRITEBACK(i) ((void)0)
#endif
#ifndef M0C_HOOK_LOAD
#define M0C_HOOK_LOAD(tag) ((void)0)
#endif
#define M0C_UNUSED __attribute__((unused))

uint32_t M0C_MISSES; /* ROM blocks loaded */
static uint8_t cache_changed; /* a cartridge RAM block left what shows them */

#if M0C_BLOCK < 512
/* One sector of the card or of the packed ROM: what blocks are copied from,
 * and where a dirty block meets the rest of its sector before the write.
 * Tag: the ROM sector, CACHE_ETAG(save sector), or -1. */
/* Whole-sector blocks read straight into the block (cache_load): the
 * staging buffer is only for smaller blocks, and for the save file's
 * write-back, which assembles the sector from its cached blocks. */
#if M0C_BLOCK != 512 || M0C_SAVE
static uint8_t sector_buf[512] __attribute__((aligned(4)));
static int32_t sector_tag = -1;
#endif
#endif

#if M0C_SAVE
static void cache_writeback(unsigned i) {
    if (!CACHE_IS_ETAG(cache_tag[i]) || !cache_dirty[i]) return;
    M0C_HOOK_WRITEBACK(i);
#if M0C_BLOCK == 512
    /* Still dirty when the card refused it: a later flush tries again. */
    if (!M0C_SAVE_WRITE(CACHE_ETAG_BLK(cache_tag[i]), cache_buf[i])) return;
    cache_dirty[i] = 0;
#else
    /* The sector as on the card, then every dirty block of it from the pool:
     * all of them are clean once it is written. */
    uint32_t sec = CACHE_ETAG_BLK(cache_tag[i]) >> (9 - M0C_SHIFT);
    if (sector_tag != CACHE_ETAG(sec)) {
        sector_tag = -1;
        if (!M0C_SAVE_READ(sec, sector_buf)) return;
        sector_tag = CACHE_ETAG(sec);
    }
    for (unsigned j = 0; j < M0C_WAYS; j++)
        if (CACHE_IS_ETAG(cache_tag[j]) && cache_dirty[j] && CACHE_ETAG_BLK(cache_tag[j]) >> (9 - M0C_SHIFT) == sec)
            memcpy(sector_buf + (CACHE_ETAG_BLK(cache_tag[j]) * M0C_BLOCK & 511u), cache_buf[j], M0C_BLOCK);
    if (!M0C_SAVE_WRITE(sec, sector_buf)) {
        sector_tag = -1; /* the card may hold either */
        return;
    }
    for (unsigned j = 0; j < M0C_WAYS; j++)
        if (CACHE_IS_ETAG(cache_tag[j]) && CACHE_ETAG_BLK(cache_tag[j]) >> (9 - M0C_SHIFT) == sec) cache_dirty[j] = 0;
#endif
}
#endif

/* A block for a miss: by the hand, written back if needed, its tag void. */
static unsigned cache_victim(void) {
    unsigned i;
    for (;;) {
        i = cache_hand;
        cache_hand = (uint8_t)(i + 1u == M0C_WAYS ? 0 : i + 1u);
#if M0C_WAYS > 2 /* two blocks: the referenced bytes alone */
        if (i == cache_mru || i == cache_rom_last) continue;
#endif
#if M0C_LEND
        if (cache_tag[i] == CACHE_LENT) continue;
#endif
        if (!cache_ref[i]) break;
        cache_ref[i] = 0;
        if (CACHE_IS_ETAG(cache_tag[i])) cache_changed = 1;
    }
    if (CACHE_IS_ETAG(cache_tag[i])) {
#if M0C_SAVE
        cache_writeback(i);
#endif
        cache_changed = 1;
    }
    cache_tag[i] = -1;
    cache_dirty[i] = 0; /* even if the card refused the write-back */
    cache_ref[i] = 0;
    return i;
}


/* Fill way i with block b of the ROM (rom) or of the save file. */
static void cache_load(unsigned i, uint32_t b, int rom) {
#if !M0C_SAVE
    (void)rom;
#define M0C_SOURCE(sec, dst) M0C_ROM_SECTOR(sec, dst)
#else
#define M0C_SOURCE(sec, dst) (rom ? M0C_ROM_SECTOR(sec, dst) : M0C_SAVE_READ(sec, dst))
#endif
#if M0C_BLOCK == 512
    int ok = M0C_SOURCE(b, cache_buf[i]);
    if (!ok) memset(cache_buf[i], 0xFF, 512);
#else
    uint32_t sec = b >> (9 - M0C_SHIFT);
    int32_t want = rom ? (int32_t)sec : CACHE_ETAG(sec);
    if (sector_tag != want) {
        sector_tag = -1;
        if (M0C_SOURCE(sec, sector_buf)) sector_tag = want;
    }
    if (sector_tag == want) memcpy(cache_buf[i], sector_buf + (b * M0C_BLOCK & 511u), M0C_BLOCK);
    else memset(cache_buf[i], 0xFF, M0C_BLOCK);
#endif
#undef M0C_SOURCE
}

#if M0C_SAVE
/* A cartridge RAM block (off: in the save file) not in the console's fast
 * path: not in the pool, not referenced since the hand passed, or a first
 * write to a clean one.  It becomes the newest and referenced. */
static uint8_t *cache_ram_block(uint32_t off) {
    int32_t tag = CACHE_ETAG(off >> M0C_SHIFT);
    unsigned i = cache_mru;
    if (cache_tag[i] != tag) {
        for (i = 0; i < M0C_WAYS && cache_tag[i] != tag; i++) {}
        if (i == M0C_WAYS) {
            i = cache_victim();
            cache_load(i, off >> M0C_SHIFT, 0);
            cache_tag[i] = tag;
            M0C_HOOK_LOAD(tag);
        }
    }
    if (i != cache_mru || !cache_ref[i] || cache_changed) {
        cache_ref[i] = 1;
        cache_mru = (uint8_t)i;
        cache_changed = 0;
        M0C_SHOWN();
    }
    return cache_buf[i];
}

static uint8_t cache_ram_load(uint32_t off) { return cache_ram_block(off)[off & (M0C_BLOCK - 1u)]; }

static void cache_ram_store(uint32_t off, uint8_t v) {
    uint8_t *p = cache_ram_block(off);
    if (p[off & (M0C_BLOCK - 1u)] == v) return;
    p[off & (M0C_BLOCK - 1u)] = v;
    M0C_PENDING = 1;
    M0C_NOTE_CHANGE();
    if (!cache_dirty[cache_mru]) {
        cache_dirty[cache_mru] = 1;
        M0C_SHOWN(); /* its write slot opens */
    }
}
#endif

/* A ROM byte from the pool (off: in the ROM). */
static M0C_UNUSED uint8_t cache_rom_byte(uint32_t off) {
    int32_t b = (int32_t)(off >> M0C_SHIFT);
    if (cache_tag[cache_rom_last] == b) return cache_buf[cache_rom_last][off & (M0C_BLOCK - 1u)];
#if M0C_BLOCK < 512
    /* the staged sector, still whole from the last miss: the 4×128 accolé
     * served before taking a block from the pool */
    if (sector_tag == (int32_t)(off >> 9)) return sector_buf[off & 511u];
#endif
    unsigned i = 0;
    while (i < M0C_WAYS && cache_tag[i] != b) i++;
    if (i == M0C_WAYS) {
        i = cache_victim();
        cache_load(i, (uint32_t)b, 1);
        cache_tag[i] = b;
        M0C_MISSES++;
        if (cache_changed) {
            cache_changed = 0;
            M0C_SHOWN();
        }
    }
    cache_ref[i] = 1;
    cache_rom_last = (uint8_t)i;
    return cache_buf[i][off & (M0C_BLOCK - 1u)];
}

#if M0C_SAVE
static int cache_any_dirty(void) {
    unsigned dirty = 0;
    for (unsigned w = 0; w < M0C_WAYS; w++) dirty |= cache_dirty[w];
    return dirty != 0;
}

/* Every dirty block written back; those the card refused stay pending. */
#ifdef M0C_FLUSH
#define cache_flush M0C_FLUSH
void cache_flush(void) {
#else
static void cache_flush(void) {
#endif
    for (unsigned i = 0; i < M0C_WAYS; i++) cache_writeback(i);
    M0C_PENDING = (uint8_t)cache_any_dirty();
    M0C_SHOWN(); /* clean again: the write slots close */
}

/* One dirty block from way i on written back: the next way to try, or -1
 * when none was left. */
#ifdef M0C_FLUSH_NEXT
#define cache_flush_next M0C_FLUSH_NEXT
int cache_flush_next(int i) {
#else
static M0C_UNUSED int cache_flush_next(int i) {
#endif
    for (; i >= 0 && i < M0C_WAYS; i++)
        if (CACHE_IS_ETAG(cache_tag[i]) && cache_dirty[i]) {
            cache_writeback(i);
            M0C_SHOWN();
            return i + 1;
        }
    M0C_PENDING = (uint8_t)cache_any_dirty();
    return -1;
}
#endif

/* Every block void (none dirty: flushed first, or a reset); lent ones stay
 * lent. */
static void cache_drop(void) {
    for (unsigned i = 0; i < M0C_WAYS; i++) {
#if M0C_LEND
        if (cache_tag[i] != CACHE_LENT) cache_tag[i] = -1;
#else
        cache_tag[i] = -1;
#endif
        cache_dirty[i] = 0;
        cache_ref[i] = 0;
    }
#if M0C_BLOCK < 512
    sector_tag = -1;
#endif
    cache_mru = cache_rom_last = 0;
#if M0C_LEND
    while (cache_tag[cache_mru] == CACHE_LENT) cache_mru = cache_rom_last = (uint8_t)(cache_mru + 1u);
#endif
    cache_hand = 0;
    cache_changed = 0;
    M0C_SHOWN();
}

static void cache_reset(void) {
    cache_drop();
#if M0C_SAVE
    M0C_PENDING = 0;
#endif
}

#if M0C_LEND
/* A menu's stack: the whole pool but its last M0C_LEND_KEEP blocks (what
 * the hand needs if something reads the ROM meanwhile), written back first
 * and kept out of the cache until cache_unlend.  Returns the top of the
 * lent part, 8-aligned; it starts at cache_buf[0]. */
static uintptr_t cache_lend(void) {
#if M0C_SAVE
    cache_flush();
#endif
    unsigned n = M0C_WAYS - M0C_LEND_KEEP;
    for (unsigned i = 0; i < n; i++) cache_tag[i] = CACHE_LENT;
    cache_drop();
    return ((uintptr_t)cache_buf[0] + n * M0C_BLOCK) & ~(uintptr_t)7;
}

static void cache_unlend(void) {
    for (unsigned i = 0; i < M0C_WAYS; i++)
        if (cache_tag[i] == CACHE_LENT) cache_tag[i] = -1;
}
#endif
