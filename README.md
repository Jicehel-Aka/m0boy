# m0boy

**Game Boy, SG-1000, Game Gear and Master System games recompiled into
native firmwares for two Cortex-M0+ handhelds: the Gamebuino META and the
Pokitto.**

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
| Chip | SAMD21, Cortex-M0+ at 48 MHz (72 MHz optional, past its rating) | LPC11U68, Cortex-M0+ at 48 MHz (72 MHz optional) |
| Memory | 32 KiB RAM, 240 KiB flash for the firmware | 32 + 2 + 2 KiB RAM, 228 KiB flash below the loader |
| Screen | 160×128: the 144 Game Boy lines blended into 128 | 220×176: picture 1:1, or enlarged to 200×176 |
| Firmware build | PlatformIO | `make` with arm-none-eabi-gcc |
| Version | 0.10.13 | 0.10.10 |
| Status | Games run on the console | Tested in an emulator only. A first tester on a real Pokitto reports a picture flipped vertically (rows reversed, columns in order), consistent with the panel's SS bit (R01) being lost at init; 0.10.1 lengthens the post-reset wait and the register write strobe and writes R01 again, still to be checked on the device |

## SG-1000

Sega SG-1000 cartridges (`.sg`, unbanked, up to 48 KiB) go through the same
pipeline: the Z80 code is recompiled, not emulated.

- **Recompiler** (`--system sg1000`): code played during automatic sessions
  (button 1 held, no input at all for logos and demos, random games), then
  the code it reaches statically, becomes C regions like the Game Boy
  backend's: gotos inside, `CALL` as a C call, flags computed only where a
  later instruction reads them (common cases inline), cycles added in
  batches, device sync at loop heads. Targets of `RET` and `JP (HL)` seen
  while profiling (pushed return addresses, jump tables) become region
  entries. Rare instructions use the Z80 interpreter's semantics on
  constant operands; the interpreter runs what was never seen.
- **Runtime**: two device events per frame instead of a check per
  instruction. TMS9918A lines are drawn when the program touches the VDP or
  at VBlank, box-filtered from 256×192 to the LCD (exact 8-to-5 weights,
  table-driven: two colours per tile; tile loops in Thumb-1 assembly run
  from RAM). Sprite line masks follow each sprite Y write. The SN76489 is
  caught up before each write, in blocks shared by its four channels.
  8 KiB cartridge RAM (The Castle) is detected while profiling.
  `OUTI`/`OUTD`, which the games unroll for VRAM uploads, are compiled as on
  the Game Gear instead of calling the interpreter.
- **Checked**: built with `--meta-precise`, the recompiled code reproduces
  the interpreter exactly (registers, memory, VDP, PSG, picture) on all 68
  cartridges of a collection. The Z80 core passes ZEXDOC and 66 of 67 ZEXALL
  tests.
- **Speed**, measured in the gm0 emulator (not on hardware), at 48 MHz:
  67 of those 68 games run at full NTSC speed on both the META and the
  Pokitto (H.E.R.O. only slows down while booting); about 32 % with the
  former emulation.

## Game Gear

Game Gear cartridges (`.gg`) go through the same Z80 recompiler, extended to
banked code:

- **Recompiler** (`--system gg`): an instruction is a page and an address;
  a region covers one 16 KiB slot of one page. Calls into another slot check
  that slot's page before jumping straight to its code, otherwise the
  dispatcher finds the right page's region; code returns to the dispatcher
  whenever the mapper moves the page it runs from. Sega and Codemasters
  mappers.
- **Runtime**: mode 4 VDP (tiles, scrolling with locks, sprites, priority,
  line interrupts, 12-bit CRAM) drawn 1:1 in the 160×144 window, background
  tiles in Thumb-1 assembly; Sega PSG (mono).
- **Large ROMs and saves**: a ROM that does not fit in flash with its code
  first tries independently LZSS-packed sectors in flash, promoting the hot
  sectors back to direct raw reads with the flash left over. If that does not
  fit, it keeps in flash page 0, the instructions it compiles and the 512-byte ROM
  sectors that, from the recorded play sessions (executions and sector reads
  per frame), save the most cycles per byte, raw or LZSS-packed; it reads the others from a raw copy
  on the SD card. Battery-backed cartridge RAM lives in a `.SAV` file on the
  card. Both share a small sector cache (four sectors on the Pokitto Game Gear, two
  elsewhere): the console's own 24 KiB of video and work RAM leave little
  else. The play sessions of a Game Gear or Master System game are recorded
  by `compile.sh`: blind sessions (10 800 frames each) and four explorations
  (`--explore`, 120 000 frames each) in which the player keeps a
  snapshot of every state where it saw code or data it had not seen and plays
  on from there, so that it reaches levels a blind player does not. The
  explorer (`targets/common/host/m0_explore.h`) is shared by the Game Boy,
  SG-1000 and Game Gear/Master System harnesses (`EXPLORERS`,
  `EXPLORE_FRAMES`); on the Game Boy it plays from save states, 400 000
  frames by default. Older
  sessions, without the execution counts or the explorations, are redone. The
  sectors the interpreter fetches the code of, for what is not compiled, count
  like the sectors read as data.
  The encoder, C/Thumb-1 LZSS decoders and hot-block promotion policy are
  shared with Game Boy (`recompiler/src/rom_storage.cpp`,
  `targets/common/runtime/m0_rom_*`). All-raw storage takes precedence when
  the whole ROM and requested code fit. Compression decisions use estimated
  costs, not device measurements. DEFLATE remains benchmark-only for now.
  Storage validation for 0.10.11: 928 local GG/SMS cartridges matched the
  reference and generated interpreters over 120 frames (seed 201); 193
  remaining checks were blocked before comparison by generation/build or
  storage failures. Six selected GG games also matched over 2000 frames.
  This is not a whole-collection compatibility claim or a hardware test.

**META hardware issue reported with earlier firmware:** streamed games show
a blue screen and buttons do not work in other games. The SPI driver is
under investigation; 0.10.11 is a prerelease until this is addressed. Emulator
success does not validate electrical bus timing on a console.
- **Checked**: with `--meta-precise`, the recompiled code reproduces the
  interpreter exactly on all 225 cartridges of a Game Gear collection.
- **Speed**, measured in gm0 at 48 MHz on the games tried: full speed on
  both consoles (Columns, Pac-Man, GG Aleste, Fantasy Zone; Aladdin, mostly
  read from the card, 94 % on the META and 98 % on the Pokitto).

## Master System

Master System cartridges (`.sms`, `--system sms`) share the Game Gear
recompiler and runtime. The whole 256×192 picture is shown, shrunk 8-to-5
across as the Game Gear's own SMS mode does: two neighbouring 4-bit pixels
index a table of blends per palette, the background loop in Thumb-1. One
line in 8/5 is drawn (160×120 on the META, 200×150 on the Pokitto). PAUSE is
MENU / C. Alex Kidd in Miracle World, Shinobi and Wonder Boy III run at
97–99 % of full speed on both consoles (gm0). No FM sound yet.

Desktop play uses the [gm0 `z80` branch](https://github.com/r043v/gm0/tree/z80):
`gm0 game.sg`. Details (French): [docs/MULTI_SYSTEM.fr.md](docs/MULTI_SYSTEM.fr.md).

## What was built

### Recompilation guided by play

- `compile.sh` plays each ROM by itself before compiling it: one
  random-input session per CPU core, 20 s each at full speed (3,000 to
  12,000 frames a second, so 15 to 60 minutes of play per session), four
  explorations (below) and any session you record yourself. Only the
  instructions these sessions
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
- The Sega backends (SG-1000, Game Gear, Master System) share more than the
  Z80 core: one CMake template per binary, the host helpers, the frontend
  skeleton, the generated code's interface (`sg_gen.h`) and the PSG
  (`sg_psg.h`) live in `targets/common/`; what differs is behind a switch
  (`sg_config.h`) or a macro.
- The META's bus rates live in `targets/meta/runtime/gbl_board.h`: panel at
  24 MHz (the rate meta-lapinou runs this panel at; 36 and 48 come from the
  PLL and are opt-in — "SPI ecran" in the Game Boy firmware's menu, or a
  game's own `build_flags`, `-DMETA_LCD_SPI_MHZ=36`), SD card at 24 MHz
  after the 250 kHz init, buttons at 12 MHz.

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

You need CMake 3.20 or newer, Ninja, gcc or clang, Python 3, zlib development
headers/library (host encoder only), and
[PlatformIO](https://platformio.org/) for the META. The Pokitto only needs
`arm-none-eabi-gcc` (from PATH, otherwise PlatformIO's).

```bash
cmake -G Ninja -B build .
ninja -C build                 # build/bin/gbrecomp

# Put your ROMs in roms/gb/ (Game Boy) and roms/sg/ (SG-1000), then:
./compile.sh                   # asks for the console (META by default)
TARGET=pokitto ./compile.sh    # or the Pokitto directly
TARGET=both ./compile.sh       # or both
```

`FIRMWARE_COMPILER=clang` builds the firmwares with Clang (GCC by default).
New CMake builds prefer Clang when available, falling back to GCC.
`-DM0BOY_PREFER_CLANG=OFF` prefers GCC; explicit `CC`, `CXX` and toolchains
take precedence. Existing build directories keep their compiler. See the
[compiler guide](docs/COMPILATEURS.fr.md).

Each game lands in `roms/meta/<source>/<Game>/` or `roms/pokitto/<source>/<Game>/`,
where `<source>` is the ROM platform (`gb`, `sg`, `gg` or `sms`). Copy that
game folder as is to the root of the SD card and start it from the console's
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
- SG-1000: unbanked cartridges up to 48 KiB, NTSC only, no SC-3000
  keyboard; code never seen while profiling runs in the interpreter.

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

A third agent (DeepSeek V4.1 Flash Free, via OpenCode) worked on the buses
and on the duplicated code: the SD card from 12 to 24 MHz, a rate per SERCOM4
device, and the SG-1000, Game Gear and Master System backends merged onto one
CMake template, host helpers, frontend skeleton and Z80 engine interface.
9 commits, 39 files, 861 lines added and 859 removed — the same code in fewer
places. It also took the panel to 48 MHz and back to 24 in 0.10.9: nothing
above 24 has been on a real META, and 36/48 stay one menu press away.

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
| `recompiler/` | `gbrecomp`/`m0recomp`, with the META and Pokitto backend (`--target meta`, `--target pokitto`) and the SG-1000 Z80 recompiler (`src/system/sg1000/`) |
| `targets/meta/runtime/` | the `gbl` runtime shared by both consoles |
| `targets/meta/platform/` | shared frontend (menu, saves, SD card, display) and each console's drivers |
| `targets/meta/host/`, `targets/meta/tools/` | host harness, cycle estimator, font tools |
| `targets/sg1000/` | SG-1000 runtime (TMS9918A, SN76489, fallback Z80 interpreter), console frontend, host harness |
| `targets/gamegear/` | Game Gear/Master System runtime (mode 4 VDP, Sega PSG, paging, saves), console frontend, host harness |
| `targets/common/` | shared ROM codecs/benchmarks and play explorer (GB and Sega); Sega CMake templates, host helpers, frontend skeleton, `sg_gen.h`, `sg_psg.h` |
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
