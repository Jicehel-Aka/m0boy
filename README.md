# m0boy

Source CPU, source console (`--system`) and destination (`--target`) now have
separate identities. SG-1000 cartridges (unbanked, up to 48 KiB) are recompiled
too: Z80 code becomes C regions for the META and the Pokitto, on a runtime with
lazy TMS9918A rendering and SN76489 sound; `compile.sh` handles `.sg` files. In
precise mode the recompiled code reproduces the Z80 interpreter exactly. See the
[multi-system architecture notes (French)](docs/MULTI_SYSTEM.fr.md).
SG-1000 uses the shared [gm0 `z80` branch](https://github.com/r043v/gm0/tree/z80) frontend: `gm0 game.sg` (arrows,
Space/Ctrl, Enter, `*`; F8 filters, F10 scaling, F11 fullscreen).
`tools/sg1000/package.py` builds Windows and Linux archives with SDL2 included;
see the [desktop distribution instructions](docs/MULTI_SYSTEM.fr.md#jouer-sur-pc).

**Game Boy games recompiled into native firmwares for two Cortex-M0+
handhelds: the Gamebuino META and the Pokitto.**

*Version française : [README.fr.md](README.fr.md).*

m0boy turns a Game Boy (DMG) ROM into a folder that you copy to the
console's SD card. The game's code is translated to C and compiled for the
console's processor: it is neither an emulator nor a JIT. Code seen during
automatic play sessions is recompiled, and a small interpreter runs the
rest. The PPU, APU, timers and MBC mappers are reimplemented in a compact
runtime whose hot paths are written in Thumb-1 assembly.

It started as a port of [gb-recompiled](https://github.com/arcanite24/gb-recompiled)
by arcanite24, a recompiler that targets desktop computers. m0boy keeps its
recompiler and adds everything needed to run on chips with 32 KiB of RAM,
256 KiB of flash and no hardware divider.

## Consoles

| | Gamebuino META | Pokitto |
|---|---|---|
| Chip | SAMD21, Cortex-M0+ at 48 MHz | LPC11U68, Cortex-M0+ at 48 MHz (72 MHz optional) |
| Memory | 32 KiB RAM, 240 KiB flash for the firmware | 32 + 2 + 2 KiB RAM, 228 KiB flash below the loader |
| Screen | 160×128: the 144 Game Boy lines blended into 128 | 220×176: picture 1:1, or enlarged to 200×176 |
| Firmware build | PlatformIO | `make` with arm-none-eabi-gcc |
| Version | 0.8.1 | 0.8.1 |
| Status | Games run on the console | Tested in an emulator only. A first tester on a real Pokitto reports an upside-down picture; the cause is being investigated |

## What was built

### Recompilation guided by play

- `compile.sh` plays each ROM by itself before compiling it: one
  random-input session per CPU core, 20 s each at full speed (3,000 to
  12,000 frames a second, so 15 to 60 minutes of play per session), plus
  any session you record yourself. Only the instructions these sessions
  executed are compiled, so data is never mistaken for code and the
  firmware fits in flash.
- Everything else, including code copied to RAM, runs in the `gbl`
  interpreter. Compiled code and interpreter hand over to each other at any
  instruction.
- The generator recognises wait loops (polling LY, STAT, the joypad or a
  flag set by an interrupt, delay loops) and skips whole iterations up to
  the next event. Registers, flags and cycle counts stay identical to
  running every iteration.
- Flags are computed only where a later instruction can read them, and
  cycles are added per straight-line run.
- A precise mode polls interrupts before every instruction. Its builds are
  bit-identical to the interpreter (Super Mario Land, 3,000 frames) and
  serve for differential tests.

### Every ROM size in 240 KiB of flash

- **Small ROMs** sit in flash next to the runtime (about 38 KiB) and the
  compiled code.
- **Compressed ROMs** (`PACK=1`, the default): bank 0 and the most-read
  4 KiB chunks stay raw and directly mapped; every other 512-byte sector
  is LZSS-compressed in flash and decoded on demand into a small cache, by
  a Thumb-1 decoder running from RAM. The ROM shrinks to 63–78% of its
  size, and the freed flash goes to compiled code: on R.C. Pro-Am, 68,000
  instead of 188,000 interpreted instructions over 400 frames, 6% fewer
  cycles per frame (estimate).
- **ROMs of 256 KiB and more** stream from the SD card. Bank 0 and the hot
  chunks stay in flash, chosen from data-read profiles recorded during the
  play sessions; the rest is read 512 bytes at a time into an 8-sector LRU
  cache. The firmware finds the ROM file through FAT16/FAT32 at boot. There
  is no ROM size limit: Link's Awakening, Pokémon Yellow and Donkey Kong
  Land are converted this way.
- **Cartridge RAM** larger than 2 KiB (8 KiB for Link's Awakening, 32 KiB
  for Pokémon) lives in a `.SAV` file on the card behind a two-sector
  write-back cache, so these games fit in 32 KiB of RAM. Battery saves are
  written back when the game disables its RAM, two seconds after the last
  change, and before returning to the loader.

### Speed on a 48 MHz Cortex-M0+

- The hot paths are hand-shaped Thumb-1 assembly: the PPU line state
  machine and line renderer, memory and register access, the interpreter,
  the dispatch cache of compiled code, OAM DMA, the pixel pushes of the
  Pokitto and the LZSS decoder. On 15 games this cut the emulation time
  per frame by 15% to 53% (median 19%), with frames identical to the C
  runtime.
- Devices are lazy: one cycle counter drives everything, the PPU renders a
  whole line at the end of mode 3, and timers, STAT, LYC and VBlank only
  raise events when they become observable.
- The M0+ has no hardware divider, so divisions were taken out of the hot
  paths.
- No console was available to measure all this, so m0boy has its own
  Cortex-M0+ cycle estimator (`targets/meta/tools/m0bench`). **Performance
  figures in this repository are estimates from that model, not device
  measurements.**

### Picture, sound and menu

- META: TBLEND, the default display mode, blends the last line of each tile
  row with the next one, so text is not cut, for about 2% of frame time.
  Other modes trade quality and speed. Pokitto: the picture 1:1 in the
  middle of the screen, or enlarged to 200×176. Unchanged lines are not
  sent again.
- Sound: the APU synthesises a whole frame of samples at VBlank. On the
  META it plays through the DAC by DMA; on the Pokitto through its R2R DAC
  from an assembly interrupt.
- Palettes: green by default. The Game Boy Color boot ROM palettes are
  built in: hold a direction (+ A or B) at boot, as on a GBC, and the
  game's own GBC palette is offered when its title is recognised.
- Option menu (hold HOME on the META, C on the Pokitto): restart, 10
  save-state slots, volume, display mode, palette, frame skip, speed,
  72 MHz overclock, return to the loader.

### Two consoles, one code base

- The runtime, the generated code and the frontend are shared; only the
  drivers differ. Every optimisation switch has its value per console in
  `targets/meta/runtime/gbl_profile.h`.
- The firmwares are bare metal: no Arduino core on the META, no mbed or
  PokittoLib on the Pokitto. Startup, clocks, SPI, display, sound, buttons
  and SD card are written for each chip.

### Tools

- `compile.sh` converts a whole folder of ROMs for one or both consoles:
  play sessions, profiles, firmware build, menu icons and title screens,
  save files, and version tracking (a firmware built by an older version is
  offered for rebuild).
- `tools/meta-compiler`: the same converter in Node, with a local web page.
- `tools/gb-emu`: a fork of Andy O'Neill's
  [gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
  that runs these firmwares.
- [gm0](https://github.com/r043v/gm0): the cycle-counted META and Pokitto
  emulator written alongside m0boy to test it, now in its own repository.
- Tests: per-pixel PPU reference, simulation of the META display, APU
  noise generator, FAT lookup, and end-to-end builds for both consoles.

On r043v's test collection of 1,661 ROMs, the last conversion (8 October
2026) produced 996 META firmwares and 760 Pokitto firmwares. These are
successful builds, not games checked one by one.

## Quick start

You need CMake 3.20 or newer, Ninja, gcc or clang, Python 3, and
[PlatformIO](https://platformio.org/) for the META. The Pokitto only needs
`arm-none-eabi-gcc` (from PATH, otherwise PlatformIO's).

```bash
cmake -G Ninja -B build .
ninja -C build                 # build/bin/gbrecomp

# Put your ROMs in roms/gb/, then:
./compile.sh                   # asks for the console (META by default)
TARGET=pokitto ./compile.sh    # or the Pokitto directly
TARGET=both ./compile.sh       # or both
```

New CMake builds prefer Clang when available, falling back to GCC.
`-DM0BOY_PREFER_CLANG=OFF` prefers GCC; explicit `CC`, `CXX` and toolchains
take precedence. Existing build directories keep their compiler. See the
[compiler guide](docs/COMPILATEURS.fr.md).

Each game lands in `roms/meta/<Game>/` or `roms/pokitto/<Game>/`. Copy that
folder as is to the root of the SD card and start it from the console's
loader. The full guide (in French) is [docs/GUIDE.fr.md](docs/GUIDE.fr.md);
the technical notes are in [targets/meta/README.md](targets/meta/README.md).

Tests:

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

## Limitations

- DMG games only: Game Boy Color-only ROMs are rejected and CGB features
  are not emulated.
- Mappers: none, MBC1, MBC2, MBC3 without the clock, MBC5.
- Simplified timing: mode 3 always lasts 172 dots, lines are drawn all at
  once, OAM DMA is instant, sound is quantised to one frame, the link cable
  has no partner.
- On the META, the 144 lines are reduced to 128.
- Frames are skipped when the console falls behind.

## 100% vibe coded

No line of the port (everything m0boy adds to gb-recompiled) was written by
hand. The code, the tests, the bare-metal drivers and this documentation
were written by AI agents, guided by r043v, who tested on the consoles and
made the choices.

**Claude** (Claude Opus 5.5 in Claude Code) wrote most of it. Its figures on
this project, from 29 September to 9 October 2026:

| | |
|---|---|
| Claude Code sessions | 10 |
| Messages from r043v | 135 |
| Model responses | 3,329 |
| Tool calls | 3,303, of which 3,072 shell commands |
| Tokens generated | 3.54 million |
| Context tokens read from cache | 1.37 billion |
| Commits | 67 |
| Lines added by its commits | 42,375 (95%) |

A second agent (GLM, via ZCode) wrote the rest: 26 commits,
2,207 lines (5%), including the web converter
(`tools/meta-compiler/web.mjs`), the Game Boy Color boot ROM palettes
(runtime 0.7.0) and Pokitto fixes. It also wrote the
[gm0](https://github.com/r043v/gm0) emulator.

The figures come from the Claude Code transcripts and the git history. The
recompiler m0boy started from (`recompiler/`, about 17,000 lines) is
gb-recompiled's.

### History

This repository keeps gb-recompiled's history (104 commits by
arcanite24 and its contributors), then the port's. Removed from it:

- the emulator (`tools/meta-emu-sdl`), now [gm0](https://github.com/r043v/gm0):
  the 61 commits that touched only the emulator are gone;
- gb-recompiled's early test folders that held commercial ROMs converted to
  C (`pokemon_test`, `test_output_tetris`, `tetris_test`,
  `tetris_test_analysis`, `tetris_fixed`), and a debugging audio capture
  (`debug_audio.raw`, `debug_audio.wav`).

The desktop parts of gb-recompiled (SDL runtime, ports, accuracy tools)
were then removed from the tree; they remain in the history and in the
[original repository](https://github.com/arcanite24/gb-recompiled).

## Repository layout

| Path | Content |
|---|---|
| `recompiler/` | `gbrecomp`, with the META and Pokitto backend (`--target meta`, `--target pokitto`) |
| `targets/meta/runtime/` | the `gbl` runtime shared by both consoles |
| `targets/meta/platform/` | shared frontend (menu, saves, SD card, display) and each console's drivers |
| `targets/meta/host/`, `targets/meta/tools/` | host harness, cycle estimator, font tools |
| `compile.sh`, `tools/meta-compiler/` | batch conversion |
| `tools/gb-emu/` | gamebuino-emulator fork |
| `tests/`, `targets/meta/tests/` | tests |
| `docs/` | French user guide, development status |

## ROMs and rights

No ROM and no built firmware are provided. Convert only games you own, or
homebrews whose licence allows it.

## Licence

MIT, see [LICENSE](LICENSE). Based on
[gb-recompiled](https://github.com/arcanite24/gb-recompiled), © 2026
arcanite24, MIT licence. `tools/gb-emu` keeps its own MIT licence (© 2017
Andy O'Neill).
