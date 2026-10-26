#ifndef M0BOY_MD_YM2612_H
#define M0BOY_MD_YM2612_H
#include <stdint.h>

/* YM2612 operator; op index 0..3 is S1..S4 (registers +0, +8, +4, +12).
 * The prepared fields (rate, tl8, sl, ssg, dt_*, mul2, det_d) are derived
 * from the registers by md_ym_write, so the per-sample loop never re-reads
 * or re-computes them. */
typedef struct {
    uint32_t phase;
    uint16_t att;
    uint16_t tl8, sl;          /* total level << 3, sustain level */
    uint8_t rate[4];           /* envelope rate of each state */
    uint8_t am_on;             /* LFO AM enable (register 0x60 bit 7) */
    uint8_t state, key, ssg_invert;
    uint8_t ssg;               /* register 0x90 */
    uint8_t dt_idx, dt_neg;    /* detune index and negation bit */
    uint16_t mul2;             /* phase multiplier */
    uint8_t det_d;             /* detune amount at the current key code */
    int16_t next, out, prev;   /* S1 only: its outputs, for the feedback */
} MdFmOperator;
typedef struct {
    uint8_t addr[2], reg[2][256], fnum_latch, special_latch, status;
    uint16_t fnum[6], special_fnum[3];
    uint8_t block[6], special_block[3];
    MdFmOperator op[6][4];
    uint32_t env_counter, lfo_counter;
    uint16_t timer_a, timer_b;
    uint8_t timer_b_sub;
    uint8_t reg_key[6], csm_key;   /* key bits from register 0x28 and from CSM */
    uint64_t busy_until;           /* master clock */
} MdYm2612;

void md_ym_reset(MdYm2612 *y);
/* A data write to register `reg` of part `bank` (0: ports 0/1, 1: ports 2/3). */
void md_ym_write(MdYm2612 *y, unsigned bank, uint8_t reg, uint8_t value);
/* One sample (fM/144): the six channels through the 9-bit DAC, summed, in
 * 14-bit channel units. */
void md_ym_sample(MdYm2612 *y, int *left, int *right);
#endif
