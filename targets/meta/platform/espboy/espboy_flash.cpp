/* ESPboy flash: ROM sectors beyond the mapped megabyte, battery RAM and
 * save states, all in the 4 MiB chip (map in gbl_espboy.h).
 *
 * Flash is erased by 4 KiB blocks and wears out, so:
 *  - battery RAM is written as a whole new copy, alternating between two
 *    copies; a copy counts only with its header (sequence number, checksum),
 *    written last, so a power cut leaves the previous copy intact.  The
 *    frontend writes after 2 s without change, when the game disables its
 *    RAM, and on leaving; never periodically.
 *  - a state slot is erased block by block as it is written and its header
 *    sector comes last, with the checksum the load checks first.
 * Nothing here masks interrupts: the audio interrupt runs from IRAM. */
#include <Arduino.h>
#include <string.h>
extern "C" {
#include "gbl.h"
#include "gbl_config.h"
}
#include "gbl_espboy.h"
#include "meta_save.h"
#include "meta_state.h"

uint32_t meta_save_reads, meta_save_writes, meta_save_errors;

static uint32_t sbuf[128]; /* 512 aligned bytes of staging */

static int flash_read(uint32_t off, void *dst, uint32_t n) {
    return ESP.flashRead(off, (uint32_t *)dst, n) ? 1 : 0;
}
static int flash_erase(uint32_t block_off) { return ESP.flashEraseSector(block_off / ESPBOY_BLOCK) ? 1 : 0; }
/* n and dst multiples of 4 and aligned; the target must be erased. */
static int flash_program(uint32_t off, const void *src, uint32_t n) {
    return ESP.flashWrite(off, (const uint32_t *)src, n) ? 1 : 0;
}
static int program_sector(uint32_t off, const uint8_t *src) {
    memcpy(sbuf, src, 512);
    return flash_program(off, sbuf, 512);
}

/* ROM bytes at `offset` of rom.bin (sectors 32 on): any alignment, up to 520 bytes. */
extern "C" int espboy_rom_read(uint32_t offset, uint8_t *dst, uint32_t bytes) {
    static uint32_t tmp[(520 + 8) / 4];
    uint32_t addr = ESPBOY_ROM_OFF + offset, a0 = addr & ~3u, end = (addr + bytes + 3u) & ~3u;
    if (end - a0 > sizeof tmp) return 0;
    if (!flash_read(a0, tmp, end - a0)) return 0;
    memcpy(dst, (uint8_t *)tmp + (addr - a0), bytes);
    return 1;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, uint32_t n) {
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* ---- battery RAM ---------------------------------------------------------- */
#if GBL_ERAM_SIZE > 0 && GBL_BATTERY

#define DATA_BLOCKS ((GBL_ERAM_SIZE + ESPBOY_BLOCK - 1u) / ESPBOY_BLOCK)
#define COPY_BLOCKS (DATA_BLOCKS + 1u) /* the header block is last */
#define COPY_OFF(c) (ESPBOY_SAVE_OFF + (c) * COPY_BLOCKS * ESPBOY_BLOCK)

typedef struct {
    char magic[8]; /* "GBLRAM01" */
    uint32_t game, seq, bytes, checksum;
} ram_header;

static int8_t current = -1; /* copy the RAM is read from, -1 none */
static uint32_t current_seq;

static int copy_valid(unsigned c, uint8_t *scratch, uint32_t *seq) {
    ram_header h;
    if (!flash_read(COPY_OFF(c) + DATA_BLOCKS * ESPBOY_BLOCK, sbuf, 512)) return 0;
    memcpy(&h, sbuf, sizeof h);
    if (memcmp(h.magic, "GBLRAM01", 8) || h.game != GBL_FLASH_GAME_ID || h.bytes != GBL_ERAM_SIZE) return 0;
    uint32_t sum = 2166136261u;
    for (uint32_t off = 0; off < GBL_ERAM_SIZE; off += 512) {
        uint32_t n = GBL_ERAM_SIZE - off < 512 ? GBL_ERAM_SIZE - off : 512;
        if (!flash_read(COPY_OFF(c) + off, sbuf, (n + 3u) & ~3u)) return 0;
        sum = fnv(sum, (const uint8_t *)sbuf, n);
    }
    (void)scratch;
    if (sum != h.checksum) return 0;
    *seq = h.seq;
    return 1;
}

int meta_save_init(int card_ok) {
    (void)card_ok;
    current = -1;
    uint32_t seq[2] = {0, 0};
    int ok[2] = {copy_valid(0, 0, &seq[0]), copy_valid(1, 0, &seq[1])};
    if (ok[0] && (!ok[1] || (int32_t)(seq[0] - seq[1]) > 0)) current = 0;
    else if (ok[1]) current = 1;
    if (current >= 0) current_seq = seq[current];
    return current >= 0 ? META_SAVE_OK : META_SAVE_NO_FILE;
}

extern "C" int gbl_platform_save_read(uint32_t sector, uint8_t *dst) {
    if (current < 0 || sector * 512u >= GBL_ERAM_SIZE) return 0;
    uint32_t n = GBL_ERAM_SIZE - sector * 512u < 512 ? GBL_ERAM_SIZE - sector * 512u : 512;
    if (!flash_read(COPY_OFF(current) + sector * 512u, sbuf, (n + 3u) & ~3u)) {
        meta_save_errors++;
        return 0;
    }
    memcpy(dst, sbuf, n);
    meta_save_reads++;
    return 1;
}

extern "C" int gbl_platform_save_image(const uint8_t *eram, uint32_t bytes) {
    if (bytes != GBL_ERAM_SIZE) return 0;
    unsigned target = current == 0 ? 1 : 0; /* the copy not in use */
    uint32_t base = COPY_OFF(target), sum = fnv(2166136261u, eram, bytes);
    int ok = 1;
    /* Header block first: the copy stops counting while it is rewritten. */
    for (unsigned b = DATA_BLOCKS; ok && b != 0xffffffffu; b--) ok = flash_erase(base + b * ESPBOY_BLOCK);
    for (uint32_t off = 0; ok && off < bytes; off += 512) {
        uint32_t n = bytes - off < 512 ? bytes - off : 512;
        memset(sbuf, 0xff, 512);
        memcpy(sbuf, eram + off, n);
        ok = flash_program(base + off, sbuf, 512);
    }
    if (ok) {
        ram_header h;
        memset(&h, 0, sizeof h);
        memcpy(h.magic, "GBLRAM01", 8);
        h.game = GBL_FLASH_GAME_ID;
        h.seq = current_seq + 1;
        h.bytes = bytes;
        h.checksum = sum;
        memset(sbuf, 0xff, 512);
        memcpy(sbuf, &h, sizeof h);
        ok = flash_program(base + DATA_BLOCKS * ESPBOY_BLOCK, sbuf, 512);
    }
    if (!ok) {
        meta_save_errors++;
        return 0;
    }
    current = (int8_t)target;
    current_seq++;
    meta_save_writes++;
    return 1;
}

#define STATE_OFF (COPY_OFF(2))

void meta_save_tick(void) {
    if (gbl_save_pending() && gbl_frame - gbl_save_last_change >= 120) gbl_save_flush();
}

#else

int meta_save_init(int card_ok) {
    (void)card_ok;
    return META_SAVE_NO_FILE;
}
extern "C" int gbl_platform_save_read(uint32_t, uint8_t *) { return 0; }
extern "C" int gbl_platform_save_write(uint32_t, const uint8_t *) { return 0; }
void meta_save_tick(void) {}
#define STATE_OFF ESPBOY_SAVE_OFF

#endif

/* ---- save states ----------------------------------------------------------- */
#if defined(GBL_STATE_SLOTS)

#define SLOT_BLOCKS ((GBL_STATE_SLOT_SECTORS + 7u) / 8u)
#define SLOT_OFF(s) (STATE_OFF + (s) * SLOT_BLOCKS * ESPBOY_BLOCK)

typedef struct {
    char magic[8]; /* "GBLSTATE" */
    uint32_t format, bytes, checksum, frame, game;
    char title[16];
} state_header;

static uint16_t used;
static uint8_t have_states;

static uint32_t format_tag(void) {
    const char *v = GBL_GEN_VERSION;
    uint32_t h = fnv(2166136261u, (const uint8_t *)v, (uint32_t)strlen(v));
    uint32_t n = gbl_state_bytes();
    return fnv(h, (const uint8_t *)&n, sizeof(n));
}
static int header_ok(const state_header *h) {
    return !memcmp(h->magic, "GBLSTATE", 8) && h->format == format_tag() && h->bytes == gbl_state_bytes() &&
           h->game == GBL_FLASH_GAME_ID && !strncmp(h->title, GBL_TITLE, sizeof(h->title));
}
static int read_sector(unsigned slot, uint32_t sector, uint8_t *buf) {
    if (!flash_read(SLOT_OFF(slot) + sector * 512u, sbuf, 512)) return 0;
    memcpy(buf, sbuf, 512);
    return 1;
}

void meta_state_init(int card_ok, uint8_t *buf) {
    (void)card_ok;
    used = 0;
    have_states = SLOT_OFF(GBL_STATE_SLOTS) <= ESPBOY_SAVE_OFF + ESPBOY_SAVE_BYTES &&
                  gbl_state_bytes() <= (GBL_STATE_SLOT_SECTORS - 1u) * 512u;
    if (!have_states) return;
    for (unsigned s = 0; s < GBL_STATE_SLOTS; s++)
        if (read_sector(s, 0, buf) && header_ok((const state_header *)buf)) used |= 1u << s;
}
int meta_state_available(void) { return have_states; }
uint16_t meta_state_used(void) { return used; }

typedef struct {
    uint8_t *buf;
    unsigned slot;
    uint32_t sector, fill, checksum;
    int32_t erased; /* last block erased while saving */
    uint8_t mode;   /* 0 save, 1 load */
    uint8_t error;
} stream_ctx;

/* Sector 0 (the header) comes last, so data starts at sector 1. */
static void stream_io(void *data, uint32_t bytes, void *ctxp) {
    stream_ctx *c = (stream_ctx *)ctxp;
    uint8_t *p = (uint8_t *)data;
    while (bytes) {
        if (c->mode && c->fill == 0 && !read_sector(c->slot, c->sector, c->buf)) c->error = 1;
        uint32_t n = 512u - c->fill;
        if (n > bytes) n = bytes;
        if (c->mode == 0) memcpy(c->buf + c->fill, p, n);
        else memcpy(p, c->buf + c->fill, n);
        c->checksum = fnv(c->checksum, c->mode == 0 ? p : c->buf + c->fill, n);
        c->fill += n;
        p += n;
        bytes -= n;
        if (c->fill == 512u) {
            if (c->mode == 0) {
                int32_t block = (int32_t)(c->sector / 8u);
                if (c->erased != block) {
                    if (!flash_erase(SLOT_OFF(c->slot) + (uint32_t)block * ESPBOY_BLOCK)) c->error = 1;
                    c->erased = block;
                }
                if (!c->error && !program_sector(SLOT_OFF(c->slot) + c->sector * 512u, c->buf)) c->error = 1;
            }
            c->sector++;
            c->fill = 0;
        }
    }
}

int meta_state_save(unsigned slot, uint8_t *buf) {
    if (!have_states || slot >= GBL_STATE_SLOTS) return 0;
    used &= ~(1u << slot);
    stream_ctx c = {buf, slot, 1, 0, 2166136261u, -1, 0, 0};
    gbl_state_io(stream_io, &c, 0);
    if (c.fill && !c.error) {
        memset(buf + c.fill, 0, 512u - c.fill);
        int32_t block = (int32_t)(c.sector / 8u);
        if (c.erased != block && !flash_erase(SLOT_OFF(slot) + (uint32_t)block * ESPBOY_BLOCK)) c.error = 1;
        if (!c.error && !program_sector(SLOT_OFF(slot) + c.sector * 512u, buf)) c.error = 1;
    }
    if (c.error) {
        meta_save_errors++;
        return 0;
    }
    state_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "GBLSTATE", 8);
    h.format = format_tag();
    h.bytes = gbl_state_bytes();
    h.checksum = c.checksum;
    h.frame = gbl_frame;
    h.game = GBL_FLASH_GAME_ID;
    strncpy(h.title, GBL_TITLE, sizeof(h.title));
    memset(buf, 0xff, 512);
    memcpy(buf, &h, sizeof h);
    if (!program_sector(SLOT_OFF(slot), buf)) {
        meta_save_errors++;
        return 0;
    }
    used |= 1u << slot;
    return 1;
}

int meta_state_load(unsigned slot, uint8_t *buf) {
    if (!have_states || slot >= GBL_STATE_SLOTS || !(used >> slot & 1u)) return 0;
    state_header h;
    if (!read_sector(slot, 0, buf)) return 0;
    memcpy(&h, buf, sizeof h);
    if (!header_ok(&h)) return 0;
    /* The checksum over exactly the bytes the load reads, before applying any. */
    uint32_t sum = 2166136261u, left = h.bytes;
    for (uint32_t sector = 1; left; sector++) {
        uint32_t n = left > 512u ? 512u : left;
        if (!read_sector(slot, sector, buf)) return 0;
        sum = fnv(sum, buf, n);
        left -= n;
    }
    if (sum != h.checksum) return 0;
    stream_ctx c = {buf, slot, 1, 0, 2166136261u, -1, 1, 0};
    gbl_state_io(stream_io, &c, 1);
    return !c.error;
}

#else

void meta_state_init(int, uint8_t *) {}
int meta_state_available(void) { return 0; }
uint16_t meta_state_used(void) { return 0; }
int meta_state_save(unsigned, uint8_t *) { return 0; }
int meta_state_load(unsigned, uint8_t *) { return 0; }

#endif
