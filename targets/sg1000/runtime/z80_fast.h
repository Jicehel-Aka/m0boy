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


/* DD/FD-prefixed instructions that use IX/IY as HL does (with a displacement
 * for the memory operand).  The prefix is a second refresh increment; the
 * T-states are the plain instruction's plus 4, as z80_execute adds them.
 * Anything else (undocumented IXH/IXL forms, DDCB/FDCB, prefix chains)
 * returns -1. */
static inline int z80_fast_index(z80_t *c, int *jumped, unsigned prefix) {
    uint16_t *ix = prefix == 0xdd ? &c->ix : &c->iy;
    const uint8_t op = Z80_FAST_FETCH((uint16_t)(c->pc + 1));
    const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    int cycles;
    unsigned length;
    const uint16_t pc1 = (uint16_t)(c->pc + 2);
    if (x == 0) {
        if (z == 1 && !q && p == 2) { /* LD IX,nn */
            *ix = (uint16_t)(Z80_FAST_FETCH(pc1) | (Z80_FAST_FETCH((uint16_t)(pc1 + 1)) << 8));
            length = 4; cycles = 14;
        } else if (z == 1 && q) { /* ADD IX,rr */
            const uint16_t a = *ix, b = p == 2 ? *ix : *z80_pair(c, p, 0);
            const uint32_t v = (uint32_t)a + b;
            *ix = (uint16_t)v;
            c->f = (uint8_t)((c->f & (Z80_S | Z80_Z | Z80_PV)) | ((v >> 8) & (Z80_Y | Z80_X)) |
                             (((a ^ b ^ v) & 0x1000) ? Z80_H : 0) | (v >> 16));
            length = 2; cycles = 15;
        } else if (z == 2 && (y == 4 || y == 5)) { /* LD (nn),IX / LD IX,(nn) */
            const uint16_t a = (uint16_t)(Z80_FAST_FETCH(pc1) | (Z80_FAST_FETCH((uint16_t)(pc1 + 1)) << 8));
            if (q) *ix = z80_word(c, a); else z80_store_word(c, a, *ix);
            length = 4; cycles = 20;
        } else if (z == 3 && p == 2) { /* INC/DEC IX */
            *ix = (uint16_t)(*ix + (q ? -1 : 1));
            length = 2; cycles = 10;
        } else if ((z == 4 || z == 5) && y == 6) { /* INC/DEC (IX+d) */
            const uint16_t a = (uint16_t)(*ix + (int8_t)Z80_FAST_FETCH(pc1));
            z80_wr(c, a, z80_incdec(c, z80_rd(c, a), z == 5));
            length = 3; cycles = 23;
        } else if (z == 6 && y == 6) { /* LD (IX+d),n */
            const uint16_t a = (uint16_t)(*ix + (int8_t)Z80_FAST_FETCH(pc1));
            z80_wr(c, a, Z80_FAST_FETCH((uint16_t)(pc1 + 1)));
            length = 4; cycles = 19;
        } else return -1;
    } else if (x == 1) {
        if (op == 0x76) return -1;
        if (z == 6 && y != 6) { /* LD r,(IX+d) */
            zf_set(c, y, z80_rd(c, (uint16_t)(*ix + (int8_t)Z80_FAST_FETCH(pc1))));
            length = 3; cycles = 19;
        } else if (y == 6 && z != 6) { /* LD (IX+d),r */
            z80_wr(c, (uint16_t)(*ix + (int8_t)Z80_FAST_FETCH(pc1)), zf_get(c, z));
            length = 3; cycles = 19;
        } else return -1;
    } else if (x == 2 && z == 6) { /* ALU A,(IX+d) */
        z80_alu(c, y, z80_rd(c, (uint16_t)(*ix + (int8_t)Z80_FAST_FETCH(pc1))));
        length = 3; cycles = 19;
    } else if (x == 3) {
        if (op == 0xe5) { z80_push(c, *ix); length = 2; cycles = 15; }          /* PUSH IX */
        else if (op == 0xe1) { *ix = z80_pop(c); length = 2; cycles = 14; }     /* POP IX */
        else if (op == 0xe9) { c->pc = *ix; *jumped = 1; length = 0; cycles = 8; } /* JP (IX) */
        else return -1;
    } else return -1;
    if (length) c->pc = (uint16_t)(c->pc + length);
    c->r = (uint8_t)((c->r & 128) | ((c->r + 1) & 127)); /* the prefix's own refresh; the caller adds the opcode's */
    return cycles;
}

/* T-states of the instruction, or -1 when it needs the generic decoder.
 * *jumped: the instruction transferred control (the interpreter ends its
 * block there, exactly as when the generic path changes pc).  On success
 * the caller owns the refresh increment, once, as z80_execute does. */
static inline int z80_fast(z80_t *c, int *jumped) {
    *jumped = 0;
    const uint8_t op = Z80_FAST_FETCH(c->pc);
    if (op == 0xdd || op == 0xfd) return z80_fast_index(c, jumped, op);
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
        if (z == 2 && (y == 4 || y == 5)) { /* LD (nn),HL / LD HL,(nn) */
            const uint16_t a = (uint16_t)(Z80_FAST_FETCH((uint16_t)(c->pc + 1)) |
                                          (Z80_FAST_FETCH((uint16_t)(c->pc + 2)) << 8));
            if (y & 1) c->hl = z80_word(c, a); else z80_store_word(c, a, c->hl);
            c->pc = (uint16_t)(c->pc + 3);
            return 16;
        }
        if (z == 7 && y < 4) { /* RLCA, RRCA, RLA, RRA */
            const uint8_t saved = c->f;
            c->a = z80_rotate(c, c->a, y);
            c->f = (uint8_t)((saved & (Z80_S | Z80_Z | Z80_PV)) | (c->a & (Z80_Y | Z80_X)) | (c->f & 1));
            c->pc = (uint16_t)(c->pc + 1);
            return 4;
        }
        if (z == 0 && y == 1) { /* EX AF,AF' */
            const uint16_t af = (uint16_t)((c->a << 8) | c->f);
            c->a = (uint8_t)(c->af2 >> 8); c->f = (uint8_t)c->af2; c->af2 = af;
            c->pc = (uint16_t)(c->pc + 1);
            return 4;
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
    if (x == 3) {
        const unsigned p = y >> 1, q = y & 1;
        switch (z) {
        case 0: /* RET cc */
            if (z80_condition(c, y)) { c->pc = z80_pop(c); *jumped = 1; return 11; }
            c->pc = (uint16_t)(c->pc + 1);
            return 5;
        case 1:
            if (!q) { /* POP rr */
                const uint16_t v = z80_pop(c);
                if (p == 3) { c->a = (uint8_t)(v >> 8); c->f = (uint8_t)v; }
                else *z80_pair(c, p, 0) = v;
                c->pc = (uint16_t)(c->pc + 1);
                return 10;
            }
            if (p == 0) { c->pc = z80_pop(c); *jumped = 1; return 10; } /* RET */
            if (p == 1) { /* EXX */
                uint16_t v = c->bc; c->bc = c->bc2; c->bc2 = v;
                v = c->de; c->de = c->de2; c->de2 = v;
                v = c->hl; c->hl = c->hl2; c->hl2 = v;
                c->pc = (uint16_t)(c->pc + 1);
                return 4;
            }
            if (p == 2) { c->pc = c->hl; *jumped = 1; return 4; } /* JP (HL) */
            c->sp = c->hl; /* LD SP,HL */
            c->pc = (uint16_t)(c->pc + 1);
            return 6;
        case 2: { /* JP cc,nn */
            const uint16_t nn = (uint16_t)(Z80_FAST_FETCH((uint16_t)(c->pc + 1)) | (Z80_FAST_FETCH((uint16_t)(c->pc + 2)) << 8));
            if (z80_condition(c, y)) { c->pc = nn; *jumped = 1; } else c->pc = (uint16_t)(c->pc + 3);
            return 10;
        }
        case 3:
            if (y == 0) { /* JP nn */
                c->pc = (uint16_t)(Z80_FAST_FETCH((uint16_t)(c->pc + 1)) | (Z80_FAST_FETCH((uint16_t)(c->pc + 2)) << 8));
                *jumped = 1;
                return 10;
            }
            if (y == 5) { const uint16_t v = c->de; c->de = c->hl; c->hl = v; c->pc = (uint16_t)(c->pc + 1); return 4; } /* EX DE,HL */
            if (y == 4) { /* EX (SP),HL */
                const uint16_t v = z80_word(c, c->sp);
                z80_store_word(c, c->sp, c->hl);
                c->hl = v;
                c->pc = (uint16_t)(c->pc + 1);
                return 19;
            }
            return -1; /* CB prefix, OUT/IN (n), DI, EI */
        case 4: { /* CALL cc,nn */
            const uint16_t nn = (uint16_t)(Z80_FAST_FETCH((uint16_t)(c->pc + 1)) | (Z80_FAST_FETCH((uint16_t)(c->pc + 2)) << 8));
            c->pc = (uint16_t)(c->pc + 3);
            if (z80_condition(c, y)) { z80_push(c, c->pc); c->pc = nn; *jumped = 1; return 17; }
            return 10;
        }
        case 5:
            if (!q) { /* PUSH rr */
                z80_push(c, p == 3 ? (uint16_t)((c->a << 8) | c->f) : *z80_pair(c, p, 0));
                c->pc = (uint16_t)(c->pc + 1);
                return 11;
            }
            if (p == 0) { /* CALL nn */
                const uint16_t nn = (uint16_t)(Z80_FAST_FETCH((uint16_t)(c->pc + 1)) | (Z80_FAST_FETCH((uint16_t)(c->pc + 2)) << 8));
                z80_push(c, (uint16_t)(c->pc + 3));
                c->pc = nn;
                *jumped = 1;
                return 17;
            }
            return -1; /* DD, ED, FD prefixes */
        case 6: /* ALU A,n */
            z80_alu(c, y, Z80_FAST_FETCH((uint16_t)(c->pc + 1)));
            c->pc = (uint16_t)(c->pc + 2);
            return 7;
        default: /* RST */
            z80_push(c, (uint16_t)(c->pc + 1));
            c->pc = (uint16_t)(y * 8);
            *jumped = 1;
            return 11;
        }
    }
    return -1;
}
#endif
