#ifndef M0BOY_MD_YM2612_H
#define M0BOY_MD_YM2612_H
#include <stdint.h>

/* YM2612 operator; op index 0..3 is S1..S4 (registers +0, +8, +4, +12). */
typedef struct {
    uint32_t phase;
    uint16_t att;
    uint8_t state, key, ssg_invert;
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
