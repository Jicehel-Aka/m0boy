/* The harness side of m0_explore.h, shared by the host harnesses that run
 * the explorer around a frame loop (sg_host, gg_host, pce_host, nes_host,
 * vectrex_host, md_full_pc.c): the state, the --explore/--random
 * arguments, the frame's keys and the segment loop.  A core wires the
 * explorer by giving the snapshot pair and the M0X_* -> pad map (m0_explore.h
 * keeps those the harness's), a novelty() counting what the runs reached,
 * and a play(count) running frames until stop.
 *
 *   m0xh_t xh;                      one per harness
 *   m0xh_arg(&xh, argc, argv, &i)   in the argument walk
 *   per frame on the explore path:
 *       input = map(m0xh_keys(&xh)); step(); m0xh_seen(&xh, novelty());
 *   m0xh_run(&xh, &machine, novelty, play, again)   plays the segments
 *
 * A discovery stops the frame run at that boundary (the state is kept by
 * the next m0x_observe) and the segment goes on from it; a core whose
 * runtime carries no live discovery count leaves m0xh_seen out and its
 * segments play whole (the SG-1000: its machine struct must stay
 * byte-identical for the console firmware).
 *
 * The Game Boy harness keeps its own wiring: its explorer checkpoints at
 * the runtime's safe points through a longjmp, not around a frame loop. */
#ifndef M0_EXPLORE_HOST_H
#define M0_EXPLORE_HOST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m0_explore.h"

typedef struct {
    m0x_t x;
    int on;    /* the explorer holds the pad: the frames go through m0xh_keys */
    int stop;  /* the run in progress ends at this frame boundary */
    uint32_t seed, total;
} m0xh_t;

/* the --explore FRAMES / --random SEED pair in the harness's argument
 * walk; consumes the argument and its value (i moves past both) */
static int m0xh_arg(m0xh_t *h, int argc, char **argv, int *i) {
    if (!strcmp(argv[*i], "--explore") && *i + 1 < argc) {
        h->total = (uint32_t)strtoul(argv[*i + 1], 0, 0);
        ++*i;
        return 1;
    }
    if (!strcmp(argv[*i], "--random") && *i + 1 < argc) {
        h->seed = (uint32_t)strtoul(argv[*i + 1], 0, 0);
        if (!h->seed) h->seed = 1; /* 0 means off in the scripted draw */
        ++*i;
        return 1;
    }
    return 0;
}

/* the frame's pad: the map's bits, 0 with stop once the segment is spent */
static unsigned m0xh_keys(m0xh_t *h, unsigned (*map)(unsigned)) {
    const unsigned k = m0x_keys(&h->x);
    if (k == M0X_END) {
        h->stop = 1;
        return 0;
    }
    return map(k);
}

/* at the frame boundary: something new was reached — the segment
 * checkpoints here (the next m0x_observe keeps the copy) and the play
 * goes on from it */
static void m0xh_seen(m0xh_t *h, uint32_t novelty) {
    if (novelty > h->x.novelty) h->stop = 1;
}

/* the segments.  play(count) runs frames until h->stop (or its own budget
 * off the explore path); novelty() is the discovery counter, global
 * across restored states; again() answers whether a segment interrupted
 * early may go on (0 on a CPU fault) — left out, an interrupted segment
 * never continues.  0 when the first snapshot ran out of memory: the
 * harness keeps its own run. */
static int m0xh_run(m0xh_t *h, const m0x_machine_t *m, uint32_t (*novelty)(void),
                    void (*play)(unsigned count), int (*again)(void)) {
    if (!m0x_begin(&h->x, m, h->seed, h->total)) return 0;
    h->on = 1;
    for (;;) {
        h->stop = 0;
        play(h->total);
        m0x_observe(&h->x, novelty());
        if (h->x.segment_left && again && again()) continue;
        if (!m0x_next(&h->x, novelty())) break;
    }
    fprintf(stderr, "explore: %u frames, %u segments, %u snapshots, %u locations\n",
            h->x.frames, h->x.segments, h->x.pool_size, novelty());
    return 1;
}

#endif
