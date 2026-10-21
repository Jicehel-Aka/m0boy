/* A discovery must survive a later death, including when the pool is full. */
#include <stdio.h>
#include <string.h>
#define M0X_POOL 3
#define M0X_SEGMENT_MIN 8
#define M0X_SEGMENT_SPAN 1
#include "m0_explore.h"

#define REQUIRE(v) do { if (!(v)) { fprintf(stderr, "line %d: %s\n", __LINE__, #v); return 1; } } while (0)
static unsigned saves;
static void save(void *ctx, void *state) { memcpy(state, ctx, sizeof(unsigned)); saves++; }
static void load(void *ctx, const void *state) { memcpy(ctx, state, sizeof(unsigned)); }
static unsigned value(const m0x_snapshot_t *s) { unsigned v; memcpy(&v, s->state, sizeof v); return v; }

int main(void) {
    unsigned machine = 0;
    const m0x_machine_t m = { &machine, sizeof machine, save, load, 0 };
    m0x_t x;
    REQUIRE(m0x_begin(&x, &m, 3, 1000));
    for (unsigned n = 1; n <= 8; n++) {
        machine = 100 + n;
        m0x_observe(&x, n * 2 - 1);
        machine = n;
        m0x_observe(&x, n * 2);
        const unsigned copied = saves;
        machine = 999; /* death after finding code/data */
        m0x_observe(&x, n * 2);
        REQUIRE(saves == copied);
        while (m0x_keys(&x) != M0X_END) {}
        REQUIRE(m0x_next(&x, n * 2));
        REQUIRE(value(x.pool[0]) == 0); /* boot always survives replacement */
        unsigned found = 0;
        for (unsigned i = 1; i < x.pool_size; i++) found |= value(x.pool[i]) == n;
        REQUIRE(found);
    }
    REQUIRE(x.pool_size == M0X_POOL);
    /* A reused pending buffer is not a discovery in the following segment. */
    const unsigned copied = saves;
    unsigned before[M0X_POOL];
    for (unsigned i = 0; i < x.pool_size; i++) before[i] = value(x.pool[i]);
    machine = 999;
    REQUIRE(m0x_next(&x, 16));
    REQUIRE(saves == copied);
    for (unsigned i = 0; i < x.pool_size; i++) REQUIRE(value(x.pool[i]) == before[i]);
    /* Segment-only callers (SG-1000) still retain new coverage. */
    machine = 42;
    REQUIRE(m0x_next(&x, 17));
    unsigned found = 0;
    for (unsigned i = 1; i < x.pool_size; i++) found |= value(x.pool[i]) == 42;
    REQUIRE(found);
    x.frames = x.total;
    machine = 999;
    REQUIRE(!m0x_next(&x, 17));
    REQUIRE(machine == 999); /* budget exhausted: no further state load */
    for (unsigned i = 0; i < x.pool_size; i++) free(x.pool[i]);
    free(x.pending);
    return 0;
}
