/*
 * Save states.
 *
 * On the consoles the file GBL_STATE_NAME (made by compile.sh / the Node
 * converter, never resized here) holds GBL_STATE_SLOTS slots of
 * GBL_STATE_SLOT_SECTORS sectors: a header, then the machine as the
 * runtime's state_io streams it.  The header names the format (generator
 * version and state size), the game, the size and a checksum of the data;
 * it is written last, and a load first reads the whole slot to check the
 * checksum, so a state cut short by a power loss is refused instead of
 * half applied.
 *
 * The PC has no card: the same slot area is a plain file in the working
 * directory (STATE_NAME), created zero-filled on the first save.  The
 * header, the checksum and the layout are the card file's.
 */
#include "m0_state.h"

#include <string.h>

#if __has_include("gbl_config.h")
#include "gbl_config.h" /* GBL_TARGET_PC: the slots are a plain file */
#endif

#if defined(GBL_TARGET_PC) && GBL_TARGET_PC
#define STATE_FILE_STORE 1
#else
#define STATE_FILE_STORE 0
#endif

#if !STATE_FILE_STORE
#include "m0_fat.h"
#include "m0_sd.h"
#endif

/* Sega projects name their state file in sg_config.h, PICO-8 ones in
 * p8_config.h; without it this file built its empty stubs on the consoles
 * and the menu never showed Etat (the host harness, which includes
 * sg_config.h itself, had states). */
#if __has_include("sg_config.h")
#include "sg_config.h"
#endif
#if __has_include("p8_config.h")
#include "p8_config.h"
#endif
#if __has_include("nes_config.h")
#include "nes_config.h"
#endif
#if __has_include("vx_config.h")
#include "vx_config.h"
#endif

/* The machine behind the slots: the Game Boy's runtime (gbl_state_io), or
 * the Sega one (gg_state_io, the machine as raw bytes). */
#if defined(GBL_STATE_NAME83)

extern "C" {
#include "gbl.h"
#include "gbl_config.h"
}

#define STATE_NAME83 GBL_STATE_NAME83
#define STATE_FILE GBL_STATE_NAME
#define STATE_SLOTS GBL_STATE_SLOTS
#define STATE_SLOT_SECTORS GBL_STATE_SLOT_SECTORS
#define STATE_TITLE GBL_TITLE
#define STATE_BYTES() gbl_state_bytes()
#define STATE_IO(fn, ctx, load) gbl_state_io(fn, ctx, load)
#define STATE_FRAME gbl_frame

#elif defined(GG_STATE_NAME83)

#include "gbl_config.h" /* GBL_GEN_VERSION */

/* Game Gear / Master System (gg_state_io) or SG-1000 (sg_state_io) */
#if SG_SYSTEM_GG
#define SEGA_STATE(fn) gg_state_##fn
#else
#define SEGA_STATE(fn) sg_state_##fn
#endif
extern "C" {
uint32_t SEGA_STATE(bytes)(void);
void SEGA_STATE(io)(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
uint32_t SEGA_STATE(frame)(void);
}

#define STATE_NAME83 GG_STATE_NAME83
#define STATE_FILE GG_STATE_NAME
#define STATE_SLOTS GG_STATE_SLOTS
#define STATE_SLOT_SECTORS GG_STATE_SLOT_SECTORS
#ifdef GG_STREAM_NAME
#define STATE_TITLE GG_STREAM_NAME
#else /* the whole ROM in flash */
#define STATE_TITLE GG_STATE_NAME
#endif
#define STATE_BYTES() SEGA_STATE(bytes)()
#define STATE_IO(fn, ctx, load) SEGA_STATE(io)(fn, ctx, load)
#define STATE_FRAME SEGA_STATE(frame)()

#elif defined(P8_STATE_NAME83)

#include "gbl_config.h" /* GBL_GEN_VERSION */
#include "p8main.h"

/* the heap's pointers are absolute: the firmware's code joins the format */
extern "C" uint32_t p8_state_identity(void);
#define STATE_NAME83 P8_STATE_NAME83
#define STATE_FILE P8_STATE_NAME
#define STATE_SLOTS P8_STATE_SLOTS
#define STATE_SLOT_SECTORS P8_STATE_SLOT_SECTORS
#define STATE_TITLE P8_STATE_NAME
#define STATE_BYTES() p8_state_bytes()
#define STATE_IO(fn, ctx, load) p8_state_io(fn, ctx, load)
#define STATE_FRAME p8_state_frame()
#define STATE_IDENTITY() p8_state_identity()

#elif defined(NES_STATE_NAME83)

#include "gbl_config.h" /* GBL_GEN_VERSION */
#include "nes.h"

#define STATE_NAME83 NES_STATE_NAME83
#define STATE_FILE NES_STATE_NAME
#define STATE_SLOTS NES_STATE_SLOTS
#define STATE_SLOT_SECTORS NES_STATE_SLOT_SECTORS
#define STATE_TITLE NES_STATE_NAME
#define STATE_BYTES() nes_state_bytes()
#define STATE_IO(fn, ctx, load) nes_state_io(fn, ctx, load)
#define STATE_FRAME nes_state_frame()
#define STATE_IDENTITY() nes_state_identity()

#elif defined(VX_STATE_NAME83)

#include "gbl_config.h"
extern "C" {
uint32_t vx_state_bytes(void);
void vx_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
uint32_t vx_state_frame(void);
uint32_t vx_state_identity(void);
}

#define STATE_NAME83 VX_STATE_NAME83
#define STATE_FILE VX_STATE_NAME
#define STATE_SLOTS VX_STATE_SLOTS
#define STATE_SLOT_SECTORS VX_STATE_SLOT_SECTORS
#define STATE_TITLE VX_STATE_NAME
#define STATE_BYTES() vx_state_bytes()
#define STATE_IO(fn, ctx, load) vx_state_io(fn, ctx, load)
#define STATE_FRAME vx_state_frame()
#define STATE_IDENTITY() vx_state_identity()

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

#if STATE_FILE_STORE

#include <stdio.h>

/* The PC's slot area: one file in the working directory, slots back to
 * back.  It is created zero-filled the first time a save needs it; reads
 * never create it (a firmware that only loads leaves none behind). */
static FILE *state_file;

static int file_open(int create) {
    if (state_file) return 1;
    state_file = fopen(STATE_FILE, "r+b");
    if (state_file) return 1;
    if (!create) return 0;
    static const uint8_t zero[512] = {0};
    state_file = fopen(STATE_FILE, "wb");
    if (state_file)
        for (uint32_t s = 0; s < (uint32_t)STATE_SLOTS * STATE_SLOT_SECTORS; s++)
            if (fwrite(zero, 1, sizeof zero, state_file) != sizeof zero) {
                fclose(state_file);
                state_file = NULL;
                break;
            }
    if (!state_file) return 0;
    if (fflush(state_file) || fclose(state_file)) {
        state_file = NULL;
        return 0;
    }
    state_file = fopen(STATE_FILE, "r+b");
    return state_file != NULL;
}

static int file_at(uint32_t lba) { return fseek(state_file, (long)lba * 512, SEEK_SET) == 0; }

static int read_sector(uint32_t lba, uint8_t *buf) {
    return file_open(0) && file_at(lba) && fread(buf, 1, 512, state_file) == 512;
}

static uint32_t slot_lba(unsigned slot, uint32_t sector) {
    return (uint32_t)slot * STATE_SLOT_SECTORS + sector;
}

static int sd_read(uint32_t lba, uint8_t *buf) { return read_sector(lba, buf); }

static int sd_write(uint32_t lba, const uint8_t *buf) {
    if (!file_open(1) || !file_at(lba) || fwrite(buf, 1, 512, state_file) != 512) return 0;
    return fflush(state_file) == 0;
}

#else /* the consoles: the state file on the card, through the FAT */

static meta_extent extents[MAX_EXTENTS];
static int n_extents;

static int read_sector(uint32_t lba, uint8_t *buf) { return m0_sd_read(lba, buf); }

static uint32_t slot_lba(unsigned slot, uint32_t sector) {
    return m0_fat_lba(extents, n_extents, slot * STATE_SLOT_SECTORS + sector);
}

static int sd_read(uint32_t lba, uint8_t *buf) {
    for (int k = 0; lba && k < 3; k++)
        if (m0_sd_read(lba, buf)) return 1;
    return 0;
}

static int sd_write(uint32_t lba, const uint8_t *buf) {
    for (int k = 0; lba && k < 3; k++)
        if (m0_sd_write(lba, buf)) return 1;
    return 0;
}

#endif /* STATE_FILE_STORE */

static uint8_t have_file;
static uint16_t used;

static uint32_t fnv(uint32_t h, const uint8_t *p, uint32_t n) {
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static uint32_t format_tag(void) {
    const char *v = GBL_GEN_VERSION;
    uint32_t h = fnv(2166136261u, (const uint8_t *)v, (uint32_t)strlen(v));
    uint32_t n = STATE_BYTES();
    h = fnv(h, (const uint8_t *)&n, sizeof(n));
#ifdef STATE_IDENTITY
    static uint32_t id; /* computed once: a hash of the firmware's code */
    if (!id) id = STATE_IDENTITY() | 1u;
    h = fnv(h, (const uint8_t *)&id, sizeof(id));
#endif
    return h;
}

static int header_ok(const state_header *h) {
    return !memcmp(h->magic, "GBLSTATE", 8) && h->format == format_tag() &&
           h->bytes == STATE_BYTES() && !strncmp(h->title, STATE_TITLE, sizeof(h->title));
}

void m0_state_init(int card_ok, uint8_t *buf) {
    have_file = 0;
    used = 0;
    if (STATE_BYTES() > (STATE_SLOT_SECTORS - 1u) * 512u) return;
#if STATE_FILE_STORE
    (void)card_ok;
    have_file = 1;
#else
    uint32_t size = 0;
    if (!card_ok || m0_fat_find(read_sector, buf, STATE_NAME83, extents, MAX_EXTENTS, &n_extents,
                                &size) != M0_FAT_OK ||
        size < (uint32_t)STATE_SLOTS * STATE_SLOT_SECTORS * 512u)
        return;
    have_file = 1;
#endif
    for (unsigned s = 0; s < STATE_SLOTS; s++)
        if (read_sector(slot_lba(s, 0), buf) && header_ok((const state_header *)buf)) used |= 1u << s;
}

int m0_state_available(void) { return have_file; }
uint16_t m0_state_used(void) { return used; }

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

int m0_state_save(unsigned slot, uint8_t *buf) {
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

/* state_io in "check" mode must not touch the machine: it streams into a
 * sink.  The structure of the stream is the save order, so a dry pass is a
 * save-mode walk whose bytes come from the card instead. */
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

int m0_state_load(unsigned slot, uint8_t *buf) {
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

void m0_state_init(int card_ok, uint8_t *buf) {
    (void)card_ok;
    (void)buf;
}
int m0_state_available(void) { return 0; }
uint16_t m0_state_used(void) { return 0; }
int m0_state_save(unsigned slot, uint8_t *buf) {
    (void)slot;
    (void)buf;
    return 0;
}
int m0_state_load(unsigned slot, uint8_t *buf) {
    (void)slot;
    (void)buf;
    return 0;
}

#endif
