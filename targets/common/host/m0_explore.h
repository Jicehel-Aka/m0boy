/* Coverage-guided explorer shared by the host harnesses (gbl_host,
 * sg_host, gg_host: --explore FRAMES, seeded by --random).
 *
 * Play segments of a few seconds with a random but plausible hand (a
 * direction held for a while, buttons held or tapped, START now and then),
 * from the start or from a snapshot of the machine kept because an earlier
 * segment reached code or data never seen; checkpoint discoveries at safe
 * frame boundaries, before later play can lose that progress. The profile
 * is that of all the frames played: logos, menus and every level the player
 * got to, not what a blind player reaches.
 *
 * The console stays with the harness: keys are generic (M0X_*, mapped to
 * the pad by the caller), snapshots are opaque bytes written and read by
 * its save and load functions, and "novelty" is any count that grows when
 * something new is reached (code locations, ROM data sectors...).
 *
 *   m0x_begin(&x, &machine, seed, frames);   keeps the start, loads it
 *   per frame, at a safe point: m0x_observe(&x, novelty)
 *   k = m0x_keys(&x);  M0X_END: stop running, then
 *   m0x_next(&x, novelty)                    0 once every frame is played
 *
 * m0x_begin, m0x_observe and m0x_next call save or load; run them where the
 * machine can be copied (between frames, or at an instruction boundary). */
#ifndef M0_EXPLORE_H
#define M0_EXPLORE_H

#include <stdint.h>
#include <stdlib.h>

enum {
    M0X_RIGHT = 1, M0X_LEFT = 2, M0X_UP = 4, M0X_DOWN = 8,
    M0X_B1 = 16,    /* A on the Game Boy, button 1 elsewhere */
    M0X_B2 = 32,    /* B, button 2 */
    M0X_START = 64,
    M0X_END = 0x100 /* m0x_keys: the segment is over */
};
#define M0X_NO_START 1 /* m0x_machine_t.flags: START pauses (SMS, SG-1000) */
/* Tuning (overridable with -D): snapshots kept, segment length in frames
 * (MIN + random below SPAN), one segment in RESTART from the start.
 * Long segments (8 to 40 s) let the player get through a level or a menu
 * before the next restart: on 17 GB, GG, SMS and SG-1000 games (4 seeds),
 * 480 + 1920 matched or beat 120 + 480 on all but two (Sonic Triple
 * Trouble, Lode Runner: 1 to 2 % less), e.g. Pokemon
 * Yellow 16 000 -> 25 000 locations, Ristar +17 %. */
#ifndef M0X_POOL
#define M0X_POOL 48
#endif
#ifndef M0X_SEGMENT_MIN
#define M0X_SEGMENT_MIN 480
#endif
#ifndef M0X_SEGMENT_SPAN
#define M0X_SEGMENT_SPAN 1920
#endif
#ifndef M0X_RESTART
#define M0X_RESTART 8
#endif

typedef struct {
    void *ctx;
    size_t state_size;
    void (*save)(void *ctx, void *state);
    void (*load)(void *ctx, const void *state);
    unsigned flags;
} m0x_machine_t;

typedef struct {
    uint32_t chosen;
    unsigned char state[];
} m0x_snapshot_t;

typedef struct {
    m0x_machine_t m;
    m0x_snapshot_t *pool[M0X_POOL];
    m0x_snapshot_t *pending; /* latest discovery in this segment */
    unsigned pool_size, segment_left, move_left, move_keys, tap_left, discovered;
    uint32_t rng, frames, total, segments, novelty;
} m0x_t;

static unsigned m0x_random(m0x_t *x) {
    x->rng ^= x->rng << 13; x->rng ^= x->rng >> 17; x->rng ^= x->rng << 5;
    return x->rng;
}

static m0x_snapshot_t *m0x_alloc(const m0x_t *x) { return (m0x_snapshot_t *)malloc(sizeof(m0x_snapshot_t) + x->m.state_size); }

static void m0x_take(m0x_t *x, m0x_snapshot_t *s) {
    x->m.save(x->m.ctx, s->state);
    s->chosen = 0;
}

/* The start now and then, else the snapshot chosen least often. */
static void m0x_segment(m0x_t *x, uint32_t novelty) {
    unsigned pick = 0;
    if (m0x_random(x) % M0X_RESTART) {
        unsigned best = ~0u;
        for (unsigned k = 0, offset = m0x_random(x) % x->pool_size; k < x->pool_size; k++) {
            const unsigned i = (k + offset) % x->pool_size;
            if (x->pool[i]->chosen < best) { best = x->pool[i]->chosen; pick = i; }
        }
    }
    x->pool[pick]->chosen++;
    x->m.load(x->m.ctx, x->pool[pick]->state);
    x->segment_left = M0X_SEGMENT_MIN + m0x_random(x) % M0X_SEGMENT_SPAN;
    x->move_left = 0;
    x->novelty = novelty;
}

/* 0: no memory for the first snapshot. */
static int m0x_begin(m0x_t *x, const m0x_machine_t *m, uint32_t seed, uint32_t total) {
    *x = (m0x_t){ .m = *m, .total = total };
    x->rng = (seed ? seed * 2654435761u : 0x9e3779b9u) | 1u; /* odd and even seeds apart */
    if (!(x->pool[0] = m0x_alloc(x))) return 0;
    m0x_take(x, x->pool[0]);
    x->pool_size = 1;
    m0x_segment(x, 0);
    return 1;
}

static void m0x_move(m0x_t *x) {
    const unsigned r = m0x_random(x) % 100;
    unsigned keys = r < 20 ? 0 : r < 50 ? M0X_RIGHT : r < 65 ? M0X_LEFT : r < 73 ? M0X_UP : r < 81 ? M0X_DOWN :
                    r < 86 ? M0X_RIGHT | M0X_UP : r < 91 ? M0X_RIGHT | M0X_DOWN : r < 96 ? M0X_LEFT | M0X_UP : M0X_LEFT | M0X_DOWN;
    x->move_left = 8 + m0x_random(x) % 56;
    x->tap_left = 0;
    if (m0x_random(x) % 100 < 45) keys |= M0X_B1;
    if (m0x_random(x) % 100 < 25) keys |= M0X_B2;
    if (!(x->m.flags & M0X_NO_START) && m0x_random(x) % 100 < 2) keys |= M0X_START;
    if ((keys & (M0X_B1 | M0X_B2)) && m0x_random(x) % 2) x->tap_left = 4 + m0x_random(x) % 8; /* tapped, not held */
    x->move_keys = keys;
}

/* Once per frame: the keys to hold, or M0X_END. */
static unsigned m0x_keys(m0x_t *x) {
    x->frames++;
    if (!x->segment_left || !--x->segment_left) return M0X_END;
    if (!x->move_left) m0x_move(x);
    x->move_left--;
    const unsigned keys = x->move_keys;
    if (x->move_keys & M0X_START) { if (x->tap_left == 0) x->tap_left = 5; }
    if (x->tap_left) { if (!--x->tap_left) x->move_keys &= ~(unsigned)(M0X_B1 | M0X_B2 | M0X_START); }
    return keys;
}

/* The counter is global across restored states. Only copy on discoveries;
 * one pending state is enough, however many frames discover new locations.
 * Call after the CPU/device event has finished, not inside its callback. */
static void m0x_observe(m0x_t *x, uint32_t novelty) {
    if (novelty <= x->novelty) return;
    if (!x->pending) x->pending = m0x_alloc(x);
    if (!x->pending) return;
    m0x_take(x, x->pending);
    x->novelty = novelty;
    x->discovered = 1;
}

/* After M0X_END: keep the latest discovery (or the end for callers that
 * only observe between segments), then load the start of the next one.
 * 0 once every frame is played. */
static int m0x_next(m0x_t *x, uint32_t novelty) {
    x->segments++;
    m0x_observe(x, novelty);
    if (x->discovered) {
        unsigned slot = x->pool_size;
        if (x->pool_size == M0X_POOL) { /* replace the most chosen, never the start */
            slot = 1;
            for (unsigned i = 2; i < x->pool_size; i++) if (x->pool[i]->chosen > x->pool[slot]->chosen) slot = i;
        } else { x->pool_size++; x->pool[slot] = NULL; }
        m0x_snapshot_t *old = x->pool[slot];
        x->pool[slot] = x->pending;
        x->pending = old;
        x->discovered = 0;
    }
    if (x->frames >= x->total) return 0;
    m0x_segment(x, novelty);
    return 1;
}

#endif
