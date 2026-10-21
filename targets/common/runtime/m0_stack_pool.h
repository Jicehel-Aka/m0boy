/* The C stack and the card cache's blocks after its fixed part share the
 * RAM between them (Game Boy GBL_CACHE_DYN, Game Gear / Master System
 * GG_CACHE_DYN, m0_cache.h M0C_DYN).  From low to high addresses:
 *
 *   fixed part | cache blocks | (free) | reserve | limit ... stack
 *                              ^ top               ^ *limit
 *
 * Generated code checks the runtime's stack limit on entry (ENTER, the
 * dispatchers' headroom) and unwinds below it; the reserve under the limit
 * takes the unguarded calls and the interrupts.  Moving the limit is all
 * it takes to give the stack more room or less, the cache taking or
 * giving back the blocks under the reserve.
 *
 * Once a frame, from the frontend (thread mode, not inside a cache
 * operation), m0_stack_pool_frame:
 *   - the stack unwound in that frame (*refused, the dispatchers' refusals,
 *     M0SP_UNWINDS or more): at once, the limit goes one block down, on
 *     free RAM, else on the cache's last block, down to the size the
 *     cache had after boot (floor: what the build sized it to; the stack
 *     never takes more, so a game is never slower than with that cache);
 *   - the stack neither unwound nor wrote in the calm zone (a block,
 *     M0SP_NEAR and the dispatchers' headroom above the limit, painted)
 *     for M0SP_CALM_WINDOWS windows of M0SP_WINDOW frames, and the cache
 *     loaded blocks meanwhile: the cache gets one block back, once the
 *     stack is M0SP_MARGIN above that calm zone.  Then the count starts
 *     over: RAM goes back to the cache slowly and only with room to spare,
 *     to the stack at once, so nothing flickers between them.
 * A block back raises the limit: that needs the stack above the calm zone
 * at that moment (the RAM under it is dead, and every running dispatcher
 * loop, above the current frame, stays at least its headroom above the
 * raised limit, so none meets a region refused at entry for ever).  The
 * frame's call can come deep in the game's calls (Super Mario Land 2:
 * always), so the block is taken by m0_stack_pool_run, which the runtime's
 * dispatcher calls on entry once the stack is above *want_sp.
 *
 * The frontend fills the struct and calls m0_stack_pool_boot once, after
 * painting the free RAM with M0SP_PAINT. */
#ifndef M0_STACK_POOL_H
#define M0_STACK_POOL_H

#include <stdint.h>

#ifndef M0SP_PAINT
#define M0SP_PAINT 0xC5AC5AC5u
#endif
#ifndef M0SP_NEAR
#define M0SP_NEAR 128u
#endif
#ifndef M0SP_WINDOW /* frames */
#define M0SP_WINDOW 30u
#endif
#ifndef M0SP_UNWINDS /* dispatcher refusals in a frame: the stack is short */
#define M0SP_UNWINDS 1u
#endif
#ifndef M0SP_CALM_WINDOWS
#define M0SP_CALM_WINDOWS 8u
#endif
#ifndef M0SP_MARGIN /* stack above the calm zone for a block back */
#define M0SP_MARGIN 256u
#endif

typedef struct {
    uintptr_t *limit;          /* the runtime's stack limit */
    uintptr_t *want_sp;        /* the runtime's: call m0_stack_pool_run above it (0: no) */
    uintptr_t ceil;            /* highest limit (the outermost dispatcher's room) */
    uintptr_t floor;           /* the cache's top after boot: never lent under */
    uintptr_t (*top)(void);    /* end of the cache's last block */
    int (*grow)(void);         /* the block at top() into the cache, 0 at the most */
    int (*shrink)(void);       /* the last block out, 0 when none can go */
    const uint32_t *refused;   /* dispatcher refusals so far (unwinds) */
    const uint32_t *loads;     /* blocks the cache loaded so far */
    uint16_t block, reserve;   /* block size; bytes kept under the limit */
    uint16_t headroom;         /* what a dispatcher wants above the limit */
    uint8_t frames, calm;      /* frames in this window; calm windows in a row */
    uint8_t touched;           /* the calm zone was written in this window */
    uint8_t want;              /* a block wanted back by the cache */
    uint32_t refused0, loads0; /* counters at the last frame, the window's start */
    uint32_t lent, taken;      /* blocks given to the stack, taken back */
} m0_stack_pool;

static inline uintptr_t m0sp_sp(void) {
#if defined(__arm__)
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp;
#else
    return (uintptr_t)__builtin_frame_address(0);
#endif
}

/* The current stack pointer (a host test simulates it). */
#ifndef M0SP_SP
#define M0SP_SP() m0sp_sp()
#endif

/* Paint [a, b), word aligned, below the current frame only. */
static inline void m0sp_paint(uintptr_t a, uintptr_t b, uintptr_t sp_safe) {
    if (b > sp_safe) b = sp_safe;
    uint32_t *p = (uint32_t *)((a + 3u) & ~(uintptr_t)3u), *e = (uint32_t *)(b & ~(uintptr_t)3u);
    while (p < e) *p++ = M0SP_PAINT;
}

/* At boot: up to `more` blocks while the stack keeps its room to ceil, and
 * the limit raised to the cache's reserve if it was under it. */
static inline void m0_stack_pool_boot(m0_stack_pool *p, unsigned more) {
    while (more-- && p->top() + p->block + p->reserve <= p->ceil && p->grow()) {}
    if (*p->limit < p->top() + p->reserve) *p->limit = p->top() + p->reserve;
    p->floor = p->top();
    p->frames = p->calm = p->touched = p->want = 0;
    *p->want_sp = 0;
    p->refused0 = *p->refused;
    p->loads0 = *p->loads;
}

/* One block more for the stack: free RAM first, then the cache's last
 * block down to its floor. */
static inline void m0sp_lend(m0_stack_pool *p, uintptr_t sp_safe) {
    uintptr_t lim = *p->limit, lower = lim - p->block;
    if (lower >= p->top() + p->reserve || (p->top() > p->floor && p->shrink())) {
        *p->limit = lower;
        p->lent++;
        m0sp_paint(lower, lim, sp_safe);
    }
}

/* The lowest stack pointer that lets the cache take its next block: the
 * calm zone above the limit it would raise. */
static inline uintptr_t m0sp_take_sp(const m0_stack_pool *p) {
    uintptr_t lim = *p->limit;
    uintptr_t raised = p->top() + p->block + p->reserve > lim ? lim + p->block : lim;
    return raised + M0SP_NEAR + p->headroom + M0SP_MARGIN;
}

/* The block wanted, if the stack (sp_safe, the caller's frame above) is
 * above its calm zone by the margin; else *want_sp tells the runtime when
 * to call again. */
static inline void m0sp_take(m0_stack_pool *p, uintptr_t sp_safe) {
    if (p->want) {
        uintptr_t lim = *p->limit;
        uintptr_t raised = p->top() + p->block + p->reserve > lim ? lim + p->block : lim;
        if (raised > p->ceil) {
            p->want = 0; /* the stack keeps its room */
        } else if (sp_safe >= m0sp_take_sp(p)) {
            *p->limit = raised; /* first: the stack keeps out of the block */
            if (p->grow()) p->taken++;
            else *p->limit = lim;
            p->want = 0;
        }
    }
    *p->want_sp = p->want ? m0sp_take_sp(p) + 64u : 0; /* the runtime's frames to here */
}

/* From the runtime's dispatcher, on entry with the stack above *want_sp. */
static inline void m0_stack_pool_run(m0_stack_pool *p) { m0sp_take(p, M0SP_SP() - 16u); }

static inline void m0_stack_pool_frame(m0_stack_pool *p) {
    uintptr_t sp_safe = M0SP_SP() - 16u; /* this function's frame is above */
    uint32_t refused = *p->refused - p->refused0;
    p->refused0 = *p->refused;
    if (refused >= M0SP_UNWINDS) {
        /* the stack is short: a block at once, and no block back for a while */
        p->want = 0;
        *p->want_sp = 0;
        p->calm = 0;
        p->frames = 0;
        p->loads0 = *p->loads;
        m0sp_lend(p, sp_safe);
        return; /* the stack is near its limit: nothing to paint under it */
    }
    uintptr_t lim = *p->limit;
    uintptr_t calm_end = lim + p->block + M0SP_NEAR + p->headroom;
    uintptr_t end = calm_end < sp_safe ? calm_end : sp_safe;
    const uint32_t *q = (const uint32_t *)((lim + 3u) & ~(uintptr_t)3u);
    while ((uintptr_t)q + 4u <= end && *q == M0SP_PAINT) q++;
    uintptr_t intact = (uintptr_t)q; /* first word written, or end */
    if (end != calm_end || intact + 4u <= end) {
        p->calm = 0; /* the stack came into the calm zone */
        p->want = 0;
        *p->want_sp = 0;
        p->frames = 0;
        p->loads0 = *p->loads;
    } else if (++p->frames >= M0SP_WINDOW) {
        uint32_t loads = *p->loads - p->loads0;
        p->frames = 0;
        p->loads0 = *p->loads;
        if (!loads) p->calm = 0; /* the cache would not use a block */
        else if (++p->calm >= M0SP_CALM_WINDOWS) {
            p->calm = 0;
            p->want = 1;
            m0sp_take(p, sp_safe);
        }
    }
    m0sp_paint(intact, end, sp_safe);
}

#endif
