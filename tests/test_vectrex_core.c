/* the checks are asserts: kept in a release build */
#undef NDEBUG
#include "../targets/vectrex/runtime/vectrex.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
const uint8_t vectrex_cart[] = {0};
const unsigned vectrex_cart_size = 1;
static uint8_t bios[8192];
static unsigned strokes;
static int32_t last_x0, last_x1, last_y0, last_y1, longest;
void vx_draw(Vectrex *v, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
             unsigned z, unsigned cycles) {
  (void)v;
  (void)z;
  (void)cycles;
  strokes++;
  last_x0 = x0;
  last_y0 = y0;
  last_x1 = x1;
  last_y1 = y1;
  if (x1 - x0 > longest)
    longest = x1 - x0;
}
/* The run loop (m6809_run.c) against the reference (m6809_step) on random
 * code in the system ROM's place: random opcodes, addressing modes, stack
 * traffic and VIA accesses (timers, ramp, spins) included.  After each
 * instruction the CPU, the RAM and the VIA must be the same. */
static uint32_t rng = 12345;
static uint32_t next_random(void) {
  rng = rng * 1103515245u + 12345u;
  return rng >> 16;
}
static void lockstep(void) {
  static uint8_t code[8192];
  Vectrex *ref = calloc(1, sizeof *ref), *run = calloc(1, sizeof *run);
  assert(ref && run);
  unsigned steps = 0;
  for (unsigned seed = 0; seed < 200; seed++) {
    rng = seed * 7919u + 1u;
    for (unsigned i = 0; i < sizeof code; i++)
      code[i] = (uint8_t)next_random();
    /* the reset vector into the ROM, the RAM's vectors (SWI, IRQ) too */
    code[0x1ffe] = (uint8_t)(0xe0 + next_random() % 32);
    code[0x1fff] = (uint8_t)next_random();
    memset(ref, 0, sizeof *ref);
    memset(run, 0, sizeof *run);
    ref->bios = run->bios = code;
    vx_reset(ref);
    vx_reset(run);
    ref->fast_boot = run->fast_boot = 0;
    for (unsigned n = 0; n < 50000 && ref->cpu.illegal < 900; n++, steps++) {
      m6809_step(ref);
      m6809_run(run, run->cpu.cycles + 1);
      via_sync(ref, ref->cpu.cycles);
      via_sync(run, run->cpu.cycles);
      assert(memcmp(&ref->cpu, &run->cpu, sizeof ref->cpu) == 0);
      assert(memcmp(ref->ram, run->ram, sizeof ref->ram) == 0);
      assert(memcmp(&ref->via, &run->via, sizeof ref->via) == 0);
    }
  }
  assert(steps > 1000000);
  free(ref);
  free(run);
}
int main(void) {
  Vectrex *v = calloc(1, sizeof *v);
  assert(v);
  const uint8_t program[] = {
      0x86, 0x07,       /* LDA #7 */
      0x8b, 0x09,       /* ADDA #9 */
      0x81, 0x10,       /* CMPA #16 */
      0x26, 0x02,       /* BNE (must not branch) */
      0x8e, 0xc8, 0x00, /* LDX #C800 */
      0xa7, 0x84,       /* STA ,X */
      0xe6, 0x84,       /* LDB ,X */
      0x34, 0x04,       /* PSHS B */
      0x5f,             /* CLRB */
      0x35, 0x04,       /* PULS B */
      0x20, 0xfe        /* BRA * */
  };
  memcpy(bios + 0x1000, program, sizeof program);
  bios[0x1ffe] = 0xf0;
  bios[0x1fff] = 0x00;
  v->bios = bios;
  vx_reset(v);
  for (int i = 0; i < 10; i++)
    m6809_step(v);
  assert(v->cpu.illegal == 0);
  assert(v->ram[0] == 16 && v->cpu.a == 16 && v->cpu.b == 16);
  assert(v->cpu.x == 0xc800 && v->cpu.s == 0xcbea);
  assert(v->cpu.pc == 0xf014);
  /* the data sheet's cycles: 2+2+2+3+3+4+4+(5+1)+2+(5+1) for LDA..PULS */
  assert(v->cpu.cycles == 34);
  bios[0x1100] = 0xa6; /* LDA 1,PC */
  bios[0x1101] = 0x8c;
  bios[0x1102] = 0x01;
  bios[0x1104] = 0x5a;
  v->cpu.pc = 0xf100;
  m6809_step(v);
  assert(v->cpu.a == 0x5a && v->cpu.pc == 0xf103);
  /* DAA carries out of the high digit */
  v->cpu.a = 0x9a;
  v->cpu.cc = 0;
  bios[0x1110] = 0x19;
  v->cpu.pc = 0xf110;
  m6809_step(v);
  assert(v->cpu.a == 0x00 && (v->cpu.cc & 1));

  /* a stroke to the right: BLANK released, X slope, the ramp for 80 cycles */
  via_write(v, 1, 0x7f); /* intensity 127 */
  via_write(v, 0, 0x04); /* mux 2: Z */
  via_write(v, 0, 0x01);
  via_write(v, 12, 0xee); /* ZERO and BLANK released */
  via_write(v, 1, 0x00); /* DAC 0 */
  via_write(v, 0, 0x00); /* mux 0: Y sample-and-hold to 0 */
  via_write(v, 0, 0x01);
  via_write(v, 1, 0x7f); /* X slope, the ramp on (PB7 low) */
  strokes = 0;
  via_tick(v, 80);
  via_flush(v);
  assert(strokes == 1);
  assert(last_x0 == VX_CENTER && last_x1 == VX_CENTER + 80 * 127 * 4);
  assert(last_y0 == VX_CENTER && last_y1 == VX_CENTER);
  /* a T1 one-shot stops the ramp when it times out: the stroke ends there */
  via_write(v, 11, 0x80);
  via_write(v, 4, 20);
  via_write(v, 5, 0);
  strokes = 0;
  longest = 0;
  via_tick(v, 100);
  via_flush(v);
  /* 21 cycles of ramp (T1 = 20 times out on the 21st), then a lit dot */
  assert(strokes == 2 && longest == 21 * 127 * 4 && last_x1 == last_x0);
  assert(v->via.ifr & 0x40);

  v->via.ay[0] = 32;
  v->via.ay[7] = 0x3e;
  v->via.ay[8] = 15;
  int16_t samples[512];
  ay_render(v, samples, 512, 44100);
  int lo = 32767, hi = -32768, flips = 0;
  for (unsigned i = 0; i < 512; ++i) {
    if (samples[i] < lo)
      lo = samples[i];
    if (samples[i] > hi)
      hi = samples[i];
    flips += i && (samples[i] > 0) != (samples[i - 1] > 0);
  }
  assert(lo < 0 && hi > 0);
  /* period 32: 1.5 MHz / (16 x 32) = 2930 Hz, 68 flips in 512 samples */
  assert(flips >= 64 && flips <= 72);
  free(v);
  lockstep();
}
