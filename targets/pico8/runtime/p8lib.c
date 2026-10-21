/* The PICO-8 functions the cartridges call (see p8_natives.def).  Semantics
 * follow femto8's p8_lua.c and lpico8lib.c. */
#include "p8rt.h"
#include "p8gfx.h"
#include "p8_audio.h"
#include "trigtables.h"
#include <string.h>

#define NUM(i, def) p8_arg_num(base, nargs, (i), (def))
#define INT(i, def) p8_arg_int(base, nargs, (i), (def))
#define RET_NUM(x) do { base[0] = V_NUM(x); return 1; } while (0)
#define RET_INT(x) RET_NUM(fix32_from_int(x))
#define RET_BOOL(x) do { base[0] = V_BOOL(x); return 1; } while (0)

/* ------------------------------------------------------------------ math */
int n_abs(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_abs(NUM(0, 0))); }
int n_ceil(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_ceil(NUM(0, 0))); }
int n_flr(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_flr(NUM(0, 0))); }
int n_max(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_max(NUM(0, 0), NUM(1, 0))); }
int n_min(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_min(NUM(0, 0), NUM(1, 0))); }
int n_mid(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_mid(NUM(0, 0), NUM(1, 0), NUM(2, 0))); }
int n_sgn(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_sgn(NUM(0, 0))); }
int n_band(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_band(NUM(0, 0), NUM(1, 0))); }
int n_bor(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_bor(NUM(0, 0), NUM(1, 0))); }
int n_bxor(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_bxor(NUM(0, 0), NUM(1, 0))); }
int n_bnot(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_bnot(NUM(0, 0))); }
int n_shl(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_shl(NUM(0, 0), fix32_to_int(NUM(1, 0)))); }
int n_shr(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_shr(NUM(0, 0), fix32_to_int(NUM(1, 0)))); }
int n_lshr(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_lshr(NUM(0, 0), fix32_to_int(NUM(1, 0)))); }
int n_rotl(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_rotl(NUM(0, 0), fix32_to_int(NUM(1, 0)))); }
int n_rotr(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(fix32_rotr(NUM(0, 0), fix32_to_int(NUM(1, 0)))); }

static fix32_t sin_helper(fix32_t x) {
    int32_t xbits = x;
    int32_t b = (xbits & 0x4000) ? fix32_bnot(x) : xbits;
    int32_t a = (b & 0x3fff) + 2;
    int32_t idx = a >> 2;
    if (idx > 4095) idx = 4095;
    fix32_t ret = (fix32_t)((idx << 4) + p8_rom_u16(&sintable[idx]));
    return (xbits & 0x8000) ? ret : fix32_neg(ret);
}
int n_sin(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(sin_helper(NUM(0, 0))); }
int n_cos(Val *base, int nargs, Clo *cl) { (void)cl; RET_NUM(sin_helper(fix32_sub(NUM(0, 0), 0x4000))); }

int n_atan2(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int32_t xbits = NUM(0, 0), ybits = NUM(1, 0);
    int32_t bits = 0x4000;
    if (xbits != 0) {
        int64_t ax = xbits < 0 ? -(int64_t)xbits : xbits, ay = ybits < 0 ? -(int64_t)ybits : ybits;
        int64_t q = (ay << 16) / ax;
        if (q > 0x10000) bits -= p8_rom_u16(&atantable[((int64_t)1 << 32) / q >> 5]);
        else bits = p8_rom_u16(&atantable[q >> 5]);
    }
    if (xbits < 0) bits = 0x8000 - bits;
    if (ybits > 0) bits = -bits & 0xffff;
    if (xbits != 0 && ybits == (int32_t)0x80000000) bits = -bits & 0xffff;
    RET_NUM(bits);
}

int n_sqrt(Val *base, int nargs, Clo *cl) {
    (void)cl;
    int64_t root = 0, x = (int64_t)NUM(0, 0) << 16;
    if (x > 0)
        for (int64_t a = (int64_t)1 << 46; a; a >>= 2, root >>= 1)
            if (x >= a + root) { x -= a + root; root += a << 1; }
    RET_NUM((int32_t)root);
}

/* random numbers: femto8's p8_seed_rng_state and rnd */
static uint32_t rng_hi, rng_lo;
void p8_seed_rng(uint32_t seed) {
    if (seed == 0) { rng_hi = 0x60009755u; rng_lo = 0xdeadbeefu; }
    else { seed &= 0x7fffffffu; rng_hi = seed ^ 0xbead29bau; rng_lo = seed; }
    for (int i = 0; i < 32; i++) {
        rng_hi = (rng_hi << 16) | (rng_hi >> 16);
        rng_hi += rng_lo;
        rng_lo += rng_hi;
    }
}

int n_rnd(Val *base, int nargs, Clo *cl) {
    (void)cl;
    rng_hi = (rng_hi << 16) | (rng_hi >> 16);
    rng_hi += rng_lo;
    rng_lo += rng_hi;
    if (nargs >= 1 && base[0].t == P8_TAB) {
        const int32_t len = p8_tab_len(base[0].u.t);
        if (len > 0) { base[0] = p8_tab_geti(base[0].u.t, (int32_t)((rng_hi >> 16) % (uint32_t)len) + 1); return 1; }
        base[0] = V_NIL;
        return 1;
    }
    const uint32_t max_fixed = nargs >= 1 ? (uint32_t)NUM(0, 0) : 0x10000u;
    RET_NUM((fix32_t)(max_fixed == 0 ? 0 : rng_hi % max_fixed));
}

int n_srand(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const uint32_t prev = rng_lo;
    p8_seed_rng((uint32_t)NUM(0, 0));
    RET_NUM((fix32_t)prev);
}

int n_time(Val *base, int nargs, Clo *cl) {
    (void)cl; (void)nargs;
    RET_NUM((fix32_t)(((uint64_t)p8_frames << 16) / p8_fps));
}

/* ----------------------------------------------------------- types, text */
int n_type(Val *base, int nargs, Clo *cl) {
    (void)cl;
    static const char *const names[] = { "nil", "boolean", "number", "string", "table", "function", "function" };
    if (!nargs) p8_error("bad argument to type", "");
    const char *s = names[base[0].t];
    base[0] = V_STR(p8_str_new(s, strlen(s)));
    return 1;
}

static Val str_val(const char *s, size_t len) { return V_STR(p8_str_new(s, len)); }

int n_tostr(Val *base, int nargs, Clo *cl) {
    (void)cl;
    char buf[24];
    if (!nargs) { base[0] = str_val("", 0); return 1; }
    switch (base[0].t) {
    case P8_NUM: { int n = p8_fix_to_str(buf, base[0].u.n); base[0] = str_val(buf, (size_t)n); return 1; }
    case P8_STR: return 1;
    case P8_BOOL: base[0] = base[0].u.b ? str_val("true", 4) : str_val("false", 5); return 1;
    case P8_NIL: base[0] = str_val("[nil]", 5); return 1;
    case P8_TAB: base[0] = str_val("[table]", 7); return 1;
    default: base[0] = str_val("[function]", 10); return 1;
    }
}

int n_tonum(Val *base, int nargs, Clo *cl) {
    (void)cl; (void)nargs;
    int ok;
    if (base[0].t == P8_BOOL) RET_INT(base[0].u.b ? 1 : 0);
    const fix32_t n = p8_tonumber_slow(base[0], &ok);
    if (!ok) return 0;
    RET_NUM(n);
}

int n_chr(Val *base, int nargs, Clo *cl) {
    (void)cl;
    char buf[32];
    if (nargs > (int)sizeof buf) nargs = sizeof buf;
    for (int i = 0; i < nargs; i++) buf[i] = (char)(uint8_t)fix32_to_int(NUM(i, 0));
    base[0] = str_val(buf, (size_t)nargs);
    return 1;
}

int n_ord(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs < 1 || base[0].t != P8_STR) return 0;
    const int n = nargs >= 2 ? fix32_to_int(NUM(1, 0)) - 1 : 0;
    if (n < 0 || (uint32_t)n >= base[0].u.s->len) return 0;
    RET_INT((uint8_t)base[0].u.s->s[n]);
}

int n_sub(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs < 1 || (base[0].t != P8_STR && base[0].t != P8_NUM)) p8_error("bad argument to sub", "");
    Val v = base[0];
    char buf[16];
    const char *str;
    int str_len;
    if (v.t == P8_NUM) { str_len = p8_fix_to_str(buf, v.u.n); str = buf; }
    else { str = v.u.s->s; str_len = (int)v.u.s->len; }
    int start = INT(1, 0), end = INT(2, -1);
    const Str *source = v.t == P8_STR ? v.u.s : 0;
    if (start < 1) start = 1;
    if (start > str_len + 1) start = str_len + 1;
    int len;
    if (end == -1) len = str_len - start + 1;
    else {
        if (end < 0) end = str_len + end + 1;
        if (end < 1) end = 1;
        if (end > str_len) end = str_len;
        len = end - start + 1;
    }
    if (len < 0) len = 0;
    base[0] = source ? V_STR(p8_str_sub(source, (size_t)(start - 1), (size_t)len)) : str_val(str + start - 1, (size_t)len);
    return 1;
}

int n_assert(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs < 1 || !P8_TRUTHY(base[0])) {
        static char text[48];
        const char *msg = "assertion failed";
        if (nargs >= 2 && base[1].t == P8_STR) {
            const unsigned n = base[1].u.s->len < sizeof text - 1 ? base[1].u.s->len : (unsigned)sizeof text - 1;
            memcpy(text, base[1].u.s->s, n);
            text[n] = 0;
            msg = text;
        }
        p8_error(msg, "");
    }
    return nargs;
}

int n_select(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs >= 1 && base[0].t == P8_STR) RET_INT(nargs - 1); /* select("#", ...) */
    int n = INT(0, 1);
    if (n < 0) n = nargs + n; /* from the end */
    if (n < 1 || n >= nargs) return 0;
    for (int i = n; i < nargs; i++) base[i - n] = base[i];
    return nargs - n;
}

int n_unpack(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs < 1 || base[0].t != P8_TAB) p8_error("bad argument to unpack", "");
    const Tab *t = base[0].u.t;
    const int first = INT(1, 1), last = INT(2, p8_tab_len(t));
    if (last < first) return 0;
    if (last - first + 1 > 16) p8_error("too many values to unpack", "");
    for (int i = first; i <= last; i++) base[i - first] = p8_tab_geti(t, i);
    return last - first + 1;
}

int n_next(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs < 1 || base[0].t != P8_TAB) p8_error("bad argument to next", "");
    Val k, v;
    if (!p8_tab_next(base[0].u.t, nargs >= 2 ? base[1] : V_NIL, &k, &v)) { base[0] = V_NIL; return 1; }
    base[0] = k;
    base[1] = v;
    return 2;
}

int n_pairs(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs < 1) p8_error("bad argument to pairs", "");
    Val t = base[0];
    base[0] = V_CFN(n_next);
    base[1] = t;
    base[2] = V_NIL;
    return 3;
}

/* -------------------------------------------------------------- graphics */
int p8_gfx_pget(int x, int y);
void p8_gfx_pset(int x, int y, int col);
void p8_gfx_rectfill(int x0, int y0, int x1, int y1, int col);
void p8_gfx_rect(int x0, int y0, int x1, int y1, int col);
void p8_gfx_line(int x0, int y0, int x1, int y1, int col);
void p8_gfx_circ(int x, int y, int r, int col);
void p8_gfx_circfill(int x, int y, int r, int col);
void p8_gfx_sprites(int n, int x, int y, int w, int h, int flip_x, int flip_y);
void p8_gfx_sspr(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int flip_x, int flip_y);
void p8_gfx_map(int celx, int cely, int sx, int sy, int celw, int celh, int layer);
int p8_gfx_mget(int celx, int cely);

int n_nop(Val *base, int nargs, Clo *cl) { (void)base; (void)nargs; (void)cl; return 0; }

#define PEN(i) (nargs > (i) ? (p8g.pen = (uint8_t)INT(i, 0), INT(i, 0)) : p8g.pen)

int n_cls(Val *base, int nargs, Clo *cl) { (void)cl; p8_gfx_cls(nargs == 1 ? INT(0, 0) : 0); return 0; }

int n_color(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int prev = p8g.pen;
    p8g.pen = (uint8_t)(nargs == 1 ? INT(0, 0) : 6);
    RET_INT(prev);
}

int n_camera(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int px = p8g.cam_x, py = p8g.cam_y;
    p8g.cam_x = (int16_t)(nargs >= 1 ? INT(0, 0) : 0);
    p8g.cam_y = (int16_t)(nargs >= 2 ? INT(1, 0) : 0);
    base[0] = V_NUM(fix32_from_int(px));
    base[1] = V_NUM(fix32_from_int(py));
    return 2;
}

int n_clip(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int px0 = p8g.clip_x0, py0 = p8g.clip_y0, px1 = p8g.clip_x1, py1 = p8g.clip_y1;
    if (!nargs) { p8g.clip_x0 = 0; p8g.clip_y0 = 0; p8g.clip_x1 = P8_W; p8g.clip_y1 = P8_H; }
    else {
        int x0 = INT(0, 0), y0 = INT(1, 0), x1 = x0 + INT(2, 0), y1 = y0 + INT(3, 0);
        int cx0 = 0, cy0 = 0, cx1 = P8_W, cy1 = P8_H;
        if (nargs >= 5 && P8_TRUTHY(base[4])) { cx0 = px0; cy0 = py0; cx1 = px1; cy1 = py1; }
        if (x0 < cx0) x0 = cx0;
        if (y0 < cy0) y0 = cy0;
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;
        p8g.clip_x0 = (int16_t)x0; p8g.clip_y0 = (int16_t)y0;
        p8g.clip_x1 = (int16_t)(x1 < 0 ? 0 : x1); p8g.clip_y1 = (int16_t)(y1 < 0 ? 0 : y1);
    }
    base[0] = V_NUM(fix32_from_int(px0));
    base[1] = V_NUM(fix32_from_int(py0));
    base[2] = V_NUM(fix32_from_int(px1 - px0));
    base[3] = V_NUM(fix32_from_int(py1 - py0));
    return 4;
}

int n_cursor(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int px = p8g.cur_x, py = p8g.cur_y;
    p8g.cur_x = (int16_t)(nargs >= 1 ? INT(0, 0) : 0);
    p8g.cur_y = (int16_t)(nargs >= 2 ? INT(1, 0) : 0);
    p8g.margin = p8g.cur_x;
    if (nargs >= 3) p8g.pen = (uint8_t)INT(2, 0);
    base[0] = V_NUM(fix32_from_int(px));
    base[1] = V_NUM(fix32_from_int(py));
    return 2;
}

int n_fget(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int flags = p8_rom_u8(&p8_cart[0x3000 + (INT(0, 0) & 255)]);
    if (nargs == 2) RET_BOOL(flags & (1 << INT(1, 0)));
    RET_INT(flags);
}

int n_pset(Val *base, int nargs, Clo *cl) {
    (void)cl;
    p8_gfx_pset(INT(0, 0), INT(1, 0), PEN(2));
    return 0;
}

int n_pget(Val *base, int nargs, Clo *cl) { (void)cl; RET_INT(p8_gfx_pget(INT(0, 0), INT(1, 0))); }

static void box_args(const Val *base, int nargs, int *l, int *t, int *r, int *b, int *col) {
    const int x0 = INT(0, 0), y0 = INT(1, 0), x1 = INT(2, 0), y1 = INT(3, 0);
    *l = x0 < x1 ? x0 : x1;
    *r = x0 < x1 ? x1 : x0;
    *t = y0 < y1 ? y0 : y1;
    *b = y0 < y1 ? y1 : y0;
    *col = PEN(4);
}

int n_rect(Val *base, int nargs, Clo *cl) {
    (void)cl;
    int l, t, r, b, c;
    box_args(base, nargs, &l, &t, &r, &b, &c);
    p8_gfx_rect(l, t, r, b, c);
    return 0;
}

int n_rectfill(Val *base, int nargs, Clo *cl) {
    (void)cl;
    int l, t, r, b, c;
    box_args(base, nargs, &l, &t, &r, &b, &c);
    p8_gfx_rectfill(l, t, r, b, c);
    return 0;
}

int n_line(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs >= 4) p8_gfx_line(INT(0, 0), INT(1, 0), INT(2, 0), INT(3, 0), PEN(4));
    return 0;
}

int n_circ(Val *base, int nargs, Clo *cl) {
    (void)cl;
    p8_gfx_circ(INT(0, 0), INT(1, 0), INT(2, 4), PEN(3));
    return 0;
}

int n_oval(Val *base, int nargs, Clo *cl) {
    (void)cl;
    p8_gfx_oval(INT(0, 0), INT(1, 0), INT(2, 0), INT(3, 0), PEN(4), 0);
    return 0;
}

int n_ovalfill(Val *base, int nargs, Clo *cl) {
    (void)cl;
    p8_gfx_oval(INT(0, 0), INT(1, 0), INT(2, 0), INT(3, 0), PEN(4), 1);
    return 0;
}

int n_sget(Val *base, int nargs, Clo *cl) { (void)cl; RET_INT(p8_gfx_sget(INT(0, 0), INT(1, 0))); }

int n_circfill(Val *base, int nargs, Clo *cl) {
    (void)cl;
    p8_gfx_circfill(INT(0, 0), INT(1, 0), INT(2, 4), PEN(3));
    return 0;
}

int n_spr(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int n = INT(0, 0), x = INT(1, 0), y = INT(2, 0);
    int w = 1, h = 1;
    if (nargs > 3) {
        const fix32_t wr = NUM(3, 0), hr = NUM(4, 0);
        if ((wr & 0xffff) || (hr & 0xffff)) p8_error("spr with fractional size is not supported", "");
        w = fix32_to_int(wr);
        h = fix32_to_int(hr);
    }
    p8_gfx_sprites(n, x, y, w, h, nargs >= 6 && P8_TRUTHY(base[5]), nargs >= 7 && P8_TRUTHY(base[6]));
    return 0;
}

int n_sspr(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int sw = INT(2, 0), sh = INT(3, 0);
    p8_gfx_sspr(INT(0, 0), INT(1, 0), sw, sh, INT(4, 0), INT(5, 0), INT(6, sw), INT(7, sh),
                nargs >= 9 && P8_TRUTHY(base[8]), nargs >= 10 && P8_TRUTHY(base[9]));
    return 0;
}

int n_map(Val *base, int nargs, Clo *cl) {
    (void)cl;
    p8_gfx_map(INT(0, 0), INT(1, 0), INT(2, 0), INT(3, 0), INT(4, 128), INT(5, 64), INT(6, 0));
    return 0;
}

int n_mget(Val *base, int nargs, Clo *cl) { (void)cl; RET_INT(p8_gfx_mget(INT(0, 0), INT(1, 0))); }

int n_pal(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs == 0) {
        for (int i = 0; i < 16; i++) { p8g.pal_draw[i] = (uint8_t)(i == 0 ? 0x10 : i); p8g.pal_screen[i] = (uint8_t)i; }
        p8_gfx_pal_changed();
        return 0;
    }
    if (base[0].t == P8_TAB) {
        const int p = nargs >= 2 ? INT(1, 0) : 0;
        Val k = V_NIL, v;
        while (p8_tab_next(base[0].u.t, k, &k, &v)) {
            const int c0 = fix32_to_int(k.u.n) & 15, c1 = fix32_to_int(v.u.n);
            if (p == 0) p8g.pal_draw[c0] = (uint8_t)((c1 & 15) | (p8g.pal_draw[c0] & 0xf0));
            else if (p == 1) p8g.pal_screen[c0] = (uint8_t)c1;
        }
        p8_gfx_pal_changed();
        return 0;
    }
    if (nargs == 1) {
        const int p = INT(0, 0);
        for (int i = 0; i < 16; i++) {
            if (p == 0) p8g.pal_draw[i] = (uint8_t)(i == 0 ? 0x10 : i);
            else if (p == 1) p8g.pal_screen[i] = (uint8_t)i;
        }
        p8_gfx_pal_changed();
        return 0;
    }
    const int c0 = INT(0, 0) & 15, c1 = INT(1, 0), p = nargs == 3 ? INT(2, 0) : 0;
    int old;
    if (p == 0) { old = p8g.pal_draw[c0]; p8g.pal_draw[c0] = (uint8_t)((c1 & 15) | (old & 0xf0)); }
    else if (p == 1) { old = p8g.pal_screen[c0]; p8g.pal_screen[c0] = (uint8_t)c1; }
    else old = 0;
    p8_gfx_pal_changed();
    RET_INT(old & 15);
}

int n_palt(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs == 0) {
        for (int i = 0; i < 16; i++) p8g.pal_draw[i] = (uint8_t)(i == 0 ? p8g.pal_draw[i] | 0x10 : p8g.pal_draw[i] & 15);
        p8_gfx_pal_changed();
    } else if (nargs == 1) {
        const unsigned b = (uint32_t)NUM(0, 0) >> 16;
        for (int i = 0; i < 16; i++) {
            const uint8_t c = p8g.pal_draw[i];
            p8g.pal_draw[i] = (b >> (15 - i)) & 1 ? (uint8_t)(c | 0x10) : (uint8_t)(c & 15);
        }
        p8_gfx_pal_changed();
    } else {
        const int col = INT(0, 0) & 15;
        const uint8_t c = p8g.pal_draw[col];
        p8g.pal_draw[col] = P8_TRUTHY(base[1]) ? (uint8_t)(c | 0x10) : (uint8_t)(c & 15);
        p8_gfx_pal_changed();
        RET_BOOL(c & 0x10);
    }
    return 0;
}

int n_print(Val *base, int nargs, Clo *cl) {
    (void)cl;
    char numbuf[16];
    const char *s = "";
    size_t len = 0;
    if (nargs >= 1) {
        if (base[0].t == P8_STR) { s = base[0].u.s->s; len = base[0].u.s->len; }
        else if (base[0].t == P8_NUM) { len = (size_t)p8_fix_to_str(numbuf, base[0].u.n); s = numbuf; }
    }
    int x, y, right;
    if (nargs <= 2) {
        const int col = nargs == 2 ? INT(1, 0) : p8g.pen;
        if (nargs == 2) p8g.pen = (uint8_t)col;
        right = p8_gfx_print(s, (unsigned)len, p8g.cur_x, p8g.cur_y, col, p8g.margin, &x, &y);
        p8g.cur_x = (int16_t)p8g.margin;
        p8g.cur_y = (int16_t)(y + 6);
    } else {
        x = INT(1, 0);
        y = INT(2, 0);
        const int col = nargs >= 4 ? INT(3, 0) : p8g.pen;
        if (nargs >= 4) p8g.pen = (uint8_t)col;
        p8g.margin = (int16_t)x;
        right = p8_gfx_print(s, (unsigned)len, x, y, col, x, &x, &y);
        p8g.cur_x = (int16_t)p8g.margin;
        p8g.cur_y = (int16_t)(y + 6);
    }
    RET_INT(right);
}

/* ----------------------------------------------------------------- input */
int n_btn(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs >= 1) {
        const int i = INT(0, 0), p = INT(1, 0);
        RET_BOOL(i >= 0 && i < 7 && p == 0 && (p8_buttons & (1u << i)));
    }
    RET_INT(p8_buttons & 0xff);
}

int n_btnp(Val *base, int nargs, Clo *cl) {
    (void)cl;
    if (nargs >= 1) {
        const int i = INT(0, 0), p = INT(1, 0);
        RET_BOOL(i >= 0 && i < 7 && p == 0 && (p8_btnp & (1u << i)));
    }
    RET_INT(p8_btnp & 0xff);
}

/* ----------------------------------------------------------------- sound */
int n_sfx(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int n = INT(0, 0), channel = INT(1, -1), offset = INT(2, 0), length = INT(3, 32 - offset);
    p8_audio_sfx(n, channel, offset, offset + length);
    return 0;
}

int n_music(Val *base, int nargs, Clo *cl) {
    (void)cl;
    const int previous = p8_audio_stat(54);
    p8_audio_music(INT(0, 0), INT(1, 0), INT(2, 0));
    RET_INT(previous);
}

int n_stat(Val *base, int nargs, Clo *cl) { (void)cl; RET_INT(p8_audio_stat(INT(0, 0))); }

void p8_lib_state_io(p8_state_fn io, void *ctx) {
    io(&rng_hi, sizeof rng_hi, ctx);
    io(&rng_lo, sizeof rng_lo, ctx);
}
