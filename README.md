# m0boy

**Game Boy, SG-1000, Game Gear, Master System and PICO-8 games compiled into
native firmwares for three small handhelds: the Gamebuino META, the Pokitto
and the ESPboy.**

*Version française : [README.fr.md](README.fr.md).*

m0boy is not an emulator. It translates a game's own code into C, compiles
that C for the handheld's processor and links it with a compact runtime that
stands in for the original hardware (video, sound, timers, mappers). Code the
recompiler could not prove or never saw running falls back to a small
interpreter, so every game boots even when only part of it is compiled.

The result of a conversion is a folder to copy onto the console's SD card (or,
on the ESPboy, an image to flash).

| | Gamebuino META | Pokitto | ESPboy |
|---|---|---|---|
| Chip | SAMD21, Cortex-M0+ rated 48 MHz, run at 70 by default | LPC11U68, Cortex-M0+ rated 50 MHz, run at 72 by default | ESP8266, Xtensa 80/160 MHz |
| Memory | 32 KiB RAM, 240 KiB flash for the game | 36 KiB RAM, 228 KiB flash | 80 KiB RAM, 4 MiB flash |
| Screen | 160×128 | 220×176 | 128×128 |
| Storage | SD card | SD card | flash only |
| Runtime version | 0.10.34 | 0.10.28 | 0.3.4 |

## What runs where

| Source | META | Pokitto | ESPboy |
|---|---|---|---|
| Game Boy (DMG) | yes | yes | yes |
| SG-1000 | yes | yes | yes |
| Game Gear | yes | yes | yes |
| Master System | yes | yes | yes |
| PICO-8 | yes | — | yes |

Every core has the same option menu (hold HOME; C on the Pokitto): resume,
restart, **10 save-state slots**, volume, speed, CPU clock, return to the
loader, plus each system's own lines (display modes and palettes on the
Game Boy, USB bootloader on the META).

| Feature | META | Pokitto | ESPboy |
|---|---|---|---|
| Save states | every core, file on the card | every core but PICO-8, file on the card | every core, in flash |
| Battery saves | `.SAV` on the card | `.SAV` on the card | in flash, two alternating copies |
| ROM larger than the flash | read from the card, hot sectors kept in flash | read from the card | `rom.bin` in the 4 MiB flash |
| Compressed ROM sectors (LZSS) | yes | yes | yes |
| ROM cache sharing RAM with the C stack | yes | Game Boy | — (no card) |

How much of this was checked, and how, is written next to each claim below and
in the linked notes: **checked** means an automatic comparison or test,
**gm0** means measured in the [gm0](https://github.com/r043v/gm0) emulator,
**device** means measured on a console, **estimate** means the Cortex-M0+
cycle model (`targets/meta/tools/m0bench`).

## Getting started

You need CMake 3.20+, Ninja, a host C/C++ compiler, Python 3, the zlib
development files, and [PlatformIO](https://platformio.org/) for the META and
the ESPboy. The Pokitto only needs `arm-none-eabi-gcc` (from `PATH`, otherwise
PlatformIO's). PICO-8 support needs a [femto8](https://github.com/benbaker76/femto8)
checkout next to this one (`FEMTO8_DIR`, default `../femto8`).

```bash
cmake -G Ninja -B build .
ninja -C build

# ROMs in roms/gb/, roms/sg/, roms/gg/, roms/sms/, roms/pico8/ then:
./compile.sh                    # asks for the console (META by default)
TARGET=pokitto ./compile.sh
TARGET=espboy ./compile.sh
TARGET=both ./compile.sh        # META and Pokitto
```

Each game lands in `roms/<console>/<system>/<Game>.zip`. Unzip it at the root
of the SD card and start it from the console's loader; keep the `.SAV`,
`.STA` or `.ST8` files that are already on the card, they hold your progress.
On the ESPboy, the zip holds `firmware.bin` (and `rom.bin` above 256 KiB) with
the `esptool.py` line in `FLASH.txt`.

Useful `compile.sh` switches:

| Switch | Effect |
|---|---|
| `RELEASE=1` | firmwares without the USB debug port (more RAM; reflash through the bootloader) |
| `UPDATE=1`, `FORCE=1` | rebuild games made by another version / rebuild everything |
| `SOURCE=1` | deliver the generated project, ready for `pio run`, instead of the firmware |
| `SRC_SYSTEM=gb\|sg\|gg\|sms\|pico8` | convert one system only |
| `PACK=0` | no compressed ROM sectors (Game Boy) |
| `CACHE_DYN=0` | fixed-size card cache instead of the adaptive one |
| `FIRMWARE_COMPILER=clang` | build the firmwares with Clang |
| `REPLAY=1`, `PLAY=1` | replay your recorded sessions / play one yourself before compiling |

The complete guide is in French: [docs/GUIDE.fr.md](docs/GUIDE.fr.md). The
technical notes are in [targets/meta/README.md](targets/meta/README.md).

Tests:

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

## How it works

### Compilation guided by play

Before compiling a game, `compile.sh` plays it by itself on the host: random
sessions, a session without input (logos, attract mode), and coverage-guided
explorations that keep a snapshot of every state where new code or data showed
up and play on from there. The sessions record what code ran, how often, and
which ROM sectors were read. They are kept per ROM (`profiles/<sha1>/`), so a
later conversion reuses them and a session you play yourself extends them.

A static sweep completes them. It classifies every byte of the ROM (padding,
repeated patterns, pointer tables), then tries speculative runs of
instructions; a run counts as code only when it reaches an unconditional end
without an illegal opcode or an access to nonexistent hardware. The flash is
then filled with compiled code, most executed first, then the swept code the
sessions never reached.

The generated C keeps the source CPU's semantics: flags are computed only
where a later instruction reads them, cycles are added per straight run,
wait loops (polling a register or an interrupt flag) skip whole iterations up
to the next event. A precise build (`--meta-precise`) synchronises the
hardware before every instruction and is **checked** identical to the
interpreter: Super Mario Land on the Game Boy, all 68 SG-1000 cartridges and
all 225 Game Gear cartridges of a test collection.

### Any ROM size

- A ROM that fits stays in flash next to the runtime and the compiled code.
- Otherwise the least-read 512-byte sectors are compressed with LZSS in flash
  and decoded on demand by a Thumb-1 decoder; the hot ones stay raw.
- When that is still too large, the ROM is read from the SD card through a
  small cache, the sectors the sessions read most kept in flash. There is no
  size limit: Link's Awakening, Pokémon Yellow and Donkey Kong Land convert
  this way.
- Cartridge RAM larger than the console can spare (8 KiB, 32 KiB) lives in a
  `.SAV` file on the card behind a write-back cache.

On the META the card cache shares the free RAM with the C stack at run time:
a fixed part (the one the menu uses) plus blocks taken from the RAM under the
stack. A native call refused for lack of stack gives a block back at once; a
block is taken again only after several seconds without pressure and with a
margin. With 64-byte blocks, Super Mario Land 2's demo ran 13,326 frames in
150 s against 10,985 with the former fixed cache of 128-byte blocks
(**device**), and Ristar's card reads dropped from 9,001 to 4,131 (**device**).

### Runtime

- One cycle counter drives everything; the video renders a line when the
  program can observe it, timers and interrupts raise events only when they
  become visible.
- The hot paths are Thumb-1 assembly: the Game Boy PPU line state machine and
  renderer, memory access, the interpreter, the dispatch cache, the Game Gear
  background, the SG-1000 tile loops, the LZSS decoder.
- The Cortex-M0+ has no divider: divisions were taken out of the hot paths.
- The META and Pokitto firmwares are bare metal (no Arduino core, no
  PokittoLib); the ESPboy starts on the Arduino core and drives the hardware
  itself.

### Game Boy

DMG games, mappers none, MBC1, MBC2, MBC3 (no clock) and MBC5. On the META
the 144 lines are blended into 128 (several modes, the default keeps text
legible); on the Pokitto the picture is 1:1 or enlarged to 200×176. The Game
Boy Color boot palettes are built in (hold a direction at boot, as on a GBC).

### SG-1000, Game Gear, Master System

The Z80 recompiler handles banked code (an instruction is a page and an
address; code returns to the dispatcher when the mapper moves its page), Sega
and Codemasters mappers. The TMS9918A and the mode 4 VDP are drawn line by
line when the program touches them; the SG-1000 and Master System pictures are
filtered 8-to-5 onto the small screens. The Z80 core passes ZEXDOC and 66 of
67 ZEXALL tests (**checked**). In gm0 at 48 MHz, 67 of 68 SG-1000 games run at
full speed on the META and the Pokitto, and Alex Kidd in Miracle World,
Shinobi and Wonder Boy III at 97–99 % (**gm0**). No FM sound.

### PICO-8

A cartridge's Lua is compiled to C, one function per Lua function, on a
runtime with a garbage-collected heap and integer drawing and sound. The PC
side uses femto8's cartridge reader and Lua compiler. The picture of the
generated code is **checked** equal to femto8's, byte for byte, on a synthetic
cartridge (also with a collection at every allocation, and across a save
state) and on Celeste and its Practice Mod. Save states hold the heap's absolute
pointers, so a state only loads into the firmware that wrote it, recognised
by a hash of its code. Notes: [docs/PICO8.fr.md](docs/PICO8.fr.md).

```bash
build/bin/m0recomp celeste.p8.png --system pico8 --target meta -o out/celeste
pio run -d out/celeste
```

### ESPboy

ESP8266 (ESP-12F, 4 MiB flash), 128×128 screen, no SD card. A ROM of up to
256 KiB is part of the firmware; a larger one keeps its first bank there and
the rest goes into `rom.bin`, flashed at `0x100000`. Battery saves (two
alternating copies) and save states live in the flash from `0x3C0000`. The
emulation runs on a 12 KiB stack in RAM, which gives back what the heap needs
to keep 4 KiB free. Checked by host tests (a mock flash included), Xtensa
builds and gm0 runs; the current version has not been validated on an ESPboy.
Status: [docs/ESPBOY_STATUS_2026-10-19.md](docs/ESPBOY_STATUS_2026-10-19.md).

## Without a console

[gm0](https://github.com/r043v/gm0) runs these firmwares: the META and the
Pokitto instruction by instruction, the ESPboy's own Xtensa code with its ROM
and SDK emulated by function. `gm0 Game.zip` takes a converted game as an SD
card; `EMU_INPUT` scripts buttons (`frame:keys:frames,...`).

## Limitations

- Game Boy Color-only games are rejected; CGB features are not emulated.
- Simplified Game Boy timing: mode 3 always lasts 172 dots, OAM DMA is
  instant, sound is quantised to a frame, no link cable.
- Code the sessions never reached runs in the interpreter, much slower: a
  session you record through that part fixes it.
- SG-1000: unbanked cartridges up to 48 KiB, NTSC, no SC-3000 keyboard.
  Master System: no FM sound. ESPboy: Game Gear cartridge RAM is 8 KiB.
- PICO-8: no Pokitto target yet; cartridges must fit the heap (18 KiB on the
  META, 22 KiB on the ESPboy).
- Frames are skipped when the console falls behind.
- Performance figures marked **estimate** or **gm0** are not device
  measurements.

## Repository layout

| Path | Content |
|---|---|
| `recompiler/` | `m0recomp` (also `gbrecomp`): Game Boy backend, Z80 recompiler (`src/system/sg1000/`), PICO-8 front end (`src/system/pico8/`), ROM storage planning |
| `targets/meta/runtime/` | the Game Boy runtime shared by the three consoles; per-console switches in `gbl_profile.h` |
| `targets/meta/platform/` | shared frontend (menu, saves, states, SD card, FAT, display) and each console's drivers |
| `targets/sg1000/`, `targets/gamegear/` | SG-1000 and Game Gear/Master System runtimes, frontends, host harnesses |
| `targets/pico8/` | PICO-8 runtime, frontend, host harness |
| `targets/common/` | ROM codecs, card cache, stack/cache pool, play explorer, Sega frontend pieces |
| `targets/meta/host/`, `targets/meta/tools/` | host harness, cycle estimator |
| `compile.sh`, `tools/meta-compiler/` | batch conversion (shell, Node) |
| `tests/`, `targets/meta/tests/` | tests |
| `docs/` | French guide, development notes |

## Written by AI agents

No line of what m0boy adds to its starting point was written by hand. The
code, tests, drivers and documentation were written by AI agents guided by
r043v, who tested on the consoles and made the choices. Claude (Claude Code)
wrote most of it; GLM (ZCode) wrote the web converter, the GBC palettes,
Pokitto fixes and the gm0 emulator; DeepSeek (OpenCode) worked on the buses
and merged the Sega backends' shared code. Figures for the first ten days
(29 September – 9 October 2026), from the transcripts and the git history:
Claude, 67 commits and 42,375 added lines (95 %); GLM, 26 commits and 2,207
lines (5 %).

m0boy started as a port of
[gb-recompiled](https://github.com/arcanite24/gb-recompiled) by arcanite24, a
Game Boy recompiler for desktop computers, and keeps its history (104
commits). The desktop runtime, ports and test folders that held converted
commercial ROMs were removed; the emulator m0boy grew alongside is now
[gm0](https://github.com/r043v/gm0).

## ROMs and rights

No ROM and no built firmware are provided, and none may be added to this
repository. Convert only games you own, or homebrews whose licence allows it.

## Licence

MIT, see [LICENSE](LICENSE). Based on
[gb-recompiled](https://github.com/arcanite24/gb-recompiled), © 2026
arcanite24, MIT licence. `tools/gb-emu` keeps its own MIT licence (© 2017
Andy O'Neill).
