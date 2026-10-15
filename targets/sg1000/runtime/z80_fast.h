/* Fast path of the interpreter's common instructions, shared by the
 * SG-1000 and Game Gear cores: profiling (gm0, EMU_PROF) shows the
 * interpreted remainder runs a few short patterns -- LD A,(HL); OR A;
 * JR cc -- so the hot opcodes' families (LD r,r', LD r,n, ALU A,r,
 * INC/DEC, JR, DJNZ) skip the generic decoder and its switch machinery.
 * Everything else returns -1 and keeps the reference path; semantics,
 * cycles and the refresh counter are exactly the interpreter's
 * (z80_ops.h, z80.c).
 *
 * The including core defines Z80_FAST_FETCH(addr): its mapped opcode
 * fetch, never traced as a data read. */
#ifndef M0_Z80_FAST_H
#define M0_Z80_FAST_H
/* z80_ops.h must be included first: the helpers below are its semantics. */

/* Register access of the unprefixed encodings (z80_get/z80_set without a
 * decoded instruction); r 6 is the (HL) memory operand. */
static inline uint8_t zf_get(z80_t *c, unsigned r) {
    switch (r) {
    case 0: return c->b; case 1: return c->c; case 2: return c->d; case 3: return c->e;
    case 4: return c->h; case 5: return c->l; case 6: return z80_rd(c, c->hl); default: return c->a;
    }
}
static inline void zf_set(z80_t *c, unsigned r, uint8_t v) {
    switch (r) {
    case 0: c->b = v; break; case 1: c->c = v; break; case 2: c->d = v; break; case 3: c->e = v; break;
    case 4: c->h = v; break; case 5: c->l = v; break; case 6: z80_wr(c, c->hl, v); break; default: c->a = v; break;
    }
}

/* T-states of the instruction, or -1 when it needs the generic decoder.
 * *jumped: the instruction transferred control (the interpreter ends its
 * block there, exactly as when the generic path changes pc).  On success
 * the caller owns the refresh increment, once, as z80_execute does. */
static inline int z80_fast(z80_t *c, int *jumped) {
    *jumped = 0;
    const uint8_t op = Z80_FAST_FETCH(c->pc);
    const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (x == 1 && op != 0x76) { /* LD r,r': both sides in registers, or HL */
        if (z == 6) zf_set(c, y, z80_rd(c, c->hl));
        else if (y == 6) z80_wr(c, c->hl, zf_get(c, z));
        else zf_set(c, y, zf_get(c, z));
        c->pc = (uint16_t)(c->pc + 1);
        return z == 6 || y == 6 ? 7 : 4;
    }
    if (x == 0) {
        if (z == 6) { /* LD r,n / LD (HL),n */
            zf_set(c, y, Z80_FAST_FETCH((uint16_t)(c->pc + 1)));
            c->pc = (uint16_t)(c->pc + 2);
            return y == 6 ? 10 : 7;
        }
        if (z == 4 || z == 5) { /* INC/DEC r / (HL) */
            if (y == 6) z80_wr(c, c->hl, z80_incdec(c, z80_rd(c, c->hl), z == 5));
            else zf_set(c, y, z80_incdec(c, zf_get(c, y), z == 5));
            c->pc = (uint16_t)(c->pc + 1);
            return y == 6 ? 11 : 4;
        }
        if (z == 3) { /* INC/DEC rr */
            uint16_t *p = z80_pair(c, y >> 1, 0);
            *p = (uint16_t)(*p + (y & 1 ? -1 : 1));
            c->pc = (uint16_t)(c->pc + 1);
            return 6;
        }
        if (z == 1) { /* LD rr,nn / ADD HL,rr */
            uint16_t *p = z80_pair(c, y >> 1, 0);
            if (y & 1) {
                const uint16_t a = c->hl, b = *p;
                const uint32_t v = (uint32_t)a + b;
                c->hl = (uint16_t)v;
                c->f = (uint8_t)((c->f & (Z80_S | Z80_Z | Z80_PV)) | ((v >> 8) & (Z80_Y | Z80_X)) |
                                 (((a ^ b ^ v) & 0x1000) ? Z80_H : 0) | (v >> 16));
                c->pc = (uint16_t)(c->pc + 1);
                return 11;
            }
            *p = (uint16_t)(Z80_FAST_FETCH((uint16_t)(c->pc + 1)) |
                            (Z80_FAST_FETCH((uint16_t)(c->pc + 2)) << 8));
            c->pc = (uint16_t)(c->pc + 3);
            return 10;
        }
        if (z == 2 && y < 4) { /* LD A,(BC/DE) / LD (BC/DE),A */
            const uint16_t a = (y & 2) ? c->de : c->bc;
            if (y & 1) c->a = z80_rd(c, a); else z80_wr(c, a, c->a);
            c->pc = (uint16_t)(c->pc + 1);
            return 7;
        }
        if (z == 2 && y >= 6) { /* LD A,(nn) / LD (nn),A */
            const uint16_t a = (uint16_t)(Z80_FAST_FETCH((uint16_t)(c->pc + 1)) |
                                          (Z80_FAST_FETCH((uint16_t)(c->pc + 2)) << 8));
            if (y & 1) c->a = z80_rd(c, a); else z80_wr(c, a, c->a);
            c->pc = (uint16_t)(c->pc + 3);
            return 13;
        }
        if (z == 0 && (y == 0 || y >= 3)) { /* NOP / JR / JR cc */
            if (y == 0) { c->pc = (uint16_t)(c->pc + 1); return 4; }
            if (y == 3 || z80_condition(c, y - 4)) {
                c->pc = (uint16_t)(c->pc + 2 + (int8_t)Z80_FAST_FETCH((uint16_t)(c->pc + 1)));
                *jumped = 1;
                return 12;
            }
            c->pc = (uint16_t)(c->pc + 2);
            return 7;
        }
        if (z == 0 && y == 2) { /* DJNZ */
            const int8_t disp = (int8_t)Z80_FAST_FETCH((uint16_t)(c->pc + 1));
            c->pc = (uint16_t)(c->pc + 2);
            if (--c->b) {
                c->pc = (uint16_t)(c->pc + disp);
                *jumped = 1;
                return 13;
            }
            return 8;
        }
        return -1;
    }
    if (x == 2) { /* ALU A,r / A,(HL) */
        z80_alu(c, y, zf_get(c, z));
        c->pc = (uint16_t)(c->pc + 1);
        return z == 6 ? 7 : 4;
    }
    if (x == 3 && z == 6) { /* ALU A,n */
        z80_alu(c, y, Z80_FAST_FETCH((uint16_t)(c->pc + 1)));
        c->pc = (uint16_t)(c->pc + 2);
        return 7;
    }
    return -1;
}
#endif
