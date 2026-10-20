/*
 * Save states on the SD card.
 *
 * The file GBL_STATE_NAME (made by compile.sh / the Node converter, never
 * resized here) holds GBL_STATE_SLOTS slots of GBL_STATE_SLOT_SECTORS
 * sectors: a header, then the machine as gbl_state_io streams it.  The
 * header names the format (generator version and state size), the
 * game, the size and a checksum of the data; it is written last, and a
 * load first reads the whole slot to check the checksum, so a state cut
 * short by a power loss is refused instead of half applied.
 */
#include "meta_state.h"

#include <string.h>

#include "meta_fat.h"
#include "meta_sd.h"

/* The machine behind the slots: the Game Boy's runtime (gbl_state_io), or
 * the Sega one (gg_state_io, the machine as raw bytes). */
#if defined(GBL_STATE_NAME83)

extern "C" {
#include "gbl.h"
#include "gbl_config.h"
}

#define STATE_NAME83 GBL_STATE_NAME83
#define STATE_SLOTS GBL_STATE_SLOTS
#define STATE_SLOT_SECTORS GBL_STATE_SLOT_SECTORS
#define STATE_TITLE GBL_TITLE
#define STATE_BYTES() gbl_state_bytes()
#define STATE_IO(fn, ctx, load) gbl_state_io(fn, ctx, load)
#define STATE_FRAME gbl_frame

#elif defined(GG_STATE_NAME83)

#include "gbl_config.h" /* GBL_GEN_VERSION */

extern "C" {
uint32_t gg_state_bytes(void);
void gg_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
uint32_t gg_state_frame(void);
}

#define STATE_NAME83 GG_STATE_NAME83
#define STATE_SLOTS GG_STATE_SLOTS
#define STATE_SLOT_SECTORS GG_STATE_SLOT_SECTORS
#define STATE_TITLE GG_STREAM_NAME
#define STATE_BYTES() gg_state_bytes()
#define STATE_IO(fn, ctx, load) gg_state_io(fn, ctx, load)
#define STATE_FRAME gg_state_frame()

#endif

#if defined(STATE_NAME83)

#define MAX_EXTENTS 8

typedef struct {
    char magic[8];       /* "GBLSTATE" */
    uint32_t format;     /* hash of the generator version and the state size */
    uint32_t bytes;
    uint32_t checksum;   /* FNV-1a of the data bytes */
    uint32_t frame;      /* gbl_frame when saved */
    char title[16];
} state_header;

static meta_extent extents[MAX_EXTENTS];
static int n_extents;
static uint8_t have_file;
static uint16_t used;

static int read_sector(uint32_t lba, uint8_t *buf) { return meta_sd_read(lba, buf); }

static uint32_t fnv(uint32_t h, const uint8_t *p, uint32_t n) {
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static uint32_t format_tag(void) {
    const char *v = GBL_GEN_VERSION;
    uint32_t h = fnv(2166136261u, (const uint8_t *)v, (uint32_t)strlen(v));
    uint32_t n = STATE_BYTES();
    return fnv(h, (const uint8_t *)&n, sizeof(n));
}

static uint32_t slot_lba(unsigned slot, uint32_t sector) {
    return meta_fat_lba(extents, n_extents, slot * STATE_SLOT_SECTORS + sector);
}

static int sd_read(uint32_t lba, uint8_t *buf) {
    for (int k = 0; lba && k < 3; k++)
        if (meta_sd_read(lba, buf)) return 1;
    return 0;
}

static int sd_write(uint32_t lba, const uint8_t *buf) {
    for (int k = 0; lba && k < 3; k++)
        if (meta_sd_write(lba, buf)) return 1;
    return 0;
}

static int header_ok(const state_header *h) {
    return !memcmp(h->magic, "GBLSTATE", 8) && h->format == format_tag() &&
           h->bytes == STATE_BYTES() && !strncmp(h->title, STATE_TITLE, sizeof(h->title));
}

void meta_state_init(int card_ok, uint8_t *buf) {
    uint32_t size = 0;
    have_file = 0;
    used = 0;
    if (!card_ok || STATE_BYTES() > (STATE_SLOT_SECTORS - 1u) * 512u) return;
    if (meta_fat_find(read_sector, buf, STATE_NAME83, extents, MAX_EXTENTS, &n_extents, &size) !=
            META_FAT_OK ||
        size < (uint32_t)STATE_SLOTS * STATE_SLOT_SECTORS * 512u)
        return;
    have_file = 1;
    for (unsigned s = 0; s < STATE_SLOTS; s++)
        if (sd_read(slot_lba(s, 0), buf) && header_ok((const state_header *)buf)) used |= 1u << s;
}

int meta_state_available(void) { return have_file; }
uint16_t meta_state_used(void) { return used; }

/* Streams the state sector by sector through buf. */
typedef struct {
    uint8_t *buf;
    unsigned slot;
    uint32_t sector, fill, checksum;
    uint8_t mode; /* 0 save, 1 check (read, checksum only), 2 load */
    uint8_t error;
} stream_ctx;

static void stream_io(void *data, uint32_t bytes, void *ctxp) {
    stream_ctx *c = (stream_ctx *)ctxp;
    uint8_t *p = (uint8_t *)data;
    while (bytes) {
        if (c->mode && c->fill == 0) {
            if (!sd_read(slot_lba(c->slot, c->sector), c->buf)) c->error = 1;
        }
        uint32_t n = 512u - c->fill;
        if (n > bytes) n = bytes;
        if (c->mode == 0) memcpy(c->buf + c->fill, p, n);
        else if (c->mode == 2) memcpy(p, c->buf + c->fill, n);
        c->checksum = fnv(c->checksum, c->mode == 0 ? p : c->buf + c->fill, n);
        c->fill += n;
        p += n;
        bytes -= n;
        if (c->fill == 512u) {
            if (c->mode == 0 && !sd_write(slot_lba(c->slot, c->sector), c->buf)) c->error = 1;
            c->sector++;
            c->fill = 0;
        }
    }
}

int meta_state_save(unsigned slot, uint8_t *buf) {
    if (!have_file || slot >= STATE_SLOTS) return 0;
    stream_ctx c = {buf, slot, 1, 0, 2166136261u, 0, 0};
    STATE_IO(stream_io, &c, 0);
    if (c.fill) {
        memset(buf + c.fill, 0, 512u - c.fill);
        if (!sd_write(slot_lba(slot, c.sector), buf)) c.error = 1;
    }
    if (c.error) return 0;
    state_header *h = (state_header *)buf;
    memset(buf, 0, 512);
    memcpy(h->magic, "GBLSTATE", 8);
    h->format = format_tag();
    h->bytes = STATE_BYTES();
    h->checksum = c.checksum;
    h->frame = STATE_FRAME;
    strncpy(h->title, STATE_TITLE, sizeof(h->title));
    if (!sd_write(slot_lba(slot, 0), buf)) return 0;
    used |= 1u << slot;
    return 1;
}

/* gbl_state_io in "check" mode must not touch the machine: it streams into
 * a sink.  The structure of the stream is the save order, so a dry pass is
 * a save-mode walk whose bytes come from the card instead. */
static void check_io(void *data, uint32_t bytes, void *ctxp) {
    stream_ctx *c = (stream_ctx *)ctxp;
    (void)data;
    while (bytes) {
        if (c->fill == 0 && !sd_read(slot_lba(c->slot, c->sector), c->buf)) c->error = 1;
        uint32_t n = 512u - c->fill;
        if (n > bytes) n = bytes;
        c->checksum = fnv(c->checksum, c->buf + c->fill, n);
        c->fill += n;
        bytes -= n;
        if (c->fill == 512u) {
            c->sector++;
            c->fill = 0;
        }
    }
}

int meta_state_load(unsigned slot, uint8_t *buf) {
    if (!have_file || slot >= STATE_SLOTS || !(used >> slot & 1u)) return 0;
    if (!sd_read(slot_lba(slot, 0), buf)) return 0;
    state_header h;
    memcpy(&h, buf, sizeof(h));
    if (!header_ok(&h)) return 0;
    /* Dry pass: the checksum over exactly the bytes the load will read. */
    stream_ctx c = {buf, slot, 1, 0, 2166136261u, 1, 0};
    uint32_t total = h.bytes;
    while (total) {
        uint32_t n = total > 512u ? 512u : total;
        check_io(0, n, &c);
        total -= n;
    }
    if (c.error || c.checksum != h.checksum) return 0;
    stream_ctx l = {buf, slot, 1, 0, 2166136261u, 2, 0};
    STATE_IO(stream_io, &l, 1);
    return 1;
}

#else

void meta_state_init(int card_ok, uint8_t *buf) {
    (void)card_ok;
    (void)buf;
}
int meta_state_available(void) { return 0; }
uint16_t meta_state_used(void) { return 0; }
int meta_state_save(unsigned slot, uint8_t *buf) {
    (void)slot;
    (void)buf;
    return 0;
}
int meta_state_load(unsigned slot, uint8_t *buf) {
    (void)slot;
    (void)buf;
    return 0;
}

#endif
