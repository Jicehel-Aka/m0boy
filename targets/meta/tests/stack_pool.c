/* m0_stack_pool.h over a simulated RAM: a cache of 128-byte blocks after a
 * fixed part, then the reserve and the stack's limit.  Each frame the
 * script sets how deep the stack went (writes under that depth, the
 * frame's call there), how many dispatcher refusals and cache loads
 * happened, and how high the stack is when the dispatcher is entered (the
 * runtime's hook).  Checked: the cache's top stays under the limit by the
 * reserve, the limit under the ceiling; a block goes back only with the
 * stack above its calm zone and margin; and the moves:
 *   - shallow stack, a cache that loads: a block back every
 *     M0SP_CALM_WINDOWS windows, not faster;
 *   - unwinds: blocks to the stack at once, one a frame, down to the
 *     cache's size after boot and never under it;
 *   - unwinds every few seconds: no block back in between (no flicker);
 *   - the frame's call always deep: blocks back through the hook only. */
#include <stdint.h>
static uintptr_t sim_sp;
#define M0SP_SP() sim_sp
#include "m0_stack_pool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BLOCK 128u
#define RESERVE 256u
#define HEADROOM 512u
#define FIXED 4u
#define MAX_WAYS 64u

static uint32_t ram_words[8192 / 4];
#define RAM ((uintptr_t)ram_words)
#define RAM_END (RAM + sizeof ram_words)
static unsigned ways = FIXED;
static uintptr_t limit;
static uint32_t refused, loads;
static uintptr_t want_sp;

static uintptr_t top(void) { return RAM + ways * BLOCK; }
static int grow(void) {
    if (ways >= MAX_WAYS) return 0;
    ways++;
    return 1;
}
static int shrink(void) {
    if (ways <= FIXED) return 0;
    ways--;
    return 1;
}

static m0_stack_pool p;

static void check(void) {
    assert(top() + RESERVE <= limit);
    assert(limit <= p.ceil);
    assert(ways >= FIXED && ways <= MAX_WAYS);
}

/* n frames: the stack down to `depth`, r refusals and l loads a frame;
 * `high`: the dispatcher entered once a frame with the stack that high. */
static void frames(unsigned n, uintptr_t depth, unsigned r, unsigned l, uintptr_t high) {
    for (unsigned f = 0; f < n; f++) {
        if (depth < RAM_END) memset((void *)depth, 0x11, RAM_END - depth);
        refused += r;
        loads += l;
        uintptr_t before = limit;
        sim_sp = depth;
        m0_stack_pool_frame(&p);
        check();
        if (limit > before) assert(depth - 16u >= limit + M0SP_NEAR + HEADROOM + M0SP_MARGIN);
        if (high && want_sp && high > want_sp) {
            before = limit;
            sim_sp = high;
            m0_stack_pool_run(&p);
            check();
            assert(limit > before && high - 16u >= limit + M0SP_NEAR + HEADROOM + M0SP_MARGIN);
        }
    }
}

#define CALM (M0SP_CALM_WINDOWS * M0SP_WINDOW)

int main(void) {
    for (unsigned i = 0; i < sizeof ram_words / 4; i++) ram_words[i] = M0SP_PAINT;
    p.limit = &limit;
    p.want_sp = &want_sp;
    p.ceil = RAM_END - 1024u;
    p.top = top;
    p.grow = grow;
    p.shrink = shrink;
    p.refused = &refused;
    p.loads = &loads;
    p.block = BLOCK;
    p.reserve = RESERVE;
    p.headroom = HEADROOM;
    limit = RAM_END - 3072u; /* the budget the frontend starts with */
    m0_stack_pool_boot(&p, 12);
    check();
    const unsigned boot = FIXED + 12;
    assert(ways == boot);

    /* Shallow stack, a cache that loads: a block every CALM frames. */
    uintptr_t depth = RAM_END - 500u;
    frames(CALM - 1, depth, 0, 1, 0);
    assert(ways == boot);
    frames(1, depth, 0, 1, 0);
    assert(ways == boot + 1);
    frames(60 * CALM, depth, 0, 1, 0);
    unsigned calm_ways = ways;
    assert(calm_ways > boot + 10 && calm_ways < MAX_WAYS);
    printf("shallow: %u blocks, limit %u below the frames\n", ways, (unsigned)(depth - limit));

    /* An idle cache (no loads) takes nothing. */
    frames(4 * CALM, depth, 0, 0, 0);
    assert(ways == calm_ways);

    /* Unwinds: a block a frame at once, down to the boot size, not under. */
    frames(1, limit + 40u, 2, 0, 0);
    assert(ways == calm_ways - 1);
    frames(3, limit + 40u, 2, 0, 0);
    assert(ways == calm_ways - 4);
    frames(200, limit + 40u, 2, 3, 0);
    assert(ways == boot);
    uintptr_t floor_limit = limit;
    frames(50, limit + 40u, 2, 3, 0);
    assert(ways == boot && limit == floor_limit && limit == top() + RESERVE); /* the floor: nothing more */
    printf("unwinds: %u blocks at the floor\n", ways);

    /* Unwinds every 3 s, a shallow stack between: nothing goes back. */
    unsigned before = ways;
    uintptr_t lim0 = limit;
    for (unsigned k = 0; k < 20; k++) {
        frames(180, RAM_END - 500u, 0, 3, RAM_END - 200u);
        frames(1, limit + 40u, 1, 3, 0);
    }
    assert(ways == before && limit <= lim0);

    /* The frame's call always deep (no refusal, but in the calm zone):
     * nothing back.  Deep but out of the calm zone, the dispatcher high:
     * blocks back through the hook. */
    frames(4 * CALM, limit + 60u, 0, 3, RAM_END - 200u);
    assert(ways == before);
    uint32_t taken = p.taken;
    for (unsigned k = 0; k < 4; k++) /* just above the calm zone, under its margin */
        frames(CALM + 1, limit + BLOCK + M0SP_NEAR + HEADROOM + 40u, 0, 3, RAM_END - 200u);
    assert(p.taken == taken + 4 && ways == before + 4);
    printf("back: %u blocks, %u lent, %u taken\n", ways, (unsigned)p.lent, (unsigned)p.taken);
    return 0;
}
