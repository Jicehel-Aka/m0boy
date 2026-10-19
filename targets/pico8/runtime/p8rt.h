/* PICO-8 runtime for recompiled cartridges: the values, tables, closures and
 * garbage-collected heap the generated C code (p8_gen.c) runs on, in plain C
 * for the Cortex-M0+ and the host harness.
 *
 * The generated code is the cartridge's Lua bytecode (femto8's Lua 5.2 with
 * 16.16 fixed-point numbers) written out as C: one function per Lua function,
 * registers in an explicit value stack, the VM's own calling convention. */
#ifndef P8RT_H
#define P8RT_H

#include <stddef.h>
#include <stdint.h>
#include "fix32.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { P8_NIL = 0, P8_BOOL, P8_NUM, P8_STR, P8_TAB, P8_FUN, P8_CFN };

struct Clo;
struct Str;
struct Tab;
typedef struct Val Val;
/* args at base[0..nargs-1]; results go to base[0..], the count is returned */
typedef int (*P8Fn)(Val *base, int nargs, struct Clo *cl);

typedef union VU {
    fix32_t n;
    int b;
    const struct Str *s;
    struct Tab *t;
    const struct Clo *c;
    P8Fn f;
    struct UpV *uv; /* the link of the open list, in an open upvalue */
} VU;

struct Val {
    VU u;
    uint32_t t;
};

typedef struct Str {
    const char *s;
    uint32_t hash;
    uint32_t len;
} Str;

/* An upvalue: the stack slot while open, its own copy of the value afterwards.
 * While it is open the copy is unused and the link of the open list (highest
 * slot first) lives there. */
typedef struct UpV {
    Val *v;
    Val closed;
} UpV;
#define UPV_NEXT(x) ((x)->closed.u.uv)

/* The number of upvalues of a heap closure is in its block header; a static
 * closure has none. */
typedef struct Clo {
    P8Fn fn;
    UpV *up[1];
} Clo;

/* Hash node (Lua 5.2's chained scatter table, no load factor): the two
 * payloads, then the types of key and value and the offset of the next node
 * of the chain (in nodes) in one word: 12 bytes on the console. */
typedef struct Node {
    VU key, val;
    uint32_t meta; /* value type: bits 0-2, key type: bits 3-5, next: bits 16-31 (signed) */
} Node;

/* 12 bytes on the console, like every object the program holds a pointer to.
 * The capacity of arr is the size of its block. */
typedef struct Tab {
    Val *arr;
    Node *node;
    uint16_t asize, hcap; /* elements in use; number of nodes (0: none) */
} Tab;

#define V_NIL ((Val){{0}, P8_NIL})
#define V_BOOL(x) ((Val){{.b = (x) ? 1 : 0}, P8_BOOL})
#define V_NUM(x) ((Val){{.n = (x)}, P8_NUM})
#define V_STR(p) ((Val){{.s = (p)}, P8_STR})
#define V_TAB(p) ((Val){{.t = (p)}, P8_TAB})
#define V_FUN(p) ((Val){{.c = (p)}, P8_FUN})
#define V_CFN(p) ((Val){{.f = (p)}, P8_CFN})

#define P8_TRUTHY(v) ((v).t > P8_BOOL || ((v).t == P8_BOOL && (v).u.b))

/* ---- configuration (override with -D, or a p8_config.h next to this file) ---- */
#if defined(__has_include)
#if __has_include("p8_config.h")
#include "p8_config.h"
#endif
#if __has_include("p8_gen_config.h")
#include "p8_gen_config.h"
#endif
#endif
#ifndef P8_HEAP_BYTES
#define P8_HEAP_BYTES 12288
#endif
#ifndef P8_STACK_VALS
#define P8_STACK_VALS 72
#endif

/* ---- state ---- */
extern Val p8_stack[P8_STACK_VALS];
extern Val *p8_top;      /* end of the running frame: the garbage collector scans below it */
extern UpV *p8_open_upvals;
#ifdef P8_STACK_STATS /* host harness: deepest frame reached */
extern Val *p8_stack_high;
#define P8_TRACK_STACK(top) do { if ((top) > p8_stack_high) p8_stack_high = (top); } while (0)
#else
#define P8_TRACK_STACK(top) do { } while (0)
#endif
extern uint32_t p8_gc_count;

/* The generated code's own roots (its globals): called by the collector. */
void p8_gen_mark_roots(void (*mark)(const Val *v));

/* Fatal error: shows the message and stops the cartridge; never returns. */
void p8_error(const char *msg, const char *what) __attribute__((noreturn));

/* ---- heap ---- */
void p8_heap_init(void);
void p8_gc(void);
size_t p8_heap_free_bytes(void);
size_t p8_heap_total_bytes(void);

/* ---- strings, tables, closures ---- */
const Str *p8_str_new(const char *s, size_t len);
/* the text of a heap string moves when the collector runs: copy from it with this */
const Str *p8_str_sub(const Str *src, size_t offset, size_t len);
uint32_t p8_str_hash(const char *s, size_t len);
Tab *p8_tab_new(unsigned narr, unsigned nhash);
Val p8_get(Val t, Val k);
Val p8_get_str(Val t, const Str *k);
void p8_set(Val t, Val k, Val v);
Val p8_tab_geti(const Tab *t, int32_t i);
void p8_tab_seti(Tab *t, int32_t i, Val v);
int32_t p8_tab_len(const Tab *t);
/* next key after k (nil: first); returns 0 at the end */
int p8_tab_next(const Tab *t, Val k, Val *key, Val *val);
Clo *p8_clo_new(P8Fn fn, uint32_t nup);
UpV *p8_upval_find(Val *slot);
void p8_upval_close(Val *level);

/* ---- operations the generated code calls ---- */
int p8_call(Val *fn, int nargs);
Val p8_arith(int op, Val a, Val b);
int p8_eq(Val a, Val b);
int p8_lt(Val a, Val b);
int p8_le(Val a, Val b);
Val p8_len(Val v);
Val p8_concat(const Val *first, int count);
Val p8_unm(Val v);
const char *p8_typename(Val v);
fix32_t p8_tonumber_slow(Val v, int *ok);

#define P8_OP_ADD 0
#define P8_OP_SUB 1
#define P8_OP_MUL 2
#define P8_OP_DIV 3
#define P8_OP_MOD 4
#define P8_OP_POW 5
#define P8_OP_IDIV 6
#define P8_OP_BAND 7
#define P8_OP_BOR 8
#define P8_OP_BXOR 9
#define P8_OP_SHL 10
#define P8_OP_SHR 11
#define P8_OP_LSHR 12
#define P8_OP_ROTL 13
#define P8_OP_ROTR 14

/* What the generated code calls: pointers to values and to constants, never
 * values passed or returned by value (the compiler gives each of those a stack
 * slot of its own in the large generated functions, and the stack is small). */
extern const Val p8_k_nil, p8_k_true, p8_k_false;
void p8_get_p(Val *d, const Val *t, const Val *k);
void p8_gets_p(Val *d, const Val *t, const Str *k);
void p8_set_p(const Val *t, const Val *k, const Val *v);
void p8_arith_p(int op, Val *d, const Val *a, const Val *b);
void p8_unm_p(Val *d, const Val *v);
void p8_len_p(Val *d, const Val *v);
void p8_concat_p(Val *d, const Val *first, int count);
void p8_newtab_p(Val *d, unsigned narr, unsigned nhash);
int p8_eq_p(const Val *a, const Val *b);
int p8_lt_p(const Val *a, const Val *b);
int p8_le_p(const Val *a, const Val *b);
#define P8_ARITH_INLINE(name, op, fn) \
    static inline void name(Val *d, const Val *a, const Val *b) { \
        if (a->t == P8_NUM && b->t == P8_NUM) { const fix32_t r = fn(a->u.n, b->u.n); d->u.n = r; d->t = P8_NUM; } \
        else p8_arith_p(op, d, a, b); \
    }
P8_ARITH_INLINE(p8_add_p, P8_OP_ADD, fix32_add)
P8_ARITH_INLINE(p8_sub_p, P8_OP_SUB, fix32_sub)
P8_ARITH_INLINE(p8_mul_p, P8_OP_MUL, fix32_mul)
P8_ARITH_INLINE(p8_div_p, P8_OP_DIV, fix32_div)
P8_ARITH_INLINE(p8_mod_p, P8_OP_MOD, fix32_mod)
static inline int p8_eqi_p(const Val *a, const Val *b) { return (a->t == P8_NUM && b->t == P8_NUM) ? a->u.n == b->u.n : p8_eq_p(a, b); }
static inline int p8_lti_p(const Val *a, const Val *b) { return (a->t == P8_NUM && b->t == P8_NUM) ? a->u.n < b->u.n : p8_lt_p(a, b); }
static inline int p8_lei_p(const Val *a, const Val *b) { return (a->t == P8_NUM && b->t == P8_NUM) ? a->u.n <= b->u.n : p8_le_p(a, b); }
/* numeric for: the three control values must be numbers */
void p8_forprep(Val *ra);

/* ---- helpers for natives ---- */
#define P8_ARG(i) ((i) < nargs ? base[i] : V_NIL)
fix32_t p8_arg_num_slow(const Val *base, int nargs, int i, fix32_t def);
static inline fix32_t p8_arg_num(const Val *base, int nargs, int i, fix32_t def) {
    return (i < nargs && base[i].t == P8_NUM) ? base[i].u.n : p8_arg_num_slow(base, nargs, i, def);
}
static inline int p8_arg_int(const Val *base, int nargs, int i, int def) { return fix32_to_int(p8_arg_num(base, nargs, i, fix32_from_int(def))); }

/* number to text, as tostr() prints it */
int p8_fix_to_str(char *out, fix32_t n);

#ifdef __cplusplus
}
#endif

#endif
