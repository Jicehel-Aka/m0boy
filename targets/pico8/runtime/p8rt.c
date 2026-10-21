/* PICO-8 runtime core: heap and collector, strings, tables, closures,
 * generic operations.  See p8rt.h. */
#include "p8rt.h"
#include <stddef.h>
#include <string.h>
#ifdef P8_HEAP_DEBUG
#include <stdio.h>
#endif

Val p8_stack[P8_STACK_VALS];
Val *p8_top = p8_stack;
UpV *p8_open_upvals;
#ifdef P8_STACK_STATS
Val *p8_stack_high = p8_stack;
#endif
uint32_t p8_gc_count;
#define STACK_END (p8_stack + P8_STACK_VALS)

/* ------------------------------------------------------------------ heap */
/* One array of units (a pointer's size), two regions that grow towards each
 * other:
 *   - the buffers (table arrays, hash nodes, the text of strings) from the
 *     bottom, by bumping a pointer; each is owned by exactly one object, so
 *     the collector slides
 *     them down over the dead ones after every collection and fragmentation
 *     cannot happen;
 *   - everything the program holds pointers to (tables, closures, upvalues,
 *     strings) from the top, first fit in the holes the collector leaves: all
 *     of them are one size (IMM_BYTES), so the holes are reused as they are.
 * Every block starts with a header (one unit) giving its size, so the
 * sweeper can walk the regions. */
enum { K_FREE = 0, K_STR, K_TAB, K_CLO, K_UPV, K_VALS, K_NODES, K_CHARS };
#define F_MARK 1u
#define F_SCAN 2u

/* units: bits 0-13, kind: 14-16, flags: 17-18, owner (buffers: the unit of the
 * owning table's payload): 19-31 */
typedef struct Hdr { uintptr_t w; } Hdr;
#define UNIT (sizeof(uintptr_t))
#define HEAP_UNITS (P8_HEAP_BYTES / UNIT)
#define H_UNITS(h) ((uint32_t)((h)->w & 0x3fffu))
#define H_KIND(h) ((unsigned)(((h)->w >> 14) & 7u))
#define H_FLAGS(h) ((unsigned)(((h)->w >> 17) & 3u))
#define H_OWNER(h) ((uint32_t)(((h)->w >> 19) & 0x1fffu))
#define H_SET(h, units, kind, flags, owner) \
    ((h)->w = (uintptr_t)(units) | ((uintptr_t)(kind) << 14) | ((uintptr_t)(flags) << 17) | ((uintptr_t)(owner) << 19))
#define H_ADD_FLAG(h, f) ((h)->w |= (uintptr_t)(f) << 17)

#define MAXOF(a, b) ((a) > (b) ? (a) : (b))
#define CLO_BYTES(nup) (offsetof(Clo, up) + (nup) * sizeof(UpV *))
#define IMM_BYTES MAXOF(MAXOF(sizeof(Tab), sizeof(Str)), MAXOF(sizeof(UpV), CLO_BYTES(2)))

static uintptr_t heap[HEAP_UNITS] __attribute__((aligned(8)));
static uint32_t buf_hi, imm_lo; /* buffers: [0, buf_hi); the rest: [imm_lo, HEAP_UNITS) */
/* the free blocks of the second region, highest first (the first word of each
 * holds the unit of the next; 0 ends the list) */
static uint32_t imm_free;

static inline Hdr *hdr_at(uint32_t unit) { return (Hdr *)&heap[unit]; }
static inline Hdr *hdr_of(const void *payload) { return (Hdr *)payload - 1; }
static inline int in_heap(const void *p) { return (const uintptr_t *)p > heap && (const uintptr_t *)p < heap + HEAP_UNITS; }
static inline uint32_t unit_of(const void *payload) { return (uint32_t)((const uintptr_t *)payload - heap); }
#define FREE_NEXT(h) (*(uint32_t *)((h) + 1))

void p8_heap_init(void) {
    memset(heap, 0, sizeof heap);
    buf_hi = 0;
    imm_lo = HEAP_UNITS;
    imm_free = 0;
    p8_top = p8_stack;
    p8_open_upvals = 0;
    p8_gc_count = 0;
}

static void out_of_memory(uint32_t need) {
#ifdef P8_HEAP_DEBUG
    fprintf(stderr, "out of memory: need %u units, gap %u units, buffers %u, others %u\n", need, imm_lo - buf_hi, buf_hi, HEAP_UNITS - imm_lo);
    {
        size_t st[8][2];
        extern void p8_heap_stats(size_t out[8][2]);
        p8_heap_stats(st);
        const char *kinds[] = { "free+gap", "str", "tab", "clo", "upv", "vals", "nodes", "chars" };
        for (int k = 0; k < 8; k++) fprintf(stderr, "  %-8s blocks %4zu bytes %6zu\n", kinds[k], st[k][0], st[k][1]);
        fprintf(stderr, "  region: ");
        for (uint32_t i = imm_lo; i < HEAP_UNITS; i += H_UNITS(hdr_at(i))) {
            const char *kn = ".STCUVNc";
            fprintf(stderr, "%c%u ", kn[H_KIND(hdr_at(i))], H_UNITS(hdr_at(i)));
        }
        fprintf(stderr, "\n");
    }
#else
    (void)need;
#endif
    p8_error("out of memory", "");
}

static inline int is_buffer(unsigned kind) { return kind == K_VALS || kind == K_NODES || kind == K_CHARS; }

/* a buffer goes to the end of the bottom region, or else to the smallest hole
 * of the other one that holds it (the next collection slides it down) */
static void *alloc_buf_try(const void *owner, uint32_t need, int kind) {
    Hdr *h = 0;
    if (buf_hi + need <= imm_lo) {
        h = hdr_at(buf_hi);
        buf_hi += need;
    } else {
        uint32_t best_at = 0, best_units = 0xffffffffu;
        for (uint32_t i = imm_lo; i < HEAP_UNITS; i += H_UNITS(hdr_at(i))) {
            const uint32_t units = H_UNITS(hdr_at(i));
            if (H_KIND(hdr_at(i)) == K_FREE && units >= need && units < best_units) { best_at = i; best_units = units; }
        }
        if (best_units == 0xffffffffu) return 0;
        h = hdr_at(best_at);
        /* the hole leaves the list; what is left of it goes back on, in its place */
        uint32_t *link = &imm_free;
        while (*link != best_at) link = &FREE_NEXT(hdr_at(*link));
        if (best_units >= need + 2) {
            Hdr *rest = hdr_at(best_at + need);
            H_SET(rest, best_units - need, K_FREE, 0, 0);
            FREE_NEXT(rest) = FREE_NEXT(h);
            *link = best_at + need;
        } else {
            need = best_units;
            *link = FREE_NEXT(h);
        }
    }
    H_SET(h, need, kind, 0, unit_of(owner));
    void *payload = h + 1;
    memset(payload, 0, (size_t)(need - 1) * UNIT);
    return payload;
}

static void *alloc_buf(const void *owner, size_t bytes, int kind) {
#ifdef P8_GC_STRESS
    p8_gc();
#endif
    const uint32_t need = (uint32_t)((bytes + UNIT - 1) / UNIT) + 1;
    void *p = alloc_buf_try(owner, need, kind);
    if (!p) {
        p8_gc();
        p = alloc_buf_try(owner, need, kind);
        if (!p) out_of_memory(need);
    }
    return p;
}

/* the first block of the free list that holds `need` units; the object takes
 * the high end of it, the rest stays on the list */
static void *alloc_imm_try(uint32_t need) {
    uint32_t prev = 0;
    for (uint32_t i = imm_free; i; i = FREE_NEXT(hdr_at(i))) {
        Hdr *h = hdr_at(i);
        const uint32_t units = H_UNITS(h);
        if (units >= need) {
            if (units >= need + 2) {
                H_SET(h, units - need, K_FREE, 0, 0);
                h = hdr_at(i + units - need);
                H_SET(h, need, K_FREE, 0, 0);
            } else { /* all of it: off the list */
                const uint32_t next = FREE_NEXT(h);
                if (prev) FREE_NEXT(hdr_at(prev)) = next;
                else imm_free = next;
            }
            return h;
        }
        prev = i;
    }
    if (imm_lo >= buf_hi + need) { /* grow downwards into the gap */
        imm_lo -= need;
        Hdr *h = hdr_at(imm_lo);
        H_SET(h, need, K_FREE, 0, 0);
        return h;
    }
    return 0;
}

static void *alloc_imm(size_t bytes, int kind) {
#ifdef P8_GC_STRESS
    p8_gc();
#endif
    if (bytes < IMM_BYTES) bytes = IMM_BYTES;
    const uint32_t need = (uint32_t)((bytes + UNIT - 1) / UNIT) + 1;
    Hdr *h = alloc_imm_try(need);
    if (!h) {
        p8_gc();
        h = alloc_imm_try(need);
        if (!h) out_of_memory(need);
    }
    H_SET(h, H_UNITS(h), kind, 0, 0);
    void *payload = h + 1;
    memset(payload, 0, (size_t)(H_UNITS(h) - 1) * UNIT);
    return payload;
}

size_t p8_heap_free_bytes(void) {
    size_t n = (size_t)(imm_lo - buf_hi) * UNIT;
    for (uint32_t i = imm_lo; i < HEAP_UNITS; i += H_UNITS(hdr_at(i)))
        if (H_KIND(hdr_at(i)) == K_FREE) n += (size_t)H_UNITS(hdr_at(i)) * UNIT;
    return n;
}

size_t p8_heap_total_bytes(void) { return P8_HEAP_BYTES; }

/* node slots in use and allocated, for tuning */
void p8_node_usage(size_t *used, size_t *total) {
    *used = *total = 0;
    for (uint32_t i = 0; i < HEAP_UNITS; i += H_UNITS(hdr_at(i))) {
        if (i == buf_hi) { i = imm_lo; if (i >= HEAP_UNITS) break; }
        Hdr *h = hdr_at(i);
        if (H_KIND(h) == K_NODES) {
            const Node *nd = (const Node *)(h + 1);
            const size_t n = (size_t)(H_UNITS(h) - 1) * UNIT / sizeof(Node);
            *total += n;
            for (size_t k = 0; k < n; k++) *used += (nd[k].meta & 7u) != P8_NIL;
        }
        if (H_UNITS(h) == 0) break;
    }
}

/* bytes and blocks per kind, for tuning: [kind][0] = blocks, [kind][1] = bytes */
void p8_heap_stats(size_t out[8][2]) {
    memset(out, 0, 8 * 2 * sizeof(size_t));
    for (uint32_t i = 0; i < buf_hi; i += H_UNITS(hdr_at(i))) {
        out[H_KIND(hdr_at(i))][0]++;
        out[H_KIND(hdr_at(i))][1] += (size_t)H_UNITS(hdr_at(i)) * UNIT;
    }
    for (uint32_t i = imm_lo; i < HEAP_UNITS; i += H_UNITS(hdr_at(i))) {
        out[H_KIND(hdr_at(i))][0]++;
        out[H_KIND(hdr_at(i))][1] += (size_t)H_UNITS(hdr_at(i)) * UNIT;
    }
    out[0][0]++;
    out[0][1] += (size_t)(imm_lo - buf_hi) * UNIT; /* the gap */
}

/* ------------------------------------------------------------- collector */
#define GRAY_MAX 32
static Hdr *gray[GRAY_MAX];
static unsigned gray_n;
static int gray_overflow;

static void mark_block(const void *payload) {
    if (!payload || !in_heap(payload)) return;
    Hdr *h = hdr_of(payload);
    if (H_FLAGS(h) & F_MARK) return;
    H_ADD_FLAG(h, F_MARK);
    if (H_KIND(h) == K_STR) { /* its text is a leaf buffer */
        const Str *str = payload;
        if (str->s && in_heap(str->s)) H_ADD_FLAG(hdr_of(str->s), F_MARK);
        return;
    }
    if (H_KIND(h) == K_CHARS) return;
    if (gray_n < GRAY_MAX) gray[gray_n++] = h;
    else gray_overflow = 1;
}

static void mark_vu(unsigned type, VU u) {
    switch (type) {
    case P8_STR: mark_block(u.s); break;
    case P8_TAB: mark_block(u.t); break;
    case P8_FUN: mark_block(u.c); break;
    default: break;
    }
}
static void mark_val(const Val *v) { mark_vu(v->t, v->u); }

static inline unsigned node_vt(const Node *n) { return n->meta & 7u; }
static inline unsigned node_kt(const Node *n) { return (n->meta >> 3) & 7u; }
static inline int node_next(const Node *n) { return (int16_t)(n->meta >> 16); }
static inline Val node_key(const Node *n) { Val v; v.u = n->key; v.t = node_kt(n); return v; }
static inline Val node_val(const Node *n) { Val v; v.u = n->val; v.t = node_vt(n); return v; }
static inline void node_set_val(Node *n, Val v) { n->val = v.u; n->meta = (n->meta & ~7u) | v.t; }
static inline void node_set_key(Node *n, Val k) { n->key = k.u; n->meta = (n->meta & ~(7u << 3)) | (k.t << 3); }
static inline void node_set_next(Node *n, int off) { n->meta = (n->meta & 0xffffu) | ((uint32_t)(uint16_t)off << 16); }

static void scan_block(Hdr *h) {
    H_ADD_FLAG(h, F_SCAN);
    void *payload = h + 1;
    switch (H_KIND(h)) {
    case K_TAB: {
        Tab *t = payload;
        mark_block(t->arr);
        mark_block(t->node);
        break;
    }
    case K_CLO: {
        Clo *c = payload;
        for (uint32_t i = 0, n = H_OWNER(h); i < n; i++) mark_block(c->up[i]);
        break;
    }
    case K_UPV: mark_val(&((UpV *)payload)->closed); break;
    case K_VALS: {
        Val *v = payload;
        for (size_t i = 0, n = (size_t)(H_UNITS(h) - 1) * UNIT / sizeof(Val); i < n; i++) mark_val(&v[i]);
        break;
    }
    case K_NODES: {
        Node *nd = payload;
        for (size_t i = 0, n = (size_t)(H_UNITS(h) - 1) * UNIT / sizeof(Node); i < n; i++) {
            mark_vu(node_kt(&nd[i]), nd[i].key);
            mark_vu(node_vt(&nd[i]), nd[i].val);
        }
        break;
    }
    }
}

static void gray_rescan(uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i += H_UNITS(hdr_at(i))) {
        Hdr *h = hdr_at(i);
        if (H_KIND(h) != K_FREE && H_KIND(h) != K_STR && H_KIND(h) != K_CHARS && (H_FLAGS(h) & F_MARK) && !(H_FLAGS(h) & F_SCAN)) {
            if (gray_n < GRAY_MAX) gray[gray_n++] = h;
            else gray_overflow = 1;
        }
    }
}

static void drain(void) {
    for (;;) {
        while (gray_n) scan_block(gray[--gray_n]);
        if (!gray_overflow) break;
        gray_overflow = 0; /* the stack was full: find what was reached but not scanned */
        gray_rescan(0, buf_hi);
        gray_rescan(imm_lo, HEAP_UNITS);
    }
}

static void mark_root(const Val *v) {
    mark_val(v);
    drain();
}

/* a buffer has just been copied to its new place: its owner points to it, and
 * its flags are cleared for the next collection */
static void relocate(Hdr *moved) {
    void *owner = &heap[H_OWNER(moved)];
    if (H_KIND(moved) == K_VALS) ((Tab *)owner)->arr = (Val *)(moved + 1);
    else if (H_KIND(moved) == K_NODES) ((Tab *)owner)->node = (Node *)(moved + 1);
    else ((Str *)owner)->s = (const char *)(moved + 1);
    H_SET(moved, H_UNITS(moved), H_KIND(moved), 0, H_OWNER(moved));
}

void p8_gc(void) {
    p8_gc_count++;
    gray_n = 0;
    gray_overflow = 0;
    for (const Val *v = p8_stack; v < p8_top; v++) mark_val(v);
    drain();
    p8_gen_mark_roots(mark_root);
    drain();
    /* open upvalues nobody references any more leave the list */
    UpV **link = &p8_open_upvals;
    while (*link) {
        if (!in_heap(*link) || (H_FLAGS(hdr_of(*link)) & F_MARK)) link = &UPV_NEXT(*link);
        else *link = UPV_NEXT(*link);
    }
    /* the buffers of the bottom region: live ones slide down, their owners
     * learn the new place */
    uint32_t dest = 0;
    for (uint32_t i = 0; i < buf_hi;) {
        Hdr *h = hdr_at(i);
        const uint32_t units = H_UNITS(h);
        if (H_FLAGS(h) & F_MARK) {
            if (dest != i) memmove(hdr_at(dest), h, (size_t)units * UNIT);
            relocate(hdr_at(dest));
            dest += units;
        }
        i += units;
    }
    /* the other region: sweep, merging neighbouring free blocks; the buffers
     * that were put in its holes join the others at the end of the bottom
     * one when there is room; then the lowest free blocks go back to the gap */
    Hdr *prev_free = 0;
    for (uint32_t i = imm_lo; i < HEAP_UNITS;) {
        Hdr *h = hdr_at(i);
        const uint32_t units = H_UNITS(h);
        int live = H_KIND(h) != K_FREE && (H_FLAGS(h) & F_MARK);
        if (live && is_buffer(H_KIND(h)) && dest + units <= imm_lo) {
            memcpy(hdr_at(dest), h, (size_t)units * UNIT);
            relocate(hdr_at(dest));
            dest += units;
            live = 0;
        }
        if (live) {
            H_SET(h, units, H_KIND(h), 0, H_OWNER(h));
            prev_free = 0;
        } else {
            H_SET(h, units, K_FREE, 0, 0);
            if (prev_free) H_SET(prev_free, H_UNITS(prev_free) + units, K_FREE, 0, 0);
            else prev_free = h;
        }
        i += units;
    }
    while (imm_lo < HEAP_UNITS && H_KIND(hdr_at(imm_lo)) == K_FREE) imm_lo += H_UNITS(hdr_at(imm_lo));
    buf_hi = dest;
    imm_free = 0; /* lowest first, so that the highest ends up at the head */
    for (uint32_t i = imm_lo; i < HEAP_UNITS; i += H_UNITS(hdr_at(i)))
        if (H_KIND(hdr_at(i)) == K_FREE) { FREE_NEXT(hdr_at(i)) = imm_free; imm_free = i; }
}

/* Allocation can collect: the object being built is made reachable first. */
#define ROOTED(root, expr) ({ Val *save_ = p8_top; if (p8_top < STACK_END) *p8_top++ = (root); __typeof__(expr) r_ = (expr); p8_top = save_; r_; })

/* --------------------------------------------------------------- strings */
uint32_t p8_str_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    while (len--) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* A string is a small object and its text, a buffer of its own.  Nothing may
 * keep a pointer into a string's text across an allocation (the collector
 * moves it): copying from another string is p8_str_sub. */
static Str *str_alloc(size_t len) {
    Str *str = alloc_imm(sizeof(Str), K_STR);
    char *data = ROOTED(V_STR(str), alloc_buf(str, len + 1, K_CHARS));
    str->s = data;
    str->len = (uint32_t)len;
    return str;
}

const Str *p8_str_new(const char *s, size_t len) {
    Str *str = str_alloc(len);
    memcpy((char *)str->s, s, len);
    str->hash = p8_str_hash(s, len);
    return str;
}

const Str *p8_str_sub(const Str *src, size_t offset, size_t len) {
    Str *str = str_alloc(len);
    memcpy((char *)str->s, src->s + offset, len);
    str->hash = p8_str_hash(str->s, len);
    return str;
}

static int str_eq(const Str *a, const Str *b) {
    return a == b || (a->len == b->len && a->hash == b->hash && !memcmp(a->s, b->s, a->len));
}

/* ---------------------------------------------------------------- tables */
Tab *p8_tab_new(unsigned narr, unsigned nhash) {
    Tab *t = alloc_imm(sizeof(Tab), K_TAB);
    if (narr) t->arr = ROOTED(V_TAB(t), alloc_buf(t, (size_t)narr * sizeof(Val), K_VALS));
    if (nhash) {
        t->node = ROOTED(V_TAB(t), alloc_buf(t, (size_t)nhash * sizeof(Node), K_NODES));
        t->hcap = (uint16_t)nhash;
    }
    return t;
}

static uint32_t key_hash(Val k) {
    switch (k.t) {
    case P8_NUM: return (uint32_t)k.u.n * 2654435761u >> 8;
    case P8_STR: return k.u.s->hash;
    case P8_BOOL: return (uint32_t)k.u.b + 1u;
    default: return (uint32_t)((uintptr_t)k.u.t >> 2) * 2654435761u >> 8;
    }
}

static int key_eq(Val a, Val b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
    case P8_NUM: return a.u.n == b.u.n;
    case P8_STR: return str_eq(a.u.s, b.u.s);
    case P8_BOOL: return a.u.b == b.u.b;
    default: return a.u.t == b.u.t;
    }
}

/* any node count, not only powers of two (so a table built with n fields has n
 * nodes): a 16-bit hash scaled to the count, a multiplication and a shift */
static inline uint32_t node_index(uint32_t hash, uint32_t count) { return (((hash ^ (hash >> 16)) & 0xffffu) * count) >> 16; }
static inline Node *main_position(const Tab *t, Val k) { return &t->node[node_index(key_hash(k), t->hcap)]; }

static Node *hash_find(const Tab *t, Val k) {
    if (!t->hcap) return 0;
    Node *n = main_position(t, k);
    for (;;) {
        if (node_kt(n) == k.t && key_eq(node_key(n), k)) return n;
        const int next = node_next(n);
        if (!next) return 0;
        n += next;
    }
}

static inline int int_key(Val k, int32_t *i) {
    if (k.t != P8_NUM || (k.u.n & 0xffff)) return 0;
    *i = k.u.n >> 16;
    return 1;
}

Val p8_tab_geti(const Tab *t, int32_t i) {
    if (i >= 1 && (uint32_t)i <= t->asize) return t->arr[i - 1];
    Node *n = hash_find(t, V_NUM(fix32_from_int(i)));
    return n ? node_val(n) : V_NIL;
}

/* the highest node that never held a key */
static Node *free_position(Tab *t) {
    for (unsigned i = t->hcap; i > 0;)
        if (node_kt(&t->node[--i]) == P8_NIL) return &t->node[i];
    return 0;
}

/* Lua 5.2's luaH_newkey on a table that has a free node (or is empty of
 * keys): the key is not in the table. */
static int newkey(Tab *t, Val k, Val v) {
    if (!t->hcap) return 0;
    Node *mp = main_position(t, k);
    if (node_vt(mp) != P8_NIL) { /* the main position is taken */
        Node *f = free_position(t);
        if (!f) return 0;
        Node *other = main_position(t, node_key(mp));
        if (other != mp) { /* the colliding node is out of its main position: move it to the free node */
            while (other + node_next(other) != mp) other += node_next(other);
            node_set_next(other, (int)(f - other));
            *f = *mp;
            if (node_next(mp)) { node_set_next(f, node_next(f) + (int)(mp - f)); node_set_next(mp, 0); }
            node_set_val(mp, V_NIL);
        } else { /* the new node goes to the free position */
            if (node_next(mp)) node_set_next(f, (int)(mp + node_next(mp) - f));
            else node_set_next(f, 0);
            node_set_next(mp, (int)(f - mp));
            mp = f;
        }
    }
    node_set_key(mp, k);
    node_set_val(mp, v);
    return 1;
}

static void hash_rehash(Tab *t, Val root_t, Val k, Val v) {
    unsigned live = 0;
    for (unsigned i = 0; i < t->hcap; i++) live += node_vt(&t->node[i]) != P8_NIL;
    const uint32_t cap = live + 1 + live / 2; /* some room for the keys a table keeps adding */
    Node *fresh = ROOTED(root_t, alloc_buf(t, (size_t)cap * sizeof(Node), K_NODES));
    Node *old = t->node;
    const unsigned old_cap = t->hcap;
    t->node = fresh;
    t->hcap = (uint16_t)cap;
    for (unsigned i = 0; i < old_cap; i++)
        if (node_vt(&old[i]) != P8_NIL) newkey(t, node_key(&old[i]), node_val(&old[i]));
    newkey(t, k, v);
}

static void hash_set(Tab *t, Val root_t, Val k, Val v) {
    Node *n = hash_find(t, k);
    if (n) { node_set_val(n, v); return; }
    if (v.t == P8_NIL) return;
    if (!newkey(t, k, v)) hash_rehash(t, root_t, k, v);
}

static inline unsigned arr_capacity(const Tab *t) {
    return t->arr ? (unsigned)((H_UNITS(hdr_of(t->arr)) - 1) * UNIT / sizeof(Val)) : 0;
}

static void arr_append(Tab *t, Val root_t, Val v) {
    const unsigned acap = arr_capacity(t);
    if (t->asize == acap) {
        const unsigned cap = acap ? acap * 2u : 4u;
        Val *fresh = ROOTED(root_t, alloc_buf(t, (size_t)cap * sizeof(Val), K_VALS));
        if (t->asize) memcpy(fresh, t->arr, (size_t)t->asize * sizeof(Val));
        t->arr = fresh;
    }
    t->arr[t->asize++] = v;
}

static void set_int(Tab *t, Val root_t, int32_t i, Val v) {
    if (i >= 1 && (uint32_t)i <= t->asize) {
        t->arr[i - 1] = v;
        if (v.t == P8_NIL && (uint32_t)i == t->asize)
            while (t->asize && t->arr[t->asize - 1].t == P8_NIL) t->asize--;
    } else if (i >= 1 && (uint32_t)i == (uint32_t)t->asize + 1u) {
        if (v.t == P8_NIL) { hash_set(t, root_t, V_NUM(fix32_from_int(i)), v); return; }
        arr_append(t, root_t, v);
        if (t->hcap) { /* the next keys may be waiting in the hash part */
            for (;;) {
                Node *n = hash_find(t, V_NUM(fix32_from_int((int32_t)t->asize + 1)));
                if (!n || node_vt(n) == P8_NIL) break;
                Val moved = node_val(n);
                node_set_val(n, V_NIL);
                arr_append(t, root_t, moved);
            }
        }
    } else {
        hash_set(t, root_t, V_NUM(fix32_from_int(i)), v);
    }
}

void p8_tab_seti(Tab *t, int32_t i, Val v) { set_int(t, V_TAB(t), i, v); }
int32_t p8_tab_len(const Tab *t) { return t->asize; }

Val p8_get(Val tv, Val k) {
    if (tv.t != P8_TAB) p8_error("attempt to index a", p8_typename(tv));
    const Tab *t = tv.u.t;
    int32_t i;
    if (int_key(k, &i)) return p8_tab_geti(t, i);
    if (k.t == P8_NIL) return V_NIL;
    Node *n = hash_find(t, k);
    return n ? node_val(n) : V_NIL;
}

Val p8_get_str(Val tv, const Str *k) {
    if (tv.t != P8_TAB) p8_error("attempt to index a", p8_typename(tv));
    const Tab *t = tv.u.t;
    if (!t->hcap) return V_NIL;
    const Node *n = &t->node[node_index(k->hash, t->hcap)];
    for (;;) {
        if (node_kt(n) == P8_STR && str_eq(n->key.s, k)) return node_val(n);
        const int next = node_next(n);
        if (!next) return V_NIL;
        n += next;
    }
}

void p8_set(Val tv, Val k, Val v) {
    if (tv.t != P8_TAB) p8_error("attempt to index a", p8_typename(tv));
    Tab *t = tv.u.t;
    int32_t i;
    if (int_key(k, &i)) { set_int(t, tv, i, v); return; }
    if (k.t == P8_NIL) p8_error("table index is nil", "");
    hash_set(t, tv, k, v);
}

int p8_tab_next(const Tab *t, Val k, Val *key, Val *val) {
    unsigned pos = 0; /* 0..asize-1 array, then nodes */
    if (k.t != P8_NIL) {
        int32_t i;
        if (int_key(k, &i) && i >= 1 && (uint32_t)i <= t->asize) pos = (unsigned)i;
        else {
            Node *n = hash_find(t, k);
            if (!n) return 0;
            pos = t->asize + (unsigned)(n - t->node) + 1u;
        }
    }
    for (; pos < t->asize; pos++)
        if (t->arr[pos].t != P8_NIL) { *key = V_NUM(fix32_from_int((int32_t)pos + 1)); *val = t->arr[pos]; return 1; }
    for (unsigned h = pos - t->asize; h < t->hcap; h++)
        if (node_vt(&t->node[h]) != P8_NIL) { *key = node_key(&t->node[h]); *val = node_val(&t->node[h]); return 1; }
    return 0;
}

/* -------------------------------------------------------------- closures */
Clo *p8_clo_new(P8Fn fn, uint32_t nup) {
    Clo *c = alloc_imm(CLO_BYTES(nup), K_CLO);
    hdr_of(c)->w |= (uintptr_t)nup << 19; /* the header keeps the count */
    c->fn = fn;
    return c;
}

UpV *p8_upval_find(Val *slot) {
    UpV **link = &p8_open_upvals;
    while (*link && (*link)->v >= slot) {
        if ((*link)->v == slot) return *link;
        link = &UPV_NEXT(*link);
    }
    UpV *u = alloc_imm(sizeof(UpV), K_UPV); /* may collect, and drop dead entries of the list */
    link = &p8_open_upvals;
    while (*link && (*link)->v > slot) link = &UPV_NEXT(*link);
    u->v = slot;
    UPV_NEXT(u) = *link;
    *link = u;
    return u;
}

void p8_upval_close(Val *level) {
    while (p8_open_upvals && p8_open_upvals->v >= level) {
        UpV *u = p8_open_upvals;
        p8_open_upvals = UPV_NEXT(u);
        u->closed = *u->v;
        u->v = &u->closed;
    }
}

/* ------------------------------------------------------------ operations */
const char *p8_typename(Val v) {
    static const char *const names[] = { "nil value", "boolean value", "number value", "string value", "table value", "function value", "function value" };
    return names[v.t];
}

int p8_call(Val *fn, int nargs) {
    int n;
    switch (fn->t) {
    case P8_FUN: n = fn->u.c->fn(fn + 1, nargs, (Clo *)fn->u.c); break;
    case P8_CFN: n = fn->u.f(fn + 1, nargs, 0); break;
    default: p8_error("attempt to call a", p8_typename(*fn));
    }
    for (int i = 0; i < n; i++) fn[i] = fn[i + 1];
    return n;
}

static int hex_digit(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* decimal or 0x hexadecimal text to a number; 0 when it is not one */
static int str_to_fix(const char *s, size_t len, fix32_t *out) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    int neg = 0;
    if (i < len && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    if (i >= len) return 0;
    uint32_t ip = 0, frac = 0;
    unsigned digits = 0;
    if (i + 1 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        for (; i < len && hex_digit(s[i]) >= 0; i++, digits++) ip = (ip << 4) | (uint32_t)hex_digit(s[i]);
        uint32_t scale = 0x1000;
        if (i < len && s[i] == '.') {
            for (i++; i < len && hex_digit(s[i]) >= 0; i++, digits++, scale >>= 4) frac |= (uint32_t)hex_digit(s[i]) * scale;
        }
        if (!digits) return 0;
        *out = (fix32_t)((ip << 16) | (frac & 0xffff));
    } else {
        for (; i < len && s[i] >= '0' && s[i] <= '9'; i++, digits++) ip = ip * 10 + (uint32_t)(s[i] - '0');
        if (i < len && s[i] == '.') {
            uint64_t num = 0, den = 1;
            for (i++; i < len && s[i] >= '0' && s[i] <= '9' && den < 1000000000ull; i++, digits++, den *= 10) num = num * 10 + (uint64_t)(s[i] - '0');
            while (i < len && s[i] >= '0' && s[i] <= '9') i++;
            frac = (uint32_t)((num << 16) / den);
        }
        if (!digits) return 0;
        *out = (fix32_t)((ip << 16) + frac);
    }
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != len) return 0;
    if (neg) *out = fix32_neg(*out);
    return 1;
}

fix32_t p8_tonumber_slow(Val v, int *ok) {
    fix32_t n = 0;
    *ok = 0;
    if (v.t == P8_NUM) { *ok = 1; return v.u.n; }
    if (v.t == P8_STR && str_to_fix(v.u.s->s, v.u.s->len, &n)) *ok = 1;
    return n;
}

Val p8_arith(int op, Val a, Val b) {
    int ok_a, ok_b;
    fix32_t x = p8_tonumber_slow(a, &ok_a), y = p8_tonumber_slow(b, &ok_b);
    if (!ok_a || !ok_b) p8_error("attempt to perform arithmetic on a", p8_typename(ok_a ? b : a));
    switch (op) {
    case P8_OP_ADD: return V_NUM(fix32_add(x, y));
    case P8_OP_SUB: return V_NUM(fix32_sub(x, y));
    case P8_OP_MUL: return V_NUM(fix32_mul(x, y));
    case P8_OP_DIV: return V_NUM(fix32_div(x, y));
    case P8_OP_MOD: return V_NUM(fix32_mod(x, y));
#ifndef P8_NO_POW
    case P8_OP_POW: return V_NUM(fix32_pow(x, y));
#endif
    case P8_OP_IDIV: return V_NUM(fix32_idiv(x, y));
    case P8_OP_BAND: return V_NUM(fix32_band(x, y));
    case P8_OP_BOR: return V_NUM(fix32_bor(x, y));
    case P8_OP_BXOR: return V_NUM(fix32_bxor(x, y));
    case P8_OP_SHL: return V_NUM(fix32_shl(x, fix32_to_int(y)));
    case P8_OP_SHR: return V_NUM(fix32_shr(x, fix32_to_int(y)));
    case P8_OP_LSHR: return V_NUM(fix32_lshr(x, fix32_to_int(y)));
    case P8_OP_ROTL: return V_NUM(fix32_rotl(x, fix32_to_int(y)));
    case P8_OP_ROTR: return V_NUM(fix32_rotr(x, fix32_to_int(y)));
    }
    p8_error("unsupported operator", "");
}

Val p8_unm(Val v) {
    int ok;
    fix32_t x = p8_tonumber_slow(v, &ok);
    if (!ok) p8_error("attempt to perform arithmetic on a", p8_typename(v));
    return V_NUM(fix32_neg(x));
}

int p8_eq(Val a, Val b) { return key_eq(a, b); }

static int str_cmp(const Str *a, const Str *b) {
    const uint32_t n = a->len < b->len ? a->len : b->len;
    const int c = memcmp(a->s, b->s, n);
    return c ? c : (a->len < b->len ? -1 : a->len > b->len);
}

int p8_lt(Val a, Val b) {
    if (a.t == P8_NUM && b.t == P8_NUM) return a.u.n < b.u.n;
    if (a.t == P8_STR && b.t == P8_STR) return str_cmp(a.u.s, b.u.s) < 0;
    p8_error("attempt to compare", a.t == b.t ? p8_typename(a) : "two different types");
}

int p8_le(Val a, Val b) {
    if (a.t == P8_NUM && b.t == P8_NUM) return a.u.n <= b.u.n;
    if (a.t == P8_STR && b.t == P8_STR) return str_cmp(a.u.s, b.u.s) <= 0;
    p8_error("attempt to compare", a.t == b.t ? p8_typename(a) : "two different types");
}

Val p8_len(Val v) {
    if (v.t == P8_STR) return V_NUM(fix32_from_int((int32_t)v.u.s->len));
    if (v.t == P8_TAB) return V_NUM(fix32_from_int(p8_tab_len(v.u.t)));
    p8_error("attempt to get length of a", p8_typename(v));
}

int p8_fix_to_str(char *out, fix32_t n) {
    char *p = out;
    uint32_t mag = n < 0 ? (uint32_t)0 - (uint32_t)n : (uint32_t)n;
    if (n < 0) *p++ = '-';
    uint32_t ip = mag >> 16;
    const uint32_t scaled = (mag & 0xffff) * 10000u, r = scaled & 0xffff;
    uint32_t q = scaled >> 16;
    if (r > 0x8000 || (r == 0x8000 && (q & 1))) q++; /* printf rounds an exact tie to even */
    if (q == 10000) { q = 0; ip++; }
    char digits[12];
    int nd = 0;
    do { digits[nd++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
    while (nd) *p++ = digits[--nd];
    if (q) {
        *p++ = '.';
        char frac[4] = { (char)('0' + q / 1000), (char)('0' + q / 100 % 10), (char)('0' + q / 10 % 10), (char)('0' + q % 10) };
        int len = 4;
        while (frac[len - 1] == '0') len--;
        for (int i = 0; i < len; i++) *p++ = frac[i];
    }
    *p = 0;
    return (int)(p - out);
}

Val p8_concat(const Val *first, int count) {
    char numbuf[12];
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        if (first[i].t == P8_STR) total += first[i].u.s->len;
        else if (first[i].t == P8_NUM) total += (size_t)p8_fix_to_str(numbuf, first[i].u.n);
        else p8_error("attempt to concatenate a", p8_typename(first[i]));
    }
    Str *str = str_alloc(total);
    char *data = (char *)str->s, *p = data; /* the text exists now: no allocation from here */
    for (int i = 0; i < count; i++) {
        if (first[i].t == P8_STR) { memcpy(p, first[i].u.s->s, first[i].u.s->len); p += first[i].u.s->len; }
        else p += p8_fix_to_str(p, first[i].u.n);
    }
    *p = 0;
    str->hash = p8_str_hash(data, total);
    return V_STR(str);
}

fix32_t p8_arg_num_slow(const Val *base, int nargs, int i, fix32_t def) {
    if (i >= nargs) return def;
    int ok;
    fix32_t n = p8_tonumber_slow(base[i], &ok);
    return ok ? n : 0;
}


void p8_forprep(Val *ra) {
    int ok;
    for (int i = 0; i < 3; i++) {
        const fix32_t n = p8_tonumber_slow(ra[i], &ok);
        if (!ok) p8_error(i == 0 ? "'for' initial value must be a number" : i == 1 ? "'for' limit must be a number" : "'for' step must be a number", "");
        ra[i] = V_NUM(n);
    }
    ra[0] = V_NUM(fix32_sub(ra[0].u.n, ra[2].u.n));
}

/* ------------------------------------------- the generated code's interface */
const Val p8_k_nil = { { 0 }, P8_NIL };
const Val p8_k_true = { { .b = 1 }, P8_BOOL };
const Val p8_k_false = { { .b = 0 }, P8_BOOL };

void p8_get_p(Val *d, const Val *t, const Val *k) { *d = p8_get(*t, *k); }
void p8_gets_p(Val *d, const Val *t, const Str *k) { *d = p8_get_str(*t, k); }
void p8_set_p(const Val *t, const Val *k, const Val *v) { p8_set(*t, *k, *v); }
void p8_arith_p(int op, Val *d, const Val *a, const Val *b) { *d = p8_arith(op, *a, *b); }
void p8_unm_p(Val *d, const Val *v) { *d = p8_unm(*v); }
void p8_len_p(Val *d, const Val *v) { *d = p8_len(*v); }
void p8_concat_p(Val *d, const Val *first, int count) { *d = p8_concat(first, count); }
void p8_newtab_p(Val *d, unsigned narr, unsigned nhash) { *d = V_TAB(p8_tab_new(narr, nhash)); }
int p8_eq_p(const Val *a, const Val *b) { return p8_eq(*a, *b); }
int p8_lt_p(const Val *a, const Val *b) { return p8_lt(*a, *b); }
int p8_le_p(const Val *a, const Val *b) { return p8_le(*a, *b); }

/* Between two frames: the heap and its indexes (no frame on the value
 * stack, the gray list empty). */
void p8_rt_state_io(p8_state_fn io, void *ctx) {
    io(heap, sizeof heap, ctx);
    io(&buf_hi, sizeof buf_hi, ctx);
    io(&imm_lo, sizeof imm_lo, ctx);
    io(&imm_free, sizeof imm_free, ctx);
    io(&p8_gc_count, sizeof p8_gc_count, ctx);
    io(&p8_open_upvals, sizeof p8_open_upvals, ctx);
}
