#include "vectrex.h"
#include <string.h>
uint8_t vx_read(Vectrex *v, uint16_t addr) {
  if (addr < 0x8000)
    return addr < vectrex_cart_size ? vectrex_cart[addr] : 0xff;
  if ((addr & 0xf800) == 0xc800)
    return v->ram[addr & 1023];
  if ((addr & 0xf800) == 0xd000)
    return via_read(v, addr & 15);
  if (addr >= 0xe000)
    return v->bios[addr & 8191];
  return 0xff;
}
void vx_write(Vectrex *v, uint16_t addr, uint8_t data) {
  if ((addr & 0xf800) == 0xc800)
    v->ram[addr & 1023] = data;
  else if ((addr & 0xf800) == 0xd000)
    via_write(v, addr & 15, data);
}
uint16_t vx_read16(Vectrex *v, uint16_t addr) {
  return (uint16_t)(vx_read(v, addr) << 8 | vx_read(v, addr + 1));
}
void vx_reset(Vectrex *v) {
  memset(&v->cpu, 0, sizeof v->cpu);
  memset(&v->via, 0, sizeof v->via);
  memset(&v->sound, 0, sizeof v->sound);
  v->sound.lfsr = 1;
  /* The BIOS boot animation reads its countdown before writing every RAM
     byte. A deterministic power-on pattern avoids an endless splash loop. */
  for (unsigned i = 0; i < sizeof v->ram; i++)
    v->ram[i] = (uint8_t)i;
  for (unsigned page = 0; page < 64; page++) {
    const unsigned at = page << 10;
    v->rmap[page] = 0;
    v->wmap[page] = 0;
    if (at + 1024 <= vectrex_cart_size)
      v->rmap[page] = vectrex_cart + at;
    else if (at >= 0xc800 && at < 0xd000) {
      v->rmap[page] = v->ram;
      v->wmap[page] = v->ram;
    } else if (at >= 0xe000)
      v->rmap[page] = v->bios + (at - 0xe000);
  }
  v->cpu.cc = 0x50;
  v->cpu.s = 0xcbea;
  v->cpu.pc = vx_read16(v, 0xfffe);
  v->frame_end = 0;
  v->via.beam_x = v->via.beam_y = VX_CENTER;
  v->via.pen_x = v->via.pen_y = VX_CENTER;
  v->via.xsh = v->via.ysh = v->via.rsh = 0x80;
  v->via.ay[14] = 0xff;
  v->via.sr_bits = 8;
  via_derive(v);
  v->joy_x = v->joy_y = 128;
  /* Fast path only for the standard Executive whose loop addresses below
     are known. Other BIOS revisions run their own complete boot path. */
  v->fast_boot = v->bios[0x101c] == 0xbd && v->bios[0x10da] == 0xb6;
  v->attention = 1;
}
/* the standard Executive's boot, shortened: called before each instruction
 * while fast_boot is set */
void vx_fast_boot_hook(Vectrex *v) {
  if (v->cpu.pc == 0xf01c)
    v->cpu.pc = 0xf06c;
  /* The BIOS intro waits for its music engine to clear C856. Until
     sound timing is complete, end that intro at its normal jump site. */
  if (v->cpu.pc == 0xf0da && v->ram[0x37] == 0 && v->ram[0x38] < 0x80) {
    v->ram[0x56] = 0;
    v->ram[0x25] = 0;
    v->ram[0x26] = 0x7e;
    v->fast_boot = 0;
  }
}
void vx_run_until(Vectrex *v, uint32_t stop) {
#ifdef VX_REFERENCE_CPU
  while ((int32_t)(v->cpu.cycles - stop) < 0 && v->cpu.illegal < 1000) {
    if (v->fast_boot)
      vx_fast_boot_hook(v);
    m6809_step(v);
  }
#else
  m6809_run(v, stop);
#endif
  via_sync(v, v->cpu.cycles);
}
void vx_end_frame(Vectrex *v) {
  via_sync(v, v->cpu.cycles);
  via_flush(v);
  v->frame_end += VX_FRAME_CYCLES;
}
void vx_frame(Vectrex *v) {
  v->via.segments = 0;
  vx_run_until(v, v->frame_end + VX_FRAME_CYCLES);
  vx_end_frame(v);
}
