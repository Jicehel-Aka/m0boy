/* PICO-8 recompiler: reads a .p8 / .p8.png cartridge with femto8's parser,
 * compiles its Lua with femto8's compiler (PICO-8 syntax, 16.16 numbers) and
 * writes the bytecode out as C for the runtime in targets/pico8/runtime:
 * one C function per Lua function, the registers in the runtime's value
 * stack, globals as C variables.
 *
 *   p8c cart.p8.png -o dir        writes dir/p8_gen.c, dir/p8_cart.c
 *   p8c cart.p8.png --info        prints what the cartridge uses */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "p8_parser.h"

uint8_t *m_file_buffer;
#include "lua_api.h"
#define CART_BYTES 0x4300

static uint8_t cart[CART_BYTES + 0x2000];
static char lua_source[128 * 1024];

/* ---------------------------------------------------------------- natives */
static const char *const native_names[] = {
#define P8_NATIVE(name, fn) #name,
#include "p8_natives.def"
#undef P8_NATIVE
};
static const char *const native_fns[] = {
#define P8_NATIVE(name, fn) #fn,
#include "p8_natives.def"
#undef P8_NATIVE
};
#define N_NATIVES (sizeof native_names / sizeof *native_names)
static const struct { const char *name; uint32_t raw; } const_globals[] = {
#define P8_CONST(name, raw) { (const char[]){ name, 0 }, raw },
#include "p8_consts.def"
#undef P8_CONST
};
#define N_CONSTS (sizeof const_globals / sizeof *const_globals)

/* ------------------------------------------------------------- registries */
typedef struct { char *name; size_t len; int set, get, native, constant; } Global;
static Global globals[512];
static int n_globals;
static int global_id(const char *name, size_t len, int is_set) {
    for (int i = 0; i < n_globals; i++)
        if (globals[i].len == len && !memcmp(globals[i].name, name, len)) {
            if (is_set) globals[i].set = 1; else globals[i].get = 1;
            return i;
        }
    if (n_globals == 512) { fprintf(stderr, "too many globals\n"); exit(1); }
    Global *g = &globals[n_globals];
    g->name = malloc(len + 1);
    memcpy(g->name, name, len);
    g->name[len] = 0;
    g->len = len;
    g->native = g->constant = -1;
    for (size_t i = 0; i < N_NATIVES; i++)
        if (strlen(native_names[i]) == len && !memcmp(native_names[i], name, len)) g->native = (int)i;
    for (size_t i = 0; i < N_CONSTS; i++)
        if (strlen(const_globals[i].name) == len && !memcmp(const_globals[i].name, name, len)) g->constant = (int)i;
    if (is_set) g->set = 1; else g->get = 1;
    return n_globals++;
}

typedef struct { char *s; size_t len; } StrK;
static StrK strs[4096];
static int n_strs;
static int str_id(const char *s, size_t len) {
    for (int i = 0; i < n_strs; i++)
        if (strs[i].len == len && !memcmp(strs[i].s, s, len)) return i;
    if (n_strs == 4096) { fprintf(stderr, "too many strings\n"); exit(1); }
    strs[n_strs].s = malloc(len + 1);
    memcpy(strs[n_strs].s, s, len);
    strs[n_strs].len = len;
    return n_strs++;
}

static int uses_pow;
/* constants (numbers, booleans, nil, strings) as static values, passed by pointer */
typedef struct { int kind; int32_t num; int str; } KConst; /* kind: 0 nil, 1 false, 2 true, 3 number, 4 string */
static KConst konsts[4096];
static int n_konsts;
static int konst_id(int kind, int32_t num, int str) {
    for (int i = 0; i < n_konsts; i++)
        if (konsts[i].kind == kind && konsts[i].num == num && konsts[i].str == str) return i;
    if (n_konsts == 4096) { fprintf(stderr, "too many constants\n"); exit(1); }
    konsts[n_konsts] = (KConst){ kind, num, str };
    return n_konsts++;
}
static const Proto *protos[1024];
static int n_protos;
static int proto_id(const Proto *p) {
    for (int i = 0; i < n_protos; i++) if (protos[i] == p) return i;
    return -1;
}
static void collect(const Proto *p) {
    if (n_protos == 1024) { fprintf(stderr, "too many functions\n"); exit(1); }
    protos[n_protos++] = p;
    for (int i = 0; i < p->sizep; i++) collect(p->p[i]);
}

static int is_env_upvalue(const Proto *p, int idx) {
    const TString *name = p->upvalues[idx].name;
    return name && !strcmp(getstr(name), "_ENV");
}

/* Closures hold only the upvalues that are not _ENV (globals are C variables):
 * the index of upvalue u among them, and how many there are. */
static int up_index(const Proto *p, int u) {
    int n = 0;
    for (int i = 0; i < u; i++) n += !is_env_upvalue(p, i);
    return n;
}
static int up_count(const Proto *p) { return up_index(p, p->sizeupvalues); }

/* GETTABUP / SETTABUP on _ENV with a constant name is a global; on any other
 * upvalue it is an ordinary table access.  Returns the global's number, or -1. */
static int global_of(const Proto *p, int upv, int rk, int is_set) {
    if (!is_env_upvalue(p, upv)) return -1;
    if (!ISK(rk) || !ttisstring(&p->k[INDEXK(rk)])) {
        fprintf(stderr, "error: dynamic global access (_G or a computed name) is not supported\n");
        exit(1);
    }
    const TString *s = rawtsvalue(&p->k[INDEXK(rk)]);
    return global_id(getstr(s), s->tsv.len, is_set);
}

/* ------------------------------------------------------------------ emit */
static FILE *out;
#define E(...) fprintf(out, __VA_ARGS__)

/* RK operand as a pointer: a register of the frame or a constant */
static void emit_rk(const Proto *p, int rk) {
    if (!ISK(rk)) { E("&R[%d]", rk); return; }
    const TValue *k = &p->k[INDEXK(rk)];
    int id;
    if (ttisnil(k)) { E("&p8_k_nil"); return; }
    else if (ttisboolean(k)) { E(bvalue(k) ? "&p8_k_true" : "&p8_k_false"); return; }
    else if (ttisnumber(k)) id = konst_id(3, (int32_t)nvalue(k), 0);
    else if (ttisstring(k)) id = konst_id(4, 0, str_id(svalue(k), tsvalue(k)->len));
    else { fprintf(stderr, "bad constant\n"); exit(1); }
    E("&K%d", id);
}

/* the constant itself, to copy into a register */
static void emit_k(const Proto *p, int idx) { E("*"); emit_rk(p, idx | BITRK); }

static int rk_is_str(const Proto *p, int rk) { return ISK(rk) && ttisstring(&p->k[INDEXK(rk)]); }

static const char *const ARITH_FN[] = { "p8_add_p", "p8_sub_p", "p8_mul_p", "p8_div_p", "p8_mod_p" };

static void emit_str_key(const Proto *p, int rk) { E("&S%d", str_id(svalue(&p->k[INDEXK(rk)]), tsvalue(&p->k[INDEXK(rk)])->len)); }

static void emit_proto(const Proto *p, int id) {
    const int n = p->sizecode;
    unsigned char *target = calloc((size_t)n + 2, 1);
    int has_closure_upvals = 0;
    for (int pc = 0; pc < n; pc++) {
        const Instruction i = p->code[pc];
        switch (GET_OPCODE(i)) {
        case OP_JMP: case OP_FORLOOP: case OP_FORPREP: case OP_TFORLOOP: target[pc + 1 + GETARG_sBx(i)] = 1; break;
        case OP_EQ: case OP_LT: case OP_LE: case OP_TEST: case OP_TESTSET: target[pc + 2] = 1; break;
        case OP_LOADBOOL: if (GETARG_C(i)) target[pc + 2] = 1; break;
        case OP_CLOSURE: {
            const Proto *child = p->p[GETARG_Bx(i)];
            for (int u = 0; u < child->sizeupvalues; u++)
                if (child->upvalues[u].instack && !is_env_upvalue(child, u)) has_closure_upvals = 1;
            break;
        }
        default: break;
        }
    }
    const int vararg = p->is_vararg != 0;
    const int maxstack = p->maxstacksize;
    E("static int f%d(Val *base, int nargs, Clo *cl) {\n", id);
    E("    Val *R = %s; Val *const old_top = p8_top; int top = 0; (void)cl; (void)top;\n", vararg ? "base + nargs" : "base");
    if (vararg) E("    const int nvar = nargs > %d ? nargs - %d : 0; (void)nvar;\n", p->numparams, p->numparams);
    E("    if (R + %d > p8_stack + P8_STACK_VALS) p8_error(\"stack overflow\", \"\");\n", maxstack);
    if (vararg) E("    for (int i = 0; i < %d; i++) R[i] = i < %d && i < nargs ? base[i] : V_NIL;\n", maxstack, p->numparams);
    else E("    for (int i = nargs; i < %d; i++) R[i] = V_NIL;\n", maxstack);
    E("    p8_top = R + %d; P8_TRACK_STACK(p8_top);\n", maxstack);
    for (int pc = 0; pc < n; pc++) {
        const Instruction i = p->code[pc];
        const int a = GETARG_A(i), b = GETARG_B(i), c = GETARG_C(i), bx = GETARG_Bx(i), sbx = GETARG_sBx(i);
        const OpCode op = GET_OPCODE(i);
        if (target[pc]) E("L%d:;\n", pc);
        E("    /* %d %s %d %d %d */ ", pc, luaP_opnames[op], a, b, c);
        switch (op) {
        case OP_MOVE: E("R[%d] = R[%d];\n", a, b); break;
        case OP_LOADK: E("R[%d] = ", a); emit_k(p, bx); E(";\n"); break;
        case OP_LOADKX: E("R[%d] = ", a); emit_k(p, (int)GETARG_Ax(p->code[pc + 1])); E(";\n"); break;
        case OP_LOADBOOL: E("R[%d] = %s;\n", a, b ? "p8_k_true" : "p8_k_false"); if (c) E("    goto L%d;\n", pc + 2); break;
        case OP_LOADNIL: E("for (int i = %d; i <= %d; i++) R[i] = p8_k_nil;\n", a, a + b); break;
        case OP_GETUPVAL:
            if (is_env_upvalue(p, b)) { fprintf(stderr, "error: _ENV as a value is not supported\n"); exit(1); }
            E("R[%d] = *cl->up[%d]->v;\n", a, up_index(p, b));
            break;
        case OP_GETTABUP: {
            const int g = global_of(p, b, c, 0);
            if (g >= 0 && globals[g].native >= 0 && !globals[g].set) E("R[%d].u.f = %s; R[%d].t = P8_CFN;\n", a, native_fns[globals[g].native], a);
            else if (g >= 0 && globals[g].constant >= 0 && !globals[g].set) E("R[%d].u.n = (fix32_t)0x%xu; R[%d].t = P8_NUM;\n", a, const_globals[globals[g].constant].raw, a);
            else if (g >= 0) E("R[%d] = G%d;\n", a, g);
            else { E("p8_get_p(&R[%d], cl->up[%d]->v, ", a, up_index(p, b)); emit_rk(p, c); E(");\n"); }
            break;
        }
        case OP_GETTABLE:
            if (rk_is_str(p, c)) { E("p8_gets_p(&R[%d], &R[%d], ", a, b); emit_str_key(p, c); E(");\n"); }
            else { E("p8_get_p(&R[%d], &R[%d], ", a, b); emit_rk(p, c); E(");\n"); }
            break;
        case OP_SETTABUP: {
            const int g = global_of(p, a, b, 1);
            if (g >= 0) { E("G%d = *", g); emit_rk(p, c); E(";\n"); }
            else { E("p8_set_p(cl->up[%d]->v, ", up_index(p, a)); emit_rk(p, b); E(", "); emit_rk(p, c); E(");\n"); }
            break;
        }
        case OP_SETUPVAL: E("*cl->up[%d]->v = R[%d];\n", up_index(p, b), a); break;
        case OP_SETTABLE: E("p8_set_p(&R[%d], ", a); emit_rk(p, b); E(", "); emit_rk(p, c); E(");\n"); break;
        case OP_NEWTABLE: E("p8_newtab_p(&R[%d], %d, %d);\n", a, luaO_fb2int(b), luaO_fb2int(c)); break;
        case OP_SELF:
            E("R[%d] = R[%d]; ", a + 1, b);
            if (rk_is_str(p, c)) { E("p8_gets_p(&R[%d], &R[%d], ", a, a + 1); emit_str_key(p, c); E(");\n"); }
            else { E("p8_get_p(&R[%d], &R[%d], ", a, a + 1); emit_rk(p, c); E(");\n"); }
            break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            E("%s(&R[%d], ", ARITH_FN[op - OP_ADD], a); emit_rk(p, b); E(", "); emit_rk(p, c); E(");\n");
            break;
        case OP_POW: case OP_IDIV: case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_LSHR: case OP_ROTL: case OP_ROTR: {
            static const struct { OpCode op; const char *name; } map[] = {
                { OP_POW, "P8_OP_POW" }, { OP_IDIV, "P8_OP_IDIV" }, { OP_BAND, "P8_OP_BAND" }, { OP_BOR, "P8_OP_BOR" },
                { OP_BXOR, "P8_OP_BXOR" }, { OP_SHL, "P8_OP_SHL" }, { OP_SHR, "P8_OP_SHR" }, { OP_LSHR, "P8_OP_LSHR" },
                { OP_ROTL, "P8_OP_ROTL" }, { OP_ROTR, "P8_OP_ROTR" },
            };
            const char *name = "";
            if (op == OP_POW) uses_pow = 1;
            for (unsigned m = 0; m < sizeof map / sizeof *map; m++) if (map[m].op == op) name = map[m].name;
            E("p8_arith_p(%s, &R[%d], ", name, a); emit_rk(p, b); E(", "); emit_rk(p, c); E(");\n");
            break;
        }
        case OP_UNM: E("p8_unm_p(&R[%d], &R[%d]);\n", a, b); break;
        case OP_NOT: E("R[%d] = P8_TRUTHY(R[%d]) ? p8_k_false : p8_k_true;\n", a, b); break;
        case OP_LEN: E("p8_len_p(&R[%d], &R[%d]);\n", a, b); break;
        case OP_CONCAT: E("p8_concat_p(&R[%d], &R[%d], %d);\n", a, b, c - b + 1); break;
        case OP_JMP:
            if (a) E("p8_upval_close(&R[%d]); ", a - 1);
            E("goto L%d;\n", pc + 1 + sbx);
            break;
        case OP_EQ: case OP_LT: case OP_LE: {
            static const char *const fn[] = { "p8_eqi_p", "p8_lti_p", "p8_lei_p" };
            E("if (%s(", fn[op - OP_EQ]); emit_rk(p, b); E(", "); emit_rk(p, c); E(") != %d) goto L%d;\n", a, pc + 2);
            break;
        }
        case OP_TEST: E("if (P8_TRUTHY(R[%d]) != %d) goto L%d;\n", a, c, pc + 2); break;
        case OP_TESTSET: E("if (P8_TRUTHY(R[%d]) != %d) goto L%d; else R[%d] = R[%d];\n", b, c, pc + 2, a, b); break;
        case OP_CALL:
            E("{ int n_ = p8_call(&R[%d], ", a);
            if (b) E("%d", b - 1); else E("top - %d", a + 1);
            E(");");
            if (c == 0) E(" top = %d + n_;", a);
            else E(" for (int i_ = n_; i_ < %d; i_++) R[%d + i_] = V_NIL;", c - 1, a);
            E(" }\n");
            break;
        case OP_TAILCALL:
            if (has_closure_upvals) E("p8_upval_close(R); ");
            E("{ int n_ = p8_call(&R[%d], ", a);
            if (b) E("%d", b - 1); else E("top - %d", a + 1);
            E("); for (int i_ = 0; i_ < n_; i_++) base[i_] = R[%d + i_]; p8_top = old_top; return n_; }\n", a);
            break;
        case OP_RETURN:
            if (has_closure_upvals) E("p8_upval_close(R); ");
            E("{ int n_ = ");
            if (b) E("%d", b - 1); else E("top - %d", a);
            E("; for (int i_ = 0; i_ < n_; i_++) base[i_] = R[%d + i_]; p8_top = old_top; return n_; }\n", a);
            break;
        case OP_FORPREP: E("p8_forprep(&R[%d]); goto L%d;\n", a, pc + 1 + sbx); break;
        case OP_FORLOOP:
            E("{ const fix32_t step_ = R[%d].u.n, prev_ = R[%d].u.n, idx_ = fix32_add(prev_, step_), limit_ = R[%d].u.n;\n", a + 2, a, a + 1);
            E("      const int wrapped_ = step_ > 0 ? (idx_ < prev_ && prev_ <= limit_) : (prev_ < idx_ && limit_ <= prev_);\n");
            E("      if (!wrapped_ && (step_ > 0 ? idx_ <= limit_ : limit_ <= idx_)) { R[%d] = V_NUM(idx_); R[%d] = V_NUM(idx_); goto L%d; } }\n", a, a + 3, pc + 1 + sbx);
            break;
        case OP_TFORCALL:
            E("R[%d] = R[%d]; R[%d] = R[%d]; R[%d] = R[%d];\n", a + 5, a + 2, a + 4, a + 1, a + 3, a);
            E("    { int n_ = p8_call(&R[%d], 2); for (int i_ = n_; i_ < %d; i_++) R[%d + i_] = V_NIL; }\n", a + 3, c, a + 3);
            break;
        case OP_TFORLOOP: E("if (R[%d].t != P8_NIL) { R[%d] = R[%d]; goto L%d; }\n", a + 1, a, a + 1, pc + 1 + sbx); break;
        case OP_SETLIST: {
            int cc = c;
            if (cc == 0) cc = (int)GETARG_Ax(p->code[pc + 1]);
            E("{ const int n_ = ");
            if (b) E("%d", b); else E("top - %d", a + 1);
            E("; for (int i_ = 1; i_ <= n_; i_++) p8_tab_seti(R[%d].u.t, %d + i_, R[%d + i_]); }\n", a, (cc - 1) * LFIELDS_PER_FLUSH, a);
            break;
        }
        case OP_CLOSURE: {
            const Proto *child = p->p[bx];
            const int cid = proto_id(child), nup = up_count(child);
            if (nup == 0) E("R[%d] = V_FUN(&C%d);\n", a, cid);
            else {
                E("{ Clo *c_ = p8_clo_new(f%d, %d); R[%d] = V_FUN(c_);", cid, nup, a);
                for (int u = 0; u < child->sizeupvalues; u++) {
                    if (is_env_upvalue(child, u)) continue;
                    if (child->upvalues[u].instack) E(" c_->up[%d] = p8_upval_find(&R[%d]);", up_index(child, u), child->upvalues[u].idx);
                    else E(" c_->up[%d] = cl->up[%d];", up_index(child, u), up_index(p, child->upvalues[u].idx));
                }
                E(" }\n");
            }
            break;
        }
        case OP_VARARG:
            E("{ const int n_ = ");
            if (b) E("%d", b - 1); else E("nvar");
            E("; for (int i_ = 0; i_ < n_; i_++) R[%d + i_] = i_ < nvar ? base[%d + i_] : V_NIL;", a, p->numparams);
            if (b) E(" }\n"); else E(" top = %d + n_; }\n", a);
            break;
        case OP_EXTRAARG: E(";\n"); break;
        default:
            fprintf(stderr, "error: unsupported opcode %s in function %d\n", luaP_opnames[op], id);
            exit(1);
        }
    }
    E("}\n\n");
    free(target);
}

/* ------------------------------------------------------------------ main */
static void c_string(FILE *f, const char *s, size_t len) {
    fputc('"', f);
    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch >= 32 && ch < 127 && ch != '?') fputc(ch, f);
        else fprintf(f, "\\%03o", ch);
    }
    fputc('"', f);
}

static uint32_t p8c_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    while (len--) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int word_at(const char *text, const char *name, size_t len) {
    for (const char *p = text; (p = strstr(p, name)); p += len) {
        const int before = p == text || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        const int after = !(isalnum((unsigned char)p[len]) || p[len] == '_');
        if (before && after) return 1;
    }
    return 0;
}

/* femto8's Lua-written API (add, del, all, foreach, ...): the definitions
 * the cartridge reaches, directly or through each other, go in front of it. */
static void build_source(const char *cart_lua, char *dest, size_t size) {
    static char api[8192];
    strncpy(api, lua_api_string, sizeof api - 1);
    char *start[32], *stop[32];
    char name[32][24];
    int n = 0;
    for (char *p = api; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : 0) {
        if (strncmp(p, "function ", 9) || n == 32) continue;
        start[n] = p;
        size_t k = 0;
        for (const char *q = p + 9; *q && *q != '(' && k < 23; q++) name[n][k++] = *q;
        name[n][k] = 0;
        n++;
    }
    for (int i = 0; i < n; i++) stop[i] = i + 1 < n ? start[i + 1] : api + strlen(api);
    int need[32] = { 0 };
    for (int pass = 0, changed = 1; changed && pass < 8; pass++) {
        changed = 0;
        for (int i = 0; i < n; i++) {
            if (need[i]) continue;
            int native = 0;
            for (size_t k = 0; k < N_NATIVES; k++) native |= !strcmp(native_names[k], name[i]);
            /* coroutines and varargs helpers are not supported */
            if (native || !strcmp(name[i], "cocreate") || !strcmp(name[i], "coresume") || !strcmp(name[i], "costatus") || !strcmp(name[i], "yield") || !strcmp(name[i], "pack")) continue;
            int used = word_at(cart_lua, name[i], strlen(name[i]));
            for (int j = 0; j < n && !used; j++) {
                if (!need[j]) continue;
                const char tmp = *stop[j];
                *stop[j] = 0;
                used = word_at(start[j] + 9 + strlen(name[j]), name[i], strlen(name[i]));
                *stop[j] = tmp;
            }
            if (used) { need[i] = 1; changed = 1; }
        }
    }
    size_t len = 0;
    for (int i = 0; i < n; i++)
        if (need[i]) len += (size_t)snprintf(dest + len, size - len, "%.*s\n", (int)(stop[i] - start[i]), start[i]);
    snprintf(dest + len, size - len, "%s", cart_lua);
}

int p8c_main(int argc, char **argv) {
    const char *path = 0, *outdir = 0;
    int info = 0, dump_lua = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--info")) info = 1;
        else if (!strcmp(argv[i], "--lua")) dump_lua = 1;
        else path = argv[i];
    }
    if (!path || (!outdir && !info && !dump_lua)) { fprintf(stderr, "usage: p8c cart.p8|cart.p8.png (-o dir | --info)\n"); return 1; }
    m_file_buffer = malloc(128 * 1024);
    static uint8_t scratch[128 * 1024];
    static char cart_lua[128 * 1024];
    if (parse_cart_file(path, cart, scratch, scratch, cart_lua, NULL) != 0) return 1;
    build_source(cart_lua, lua_source, sizeof lua_source);
    if (dump_lua) { fputs(lua_source, stdout); return 0; }
    lua_State *L = luaL_newstate();
    if (luaL_loadbuffer(L, lua_source, strlen(lua_source), "=cart") != 0) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        return 1;
    }
    const Proto *main_proto = clLvalue(L->top - 1)->p;
    collect(main_proto);

    /* discover the globals and strings first: they are numbered by use */
    out = fopen("/dev/null", "w");
    for (int i = 0; i < n_protos; i++) emit_proto(protos[i], i);
    fclose(out);

    int warnings = 0;
    for (int i = 0; i < n_globals; i++) {
        const Global *g = &globals[i];
        if (g->get && !g->set && g->native < 0 && g->constant < 0) {
            fprintf(stderr, "warning: global '%s' is read but never defined (calling it stops the cartridge)\n", g->name);
            warnings++;
        }
    }
    if (info) {
        printf("functions %d, globals %d, strings %d, warnings %d\n", n_protos, n_globals, n_strs, warnings);
        return 0;
    }

    char file[1024];
    snprintf(file, sizeof file, "%s/p8_gen.c", outdir);
    out = fopen(file, "w");
    if (!out) { perror(file); return 1; }
    E("/* Generated by p8c from %s. */\n", path);
    /* the generated code is large and not the hot path: smaller code and, with it, smaller stack frames */
    E("#if defined(__GNUC__) && !defined(__clang__) && !defined(P8_GEN_SPEED)\n#pragma GCC optimize (\"Os\")\n#endif\n");
    E("#include \"p8rt.h\"\n#include <string.h>\n\n");
    E("#define P8_GEN_FUNCTIONS %d\n", n_protos);
    for (int i = 0; i < n_protos; i++) E("static int f%d(Val *base, int nargs, Clo *cl);\n", i);
    E("\n");
    for (int i = 0; i < n_protos; i++) if (up_count(protos[i]) == 0) E("static const Clo C%d = { f%d, { 0 } };\n", i, i);
    E("\n#define P8_NATIVE(name, fn) extern int fn(Val *base, int nargs, Clo *cl);\n");
    E("#include \"p8_natives.def\"\n#undef P8_NATIVE\n\n");
    for (int i = 0; i < n_strs; i++) {
        E("static const char T%d[] = ", i); c_string(out, strs[i].s, strs[i].len); E(";\n");
        E("static const Str S%d = { T%d, 0x%xu, %zu };\n", i, i, p8c_hash(strs[i].s, strs[i].len), strs[i].len);
    }
    E("\n");
    for (int i = 0; i < n_konsts; i++) {
        if (konsts[i].kind == 3) E("static const Val K%d = {{ .n = (fix32_t)%dL }, P8_NUM};\n", i, konsts[i].num);
        else E("static const Val K%d = {{ .s = &S%d }, P8_STR};\n", i, konsts[i].str);
    }
    E("\n");
    /* a global the cartridge never assigns and that is one of ours is a constant in the code */
    for (int i = 0; i < n_globals; i++) {
        const Global *g = &globals[i];
        if ((g->native >= 0 || g->constant >= 0) && !g->set) continue;
        if (g->native >= 0) E("static Val G%d = {{ .f = %s }, P8_CFN}; /* %s */\n", i, native_fns[g->native], g->name);
        else if (g->constant >= 0) E("static Val G%d = {{ .n = (fix32_t)0x%xu }, P8_NUM}; /* glyph */\n", i, const_globals[g->constant].raw);
        else E("static Val G%d; /* %s */\n", i, g->name);
    }
    E("\nvoid p8_gen_mark_roots(void (*mark)(const Val *v)) {\n");
    for (int i = 0; i < n_globals; i++) {
        const Global *g = &globals[i];
        if ((g->native >= 0 || g->constant >= 0) && !g->set) continue;
        if (g->native < 0 && g->constant < 0) E("    mark(&G%d);\n", i);
    }
    E("}\n\n");
    /* every global the cartridge can change, natives it reassigns included: save states */
    E("void p8_gen_state_globals(void (*visit)(Val *v)) {\n");
    for (int i = 0; i < n_globals; i++) {
        const Global *g = &globals[i];
        if ((g->native >= 0 || g->constant >= 0) && !g->set) continue;
        E("    visit(&G%d);\n", i);
    }
    E("}\n\n");
    for (int i = 0; i < n_protos; i++) emit_proto(protos[i], i);
    const char *cbs[] = { "_init", "_update", "_update60", "_draw" };
    E("static Val nil_cb;\n");
    for (int k = 0; k < 4; k++) {
        int found = -1;
        for (int i = 0; i < n_globals; i++) if (!strcmp(globals[i].name, cbs[k])) found = i;
        if (found >= 0) E("Val *const p8_cb%s = &G%d;\n", cbs[k], found);
        else E("Val *const p8_cb%s = &nil_cb;\n", cbs[k]);
    }
    E("\n/* runs the cartridge's main chunk: defines the globals and the callbacks */\nvoid p8_gen_run_main(void) { f0(p8_stack, 0, 0); }\n");
    fclose(out);

    snprintf(file, sizeof file, "%s/p8_cart.c", outdir);
    out = fopen(file, "w");
    if (!out) { perror(file); return 1; }
    E("/* Generated by p8c: the cartridge's sprites, map, flags, music and sound effects. */\n#include \"p8gfx.h\"\n\n");
    E("const uint8_t p8_cart[P8_CART_BYTES] P8_ROM = {");
    for (int i = 0; i < CART_BYTES; i++) E("%s%u,", i % 32 ? "" : "\n", cart[i]);
    E("\n};\n");
    fclose(out);
    snprintf(file, sizeof file, "%s/p8_gen_config.h", outdir);
    out = fopen(file, "w");
    if (!out) { perror(file); return 1; }
    E("/* Generated by p8c. */\n#pragma once\n");
    if (!uses_pow) E("#define P8_NO_POW 1 /* no ^ operator: the floating-point pow stays out */\n");
    fclose(out);
    fprintf(stderr, "p8c: %d functions, %d globals, %d strings\n", n_protos, n_globals, n_strs);
    return 0;
}
