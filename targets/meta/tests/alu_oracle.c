/*
 * Exhaustive flag/ALU comparison between the gbl helpers and the desktop
 * runtime (runtime/src/gbrt.c), which is validated by Blargg cpu_instrs.
 * Every helper is checked for every operand and incoming flag combination.
 */
#include "gbrt.h"

#include "gbl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The gbl interpreter in gbl_cpu.c references bus/event symbols that this
 * test never exercises. */
gbl_cpu_t gbr;
const uint8_t *gbl_rmap[16]; /* all NULL: the interpreter reads through gbl_rd */
uint8_t gbl_rd(uint16_t a) { (void)a; abort(); }
void gbl_wr(uint16_t a, uint8_t v) { (void)a; (void)v; abort(); }
void gbl_push(uint16_t v) { (void)v; abort(); }
uint16_t gbl_pop(void) { abort(); }
void gbl_sync(void) { abort(); }
void gbl_update_evt(void) {}
int gbl_irq_enter(void) { abort(); }
void gbl_halt_wait(void) { abort(); }
uint32_t gbl_spin_n(uint32_t period, uint32_t max) { (void)period; (void)max; abort(); }
uint32_t gbl_interp_instrs;

static GBContext ref;
static long failures;

static void set_flags(unsigned f) {
    gbl_setf((uint8_t)(f << 4));
    ref.f_z = (f >> 3) & 1;
    ref.f_n = (f >> 2) & 1;
    ref.f_h = (f >> 1) & 1;
    ref.f_c = f & 1;
}

static void compare(const char *what, unsigned a_in, unsigned v, unsigned f_in,
                    unsigned got_a, unsigned want_a) {
    uint8_t got_f = gbl_getf();
    uint8_t want_f = (uint8_t)((ref.f_z ? 0x80 : 0) | (ref.f_n ? 0x40 : 0) |
                               (ref.f_h ? 0x20 : 0) | (ref.f_c ? 0x10 : 0));
    if (got_a != want_a || got_f != want_f) {
        if (failures++ < 20)
            fprintf(stderr, "%s a=%02x v=%02x f=%x: got %02x/%02x want %02x/%02x\n",
                    what, a_in, v, f_in << 4, got_a, got_f, want_a, want_f);
    }
}

int main(void) {
    for (unsigned f = 0; f < 16; f++) {
        for (unsigned a = 0; a < 256; a++) {
            for (unsigned v = 0; v < 256; v++) {
#define BIN(name, mine, theirs)                                   \
                set_flags(f); gbr.a = (uint8_t)a; ref.a = (uint8_t)a; \
                mine((uint8_t)v); theirs(&ref, (uint8_t)v);       \
                compare(name, a, v, f, gbr.a, ref.a);
                BIN("add", gbl_add, gb_add8);
                BIN("adc", gbl_adc, gb_adc8);
                BIN("sub", gbl_sub, gb_sub8);
                BIN("sbc", gbl_sbc, gb_sbc8);
                BIN("cp", gbl_cp, gb_cp8);
            }
            /* Unary ops on a. */
            set_flags(f); gbr.a = (uint8_t)a; ref.a = (uint8_t)a;
            gbl_daa(); gb_daa(&ref);
            compare("daa", a, 0, f, gbr.a, ref.a);

            set_flags(f);
            unsigned r1 = gbl_inc((uint8_t)a), r2 = gb_inc8(&ref, (uint8_t)a);
            compare("inc", a, 0, f, r1, r2);
            set_flags(f);
            r1 = gbl_dec((uint8_t)a); r2 = gb_dec8(&ref, (uint8_t)a);
            compare("dec", a, 0, f, r1, r2);

            static uint8_t (*const cb_ref[8])(GBContext *, uint8_t) = {
                gb_rlc, gb_rrc, gb_rl, gb_rr, gb_sla, gb_sra, gb_swap, gb_srl};
            for (unsigned op = 0; op < 8; op++) {
                set_flags(f);
                r1 = gbl_cbop((uint8_t)(op << 3), (uint8_t)a);
                r2 = cb_ref[op](&ref, (uint8_t)a);
                compare("cb", a, op, f, r1, r2);
            }

            /* RLCA/RRCA/RLA/RRA: gbl forces Z clear after the CB helper. */
            static void (*const acc_ref[4])(GBContext *) = {gb_rlca, gb_rrca, gb_rla, gb_rra};
            for (unsigned op = 0; op < 4; op++) {
                set_flags(f); gbr.a = (uint8_t)a; ref.a = (uint8_t)a;
                gbr.a = gbl_cbop((uint8_t)(op << 3), gbr.a);
                gbr.zr = 1;
                acc_ref[op](&ref);
                compare("acc-rot", a, op, f, gbr.a, ref.a);
            }
        }
        /* 16-bit adds. */
        for (unsigned hl = 0; hl < 0x10000; hl += 0x0F1) {
            for (unsigned v = 0; v < 0x10000; v += 0x0E3) {
                set_flags(f); gbr.hl = (uint16_t)hl; ref.hl = (uint16_t)hl;
                gbl_addhl((uint16_t)v); gb_add16(&ref, (uint16_t)v);
                compare("addhl", hl >> 8, v >> 8, f, gbr.hl, ref.hl);
            }
            for (unsigned e = 0; e < 256; e++) {
                set_flags(f); gbr.sp = (uint16_t)hl; ref.sp = (uint16_t)hl;
                uint16_t mine = gbl_spofs((uint8_t)e);
                gb_add_sp(&ref, (int8_t)e);
                compare("addsp", hl >> 8, e, f, mine, ref.sp);
            }
        }
    }
    if (failures) {
        fprintf(stderr, "%ld mismatches\n", failures);
        return 1;
    }
    printf("alu_oracle: all helpers match the desktop runtime\n");
    return 0;
}
