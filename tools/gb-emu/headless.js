/* Pilote headless du gamebuino-emulator : charge un firmware.bin, exécute,
 * et rapporte la couleur dominante de l'écran ST7735 à intervalles réguliers. */
const fs = require('fs');
globalThis.document = { addEventListener: () => {}, removeEventListener: () => {}, getElementById: () => ({ appendChild: () => {} }) };
const { Atsamd21 } = require('./dist-cjs/atsamd21');
const { St7735 } = require('./dist-cjs/st7735');
const { Buttons } = require('./dist-cjs/buttons');
const { SdCard } = require('./dist-cjs/sdcard');

const W = 160, H = 128, SCALE = 2;
const fakeImageData = { data: new Uint8ClampedArray(W * SCALE * H * SCALE * 4) };
const fakeCtx = {
  getImageData: () => fakeImageData,
  putImageData: () => {},
  fillRect: () => {},
  clearRect: () => {},
};

const fw = fs.readFileSync(process.argv[2]);
const cpu = new Atsamd21();
cpu.loadFlash(new Uint8Array(fw.buffer, fw.byteOffset, fw.length), 0x4000);
const screen = new St7735(cpu.sercom4, cpu.portA, cpu.portB, fakeCtx);
const buttons = new Buttons(cpu.sercom4, cpu.portA, cpu.portB);
let sd = null;
if (process.argv[5]) {
    sd = new SdCard(cpu.portA, cpu.sercom4);
    const p5 = process.argv[5];
    if (fs.statSync(p5).isDirectory()) {
        const { buildFatImage } = require('./sddir.js');
        const { image, fileMap } = buildFatImage(p5);
        sd.loadImage(image);
        sd.setWriteListener((lba, data) => {
            const e = fileMap.find((m) => lba >= m.lba && (lba - m.lba) * 512 < m.bytes);
            if (e) {
                const fd = fs.openSync(e.path, 'r+');
                fs.writeSync(fd, data, 0, 512, (lba - e.lba) * 512);
                fs.closeSync(fd);
            }
        });
        console.log(`carte SD : dossier ${p5} (${fileMap.length} fichiers, .SAV réécrits sur place)`);
    } else {
        sd.loadImage(new Uint8Array(fs.readFileSync(p5)));
        console.log(`carte SD : image ${p5}`);
    }
}
cpu.reset(0x4000);

function report(label) {
  const u8 = screen._buf8;   // tampon interne du ST7735 (RGBA)
  if (!u8) { console.log(label + ': pas de tampon'); return; }
  const colors = new Map();
  for (let i = 0; i < u8.length; i += 4 * 53) { // échantillon
    const c = (u8[i] << 16) | (u8[i + 1] << 8) | u8[i + 2];
    colors.set(c, (colors.get(c) || 0) + 1);
  }
  const top = [...colors.entries()].sort((a, b) => b[1] - a[1]).slice(0, 3)
    .map(([c, n]) => `#${c.toString(16).padStart(6, '0')}×${n}`).join(' ');
  console.log(`${label}: tick=${cpu.tickCount} couleurs=[${top}]`);
}

const CHUNK = +(process.argv[3] || 8000000);
const BATCHES = +(process.argv[4] || 8);
try {
  for (let b = 1; b <= BATCHES; b++) {
    for (let i = 0; i < CHUNK; i++) cpu.step();
    report(`batch ${b}/${BATCHES}`);
  }
} catch (e) {
  console.log('CRASH émulateur:', e.message);
  report('état au crash');
}
