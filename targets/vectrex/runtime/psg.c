#include "vectrex.h"

/* AY-3-8912 at the Vectrex's 1.5 MHz clock, in integers (no FPU on the
 * consoles).  The generators count in steps of 8 clocks: a tone flips every
 * `period` steps (f = clock / 16 period), the noise register and the
 * envelope move every 2 x period steps.  One frame of samples is rendered
 * after the CPU frame, with that frame's last registers. */
#define AY_STEP_RATE (VX_CLOCK / 8u)
static const int16_t volume[16] = {0,   36,  51,  72,   102,  144,  204,  289,
                                   408, 578, 817, 1155, 1633, 2309, 3265, 4617};

static void advance_envelope(AySynth *s, uint8_t shape) {
  if (s->env_hold)
    return;
  int next = (int)s->env_level + s->env_dir;
  if (next >= 0 && next <= 15) {
    s->env_level = (uint8_t)next;
    return;
  }
  if (!(shape & 8)) {
    s->env_level = 0;
    s->env_hold = 1;
  } else if (shape & 1) {
    s->env_level =
        (shape & 2) ? (s->env_dir > 0 ? 0 : 15) : (s->env_dir > 0 ? 15 : 0);
    s->env_hold = 1;
  } else {
    if (shape & 2)
      s->env_dir = (int8_t)-s->env_dir;
    s->env_level = s->env_dir > 0 ? 0 : 15;
  }
}

void ay_render(Vectrex *v, int16_t *out, unsigned count, unsigned rate) {
  AySynth *s = &v->sound;
  const uint8_t *r = v->via.ay;
  if (!rate)
    return;
  if (s->last_epoch != v->via.ay_epoch) {
    s->last_epoch = v->via.ay_epoch;
    s->env_dir = (r[13] & 4) ? 1 : -1;
    s->env_level = s->env_dir > 0 ? 0 : 15;
    s->env_hold = 0;
    s->env_count = 0;
  }
  const unsigned whole = AY_STEP_RATE / rate, part = AY_STEP_RATE % rate;
  unsigned period[3];
  for (unsigned ch = 0; ch < 3; ++ch) {
    period[ch] = r[ch * 2] | ((unsigned)(r[ch * 2 + 1] & 15) << 8);
    if (!period[ch])
      period[ch] = 1;
  }
  unsigned noise_period = (r[6] & 31) * 2u;
  unsigned env_period = (r[11] | (unsigned)r[12] << 8) * 2u;
  if (!noise_period)
    noise_period = 2;
  if (!env_period)
    env_period = 2;
  /* a shorter period than the count so far: restart the count */
  for (unsigned ch = 0; ch < 3; ++ch)
    if (s->tone_count[ch] >= period[ch])
      s->tone_count[ch] = 0;
  if (s->noise_count >= noise_period)
    s->noise_count = 0;
  if (s->env_count >= env_period)
    s->env_count = 0;
  /* only the channels heard tick sample by sample; the others' tone
   * counters jump once at the end, and the noise register only moves while
   * a heard channel takes it */
  int level[3], heard[3], noise_heard = 0;
  for (unsigned ch = 0; ch < 3; ++ch) {
    level[ch] = (r[8 + ch] & 16) ? -1 : volume[r[8 + ch] & 15];
    heard[ch] = level[ch] != 0;
    noise_heard |= heard[ch] && !(r[7] & (8 << ch));
  }
  /* The generators move at known step counts: `due` is the total at which
   * each one moves next, and between two such moments every sample is the
   * same mix.  Same counts and same output as stepping each sample. */
  uint32_t frac = s->step_frac, total = 0;
  uint32_t due[5];
  /* a tone the mixer leaves out does not change the output: its count
   * jumps at the end with the silent ones' */
  int moves[3];
  for (unsigned ch = 0; ch < 3; ++ch)
    moves[ch] = heard[ch] && !(r[7] & (1 << ch));
  const int active[5] = {moves[0], moves[1], moves[2], noise_heard, !s->env_hold};
  const uint32_t every[5] = {period[0], period[1], period[2], noise_period, env_period};
  const uint32_t count0[5] = {s->tone_count[0], s->tone_count[1], s->tone_count[2],
                              s->noise_count, s->env_count};
  uint32_t next = 0xffffffffu;
  for (unsigned g = 0; g < 5; g++) {
    due[g] = every[g] - count0[g];
    if (active[g] && due[g] < next)
      next = due[g];
  }
  int mix = 0, changed = 1;
  for (unsigned sample = 0; sample < count; ++sample) {
    total += whole;
    frac += part;
    if (frac >= rate) {
      frac -= rate;
      total++;
    }
    if (total >= next) {
      changed = 1;
      next = 0xffffffffu;
      for (unsigned g = 0; g < 5; g++) {
        if (!active[g])
          continue;
        while (due[g] <= total) {
          due[g] += every[g];
          if (g < 3)
            s->tone_level[g] ^= 1;
          else if (g == 3) {
            unsigned feedback = (s->lfsr ^ (s->lfsr >> 3)) & 1;
            s->lfsr = (s->lfsr >> 1) | (feedback << 16);
            s->noise_level = s->lfsr & 1;
          } else
            advance_envelope(s, r[13]);
        }
        if (due[g] < next)
          next = due[g];
      }
    }
    if (changed) {
      changed = 0;
      mix = 0;
      for (unsigned ch = 0; ch < 3; ++ch) {
        if (!heard[ch])
          continue;
        int tone = (r[7] & (1 << ch)) || s->tone_level[ch];
        int noise_on = (r[7] & (1 << (ch + 3))) || s->noise_level;
        int amp = level[ch] < 0 ? volume[s->env_level] : level[ch];
        mix += (tone && noise_on) ? amp : -amp;
      }
      mix *= 2;
    }
    out[sample] = (int16_t)mix;
  }
  /* the counts: how far past its last move each generator is */
  uint32_t tone0 = s->tone_count[0], tone1 = s->tone_count[1],
           tone2 = s->tone_count[2], noise = s->noise_count, env = s->env_count;
  uint32_t *const counts[5] = {&tone0, &tone1, &tone2, &noise, &env};
  for (unsigned g = 0; g < 5; g++)
    if (active[g])
      *counts[g] = every[g] - (due[g] - total);
  uint32_t *tone[3] = {&tone0, &tone1, &tone2};
  for (unsigned ch = 0; ch < 3; ++ch)
    if (!moves[ch]) {
      const uint32_t t = *tone[ch] + total;
      s->tone_level[ch] ^= (uint8_t)((t / period[ch]) & 1);
      *tone[ch] = t % period[ch];
    }
  s->step_frac = frac;
  s->tone_count[0] = tone0;
  s->tone_count[1] = tone1;
  s->tone_count[2] = tone2;
  s->noise_count = noise;
  s->env_count = env;
}
