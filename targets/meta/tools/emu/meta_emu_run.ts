// Headless driver for aoneill01/gamebuino-emulator: runs a META .bin, dumps a
// 160x128 PPM screenshot every 10M instructions and, given the addresses of
// gbl_frame and gbl_skip_render (arm-none-eabi-nm firmware.elf), reports
// emulated DMG frames per 20M instructions (the emulator's notion of one
// second).
//
//   cd $GAMEBUINO_EMULATOR_DIR && ./node_modules/.bin/ts-node -T \
//     -O '{"module":"commonjs","target":"es2017"}' meta_emu_run.ts \
//     firmware.bin <million_steps> <out_prefix> [events] [frame_addr skip_addr]
//
// events: "<million>:<mask>,..." with mask bits 0 down 1 left 2 right 3 up
// 4 A 5 B 6 MENU 7 HOME (pressed = 1).
import * as fs from 'fs';
(global as any).document = { addEventListener: () => {} };
const EMU = (process.env.GAMEBUINO_EMULATOR_DIR || process.cwd()) + '/src/';
const { Atsamd21 } = require(EMU + 'atsamd21');
const { St7735 } = require(EMU + 'st7735');
const { Buttons } = require(EMU + 'buttons');

const [bin, stepsM, outPrefix, script, frameAddr, skipAddr] = process.argv.slice(2);
let skipped = 0, samples = 0, lastFrame = 0;
const cpu = new Atsamd21();
cpu.loadFlash(new Uint8Array(fs.readFileSync(bin)), 0x4000);
const fake = { getImageData: (_x: number, _y: number, w: number, h: number) => ({ data: new Uint8ClampedArray(w * h * 4) }), putImageData: () => {} };
const screen = new St7735(cpu.sercom4, cpu.portA, cpu.portB, fake as any);
screen.clear();
const buttons = new Buttons(cpu.sercom4, cpu.portA, cpu.portB);
// script: "step_million:mask,..." mask = pressed bits in emulator order (0 down,1 left,2 right,3 up,4 A,5 B,6 menu,7 home)
const events = (script || '').split(',').filter(Boolean).map(e => e.split(':').map(Number));
const total = Number(stepsM);
const t0 = Date.now();
function dump(name: string) {
  const d: Uint32Array = (screen as any)._data;
  const w = 320, h = 256, rgb = Buffer.alloc(160 * 128 * 3);
  for (let y = 0; y < 128; y++) for (let x = 0; x < 160; x++) {
    const c = d[(y * 2) * w + x * 2];
    rgb[(y * 160 + x) * 3] = c & 255; rgb[(y * 160 + x) * 3 + 1] = (c >> 8) & 255; rgb[(y * 160 + x) * 3 + 2] = (c >> 16) & 255;
  }
  fs.writeFileSync(name, Buffer.concat([Buffer.from(`P6\n160 128\n255\n`), rgb]));
}
for (let m = 0; m < total; m++) {
  for (const [at, mask] of events) if (at === m) buttons.customButtonData(0xff & ~mask);
  for (let i = 0; i < 1000000; i++) cpu.step();
  if (frameAddr) {
    const f = cpu.fetchWord(parseInt(frameAddr, 16));
    samples++; if (cpu.fetchByte(parseInt(skipAddr, 16))) skipped++;
    if ((m + 1) % 20 === 0) { console.log('@' + (m + 1) + 'M gb frames ' + f + ' (+' + (f - lastFrame) + ' per 20M instr = 1 s of META time), skipped samples ' + skipped + '/' + samples); lastFrame = f; skipped = 0; samples = 0; }
  }
  if ((m + 1) % 10 === 0) dump(`${outPrefix}_${m + 1}.ppm`);
}
const secs = (Date.now() - t0) / 1000;
console.log(`steps ${total}M in ${secs.toFixed(1)}s (${(total / secs).toFixed(1)} M/s), systick ms ~${(cpu.tickCount / 20000).toFixed(0)}`);
