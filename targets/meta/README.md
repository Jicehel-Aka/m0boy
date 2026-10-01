# Gamebuino META target (`--target meta`)

`gbrecomp --target meta` turns a DMG ROM into a PlatformIO project for the
[Gamebuino META](https://gamebuino.com) (ATSAMD21G18A: Cortex-M0+ at 48 MHz,
32 KiB RAM, 256 KiB flash with a 16 KiB loader, 160×128 ST7735 display, DAC
speaker). It is a separate, compact backend: the desktop runtime and emitter
are untouched.

| Part | Location |
| --- | --- |
| Emitter | `recompiler/src/codegen/meta_emitter.cpp` |
| Runtime (portable C) | `targets/meta/runtime/gbl_*.c` |
| META frontend | `targets/meta/platform/src/meta_*.cpp` |
| Host harness | `targets/meta/host/gbl_host.c` |
| Cycle estimator | `targets/meta/tools/m0bench/` |
| Tests | `targets/meta/tests/`, `tests/test_meta_target_e2e.py` |

## How it works

- **Coverage-driven compilation.** Only instructions that a profiling run
  actually executed are compiled, so data is never mistaken for code and the
  output fits in flash. Everything else, including RAM code such as OAM DMA
  routines, runs in the compact `gbl` interpreter. The interpreter is also
  the reference for differential tests.
- **Regions.** Covered instructions are grouped by address into C functions.
  Inside a function, control flow uses `goto`. CALL is a C call followed by a
  return-address check, and RET is a C return. A mismatch unwinds to the
  nearest `gbl_exec()` loop, which resumes from `gbr.pc` through compiled code
  or the interpreter. A C stack budget bounds nesting.
- **Cheap state.** Flags are stored as `zr` (Z is set when it is zero) plus
  0/1 bytes for C/H/N. A per-region liveness pass emits only the flags that a
  later instruction can observe. Cycles are summed per straight-line run and
  flushed before control transfers and IO accesses. Pending cycles carry
  across labels (most instructions are labels, because an interrupt can
  return there); gotos and the region's entry switch subtract the carried
  amount instead.
- **Wait loops.** The generator recognises loops with no side effect other
  than registers and flags, that carry nothing from one iteration to the
  next: polling LY, STAT, IF, the joypad or a RAM flag set by an interrupt.
  They repeat the same iteration until the value they read can change: at
  the next PPU mode or line change for LY/STAT, at the next device event
  otherwise. The back edge then adds whole loop periods to the cycle counter
  (`gbl_spin`). Delay loops (`DEC r`, or `DEC rr; LD A,hi; OR lo`, then
  `JR NZ`) skip whole iterations up to the next event (`gbl_spin_n`), then
  replay the last one so registers and flags match. The interpreter
  shortcuts the `DEC A / JR NZ,-3` OAM DMA wait that games copy to HRAM.
  The results are identical to running every iteration, frames, WRAM,
  registers and cycle counter included.
- **Lazy devices.** One T-cycle counter drives everything:
  - The PPU renders a whole scanline at the end of mode 3, straight into the
    frontend's RGB565 DMA buffer.
  - The timer, STAT, LYC and VBlank raise events only when they become
    observable.
  - Interrupts are polled at region entry, at loop heads, on HALT, after EI
    and after writes to IE, IF, STAT, LCDC or LYC.
- **Sound.** The APU keeps channel state on register writes and synthesizes
  a whole frame of 8-bit mono samples at VBlank. The frame sequencer runs in
  sample time.

The `--meta-precise` mode polls interrupts before every instruction, exactly
like the interpreter. It is meant for differential testing, not for the
device.

## Workflow

```bash
# 0. Build gbrecomp (repository root)
cmake -G Ninja -B build . && ninja -C build

# 1. Interpreter-only project + host tools
./build/bin/gbrecomp game.gb --target meta -o output/game_meta
cmake -G Ninja -S output/game_meta/host -B output/game_meta/host/build
ninja -C output/game_meta/host/build

# 2a. Record coverage by playing (SDL2 window; Esc saves and quits)
./output/game_meta/host/build/game_play --interp \
  --coverage logs/game-1.cov --record-input logs/game-1.input

# 2b. ...or from a scripted run ("<frame> <buttons>" lines)
./output/game_meta/host/build/game_profile --interp --frames 3000 \
  --input logs/game-1.input --coverage logs/game-1.cov

# 3. Compile the covered code (several --coverage files are merged)
./build/bin/gbrecomp game.gb --target meta --coverage logs/game-1.cov \
  -o output/game_meta

# 4. Firmware
pio run -d output/game_meta                 # check the RAM/Flash report
pio run -d output/game_meta -t upload       # USB (sam-ba, offset 0x4000)
# or copy output/game_meta/.pio/build/meta/firmware.bin to the SD card
```

Coverage is incremental. Run a compiled build with `--coverage` but without
`--interp`: it records only the instructions that still fall back to the
interpreter. Regenerate with every coverage file. The META prints the count
of interpreted instructions each second on its USB serial port.

Play-mode keys: arrows, X = A, Z = B, Enter = Start, Backspace or Right
Shift = Select.

On the META:

| Button | Game Boy |
| --- | --- |
| D-pad | D-pad |
| A | B (swapped: `META_SWAP_AB`, default 1) |
| B | A |
| MENU | Start |
| HOME (tap) | Select |
| HOME (hold 1 s) | Next display mode |
| HOME (hold 3 s) | Back to the Gamebuino loader |

Display modes (144 DMG lines on 128 META lines), cycled with HOME held:

| Mode | Method |
| --- | --- |
| SMOOTH | Area resampling, 9 lines onto 8 rows: row k of each group is (8 − k)/9 of line k plus (k + 1)/9 of line k + 1, nothing dropped. Lines are rendered packed (2-bit shades, `gbl_ppu_packed`, no colour conversion in the PPU) and every row is blended from two of them, two pixels per lookup in 4 KiB of compile-time tables (Thumb-1 loop in `meta_display_asm.S`) |
| TILE | Drops the last line of each tile row (8k+7), keeping lines 63 and 95; fonts usually leave that line blank, so text stays intact (taken from gm0) |
| BLEND | Merges lines 8 and 9 of every 9 with averaged colours (1 KiB table) |
| DARK | Same merge, keeping the darker shade |
| CROP | No scaling: DMG lines 8–135 |
| SKIP | Drops the 9th line of every 9 |
| TBLEND (default) | TILE's rows, but line 8k+7 is averaged into line 8k+8 instead of dropped: 7 rows of 8 stay sharp, only line 143 is lost (taken from gm0's ESPboy port). The two lines are rendered packed; the 15 held lines are kept across frames (600 bytes, in the RAM of the BLEND/DARK buffers) so a held line is rendered only when it changes, and the pair goes through SMOOTH's Thumb-1 loop with a 1 KiB flash table of ½ weights |

SMOOTH and line skipping: a row needs both of its lines, but only the
previous one is kept (keeping all 144 would take 5.6 KiB of RAM). When only
the lower line of a row changed, the upper one was not rendered: that row
is drawn from the lower line alone for one frame and blended on the next.
Lines that changed are rendered again on the next frame, so things that
keep moving find their upper line (0 late rows over 119 frames of sprites
moving down in the test). M0 estimator, real display pipeline
(`--cflag=-DBENCH_META_DISPLAY=0`, 1 for TILE), 25 games: SMOOTH costs
3.6% to 31% more per frame than TILE (median 12%; Super Mario Land 106k →
131k cycles), mostly the blend of every row; frames over the 60 Hz budget
go up on Prehistorik Man (23 → 103 of 1,599) and Faceball 2000 (35 → 67).
With the identity BGP (0xE4, most games) packed lines are the raw shades,
copied without a table.

TBLEND on the same 22 non-streaming games of the panel (estimator, real
display pipeline, `-DBENCH_META_DISPLAY=6`, projects regenerated with the
current runtime): +0.9% to +7.2% per frame over TILE (median +1.9%;
Lemmings, non-identity BGP, is the +7.2%), against +9.5% to +32% for SMOOTH
(median +15.3%) and +7.9% to +27.8% for BLEND. Frames over the budget stay
at TILE's level (Prehistorik Man 29 against 25; SMOOTH 100). RAM is
unchanged (Super Mario Land 29,784 bytes either way), flash +1.4 KiB. A
first version that rendered the held line every frame through the palette
switch of BLEND cost as much as SMOOTH (median +15.5%).

`meta_display_panel_matches_reference` runs the real display pipeline
against a simulated ST7735 (row windows included) and checks every mode
pixel for pixel over 400 frames of random changes (SMOOTH: on the frames
after an unchanged one, the late rows being counted), plus the SMOOTH
tables against the formula and a run of sprites moving down. `<name>_metaview
--mode … --dump N file.ppm` writes the exact META screen of a game on the
host.

The buttons are read at 12 MHz in the Gamebuino library bit order. Build with
`-DMETA_BUTTONS_LAPINOU_ORDER` (and `-DMETA_BUTTONS_SPI_HZ=24000000`) for the
order observed when reading at 24 MHz.

## Saves and large cartridge RAM

Battery-backed cartridges (`GBL_BATTERY`, from header byte 0x147) keep
their cartridge RAM in a file on the SD card, `GBL_SAVE_NAME` (e.g.
`MQ.SAV`). `compile.sh` creates it at exactly the size of the RAM (filled
with 0xFF, an erased cartridge) and never overwrites it, inside the game's
folder (see below). Copy the folder to the card once, in the root
directory: the console rewrites its 512-byte sectors in place and never
touches the FAT.

- RAM up to 2 KiB (MBC2's 512 half-bytes, for instance) stays in RAM. It is
  loaded at boot; writes go through the slow path and mark sectors dirty.
- Larger RAM (`GBL_ERAM_ON_SD`, the default above 2 KiB: 8 KiB for Link's
  Awakening, 32 KiB for Pokémon) lives only in the file, behind a two-sector
  write-back cache. Without that, those games did not fit in RAM (8 KiB on
  top of 29 KiB). Such builds, like streaming ones, keep two display strips
  and drop the BLEND, DARK and TBLEND modes (TBLEND falls back to TILE). Zerd no Densetsu (8 KiB) builds at
  27.5 KiB of RAM.
- Changed sectors are written back in three cases:
  - when the game disables cartridge RAM, which games do right after
    saving;
  - two seconds after the last change;
  - before returning to the loader (HOME held 3 s).

  Switching the console off during the two seconds that follow a save
  without RAM disable can lose it.
- If the file is missing, a game whose RAM must live on the card shows a
  yellow screen. A game whose RAM stays in RAM runs normally, but its
  progress is not kept (the USB serial status says so).
- On the host, `--save FILE` uses the same file format. Without it, the RAM
  starts erased and is not kept.

Test on the host: Gem Gem (8 KiB) writes 5 sectors on its first run and
reads them back on the next (different state, nothing rewritten). The e2e
test checks the same round trip on a synthetic battery cartridge, and that
RAM on the card and RAM in memory give the same machine state.

Compiled regions call each other directly. Each one now checks the C stack
on entry (`ENTER()`, `GBL_STACK_GUARD`), like `gbl_exec`. A guest that
recurses without returning (a crashed game looping on `RST $38`) therefore
unwinds instead of overflowing the C stack. The check costs about 0.3%
(SML).

`gbl_exec` and `gbl_run` only call a compiled region when at least
`GBL_EXEC_HEADROOM` (512) bytes remain above the limit, so the region's own
guard always passes at least once. Without that margin a region entered
just above the limit returned at once without progress and the dispatcher
called it again forever (seen on R.C. Pro-Am with a packed ROM at frame 86).
With no room at all, `gbl_run` interprets instead. The host takes
`--stack-kib N` or `--stack-bytes N` to reproduce small budgets.

## ROMs larger than the flash (SD streaming)

The whole ROM normally sits in the META's 240 KiB of application flash, next
to about 38 KiB of runtime, so ROMs of 256 KiB and more cannot fit. For
them, the generator switches to streaming (`--meta-stream auto`, the
default; `on` / `off` force it):

- Bank 0 and the most-read 4 KiB chunks of the other banks stay in flash
  and are mapped directly: reading them costs nothing more than usual.
  Compiled code never reads its own instructions from the ROM, so only
  data (tiles, maps, text, tables, music) and the code left to the
  interpreter count.
- The other chunks are read from the ROM file on the SD card, one 512-byte
  sector at a time, into an LRU cache of `GBL_STREAM_SECTORS` sectors
  (4, i.e. 2 KiB). A read first lets the display's DMA queue drain and
  frees the shared SPI bus, then issues CMD17 at 24 MHz.
- The resident set comes from ROM data-read profiles. In streaming
  projects, the host `_profile` and `_play` builds record them with
  `--rom-profile-out FILE`; pass them back with `--rom-profile FILE`.
  By default, resident data may take up to half the free flash
  (`--meta-rom-resident-kib` overrides this), and compiled code gets the
  rest.
- The ROM file must be on the card under the 8.3 name printed by the
  generator (`GBL_STREAM_NAME`, e.g. `MYSTICQU.GB`), in the root directory
  or one directory below. At boot the firmware finds it through FAT16 or
  FAT32 (`meta_fat.c`; no FAT access afterwards; at most 16 fragments) and
  checks its cartridge header against bank 0. If that fails, the screen
  turns red (file not found), blue (card unreadable), orange (too
  fragmented) or magenta (a different ROM). The USB serial port explains
  the error, and holding HOME for 3 s returns to the loader.
- To make room for the 2 KiB cache, streaming builds use two display strips
  instead of three and drop the BLEND, DARK and TBLEND modes (TBLEND falls back to TILE). RAM then stays at the
  usual level: 28.9 KiB.

`compile.sh` records the profiles, regenerates with them, and copies the
ROM into the game's folder under its card name.

## Compressed ROM in flash (`--meta-pack`, default on)

With `--meta-pack` (the default in `compile.sh` and the Node compiler;
`PACK=0` / `--no-pack` turn it off), the streaming machinery
reads from flash instead of the SD card (`GBL_ROM_PACKED`):
- Bank 0 and the hot 4 KiB chunks stay raw and mapped directly. By default
  they take at most a third of the free flash, so more of it goes to
  compiled code.
- Every other 512-byte sector is LZSS-compressed in flash (Heatshrink-style:
  a flag bit per token, 8-bit offset, 4-bit length), or stored as is when
  compression does not shrink it.
- A cache miss decodes the sector into the same 512-byte slot the SD
  reader would fill. It needs no card, no `.GB` file and no extra RAM:
  back-references read the sector being decoded.
- `gbl_unpack512_asm` (Thumb-1, in SRAM) decodes on Cortex-M0+
  (`GBL_UNPACK_ASM`); the C version in `gbl_core.c` is the reference.
- When the compressed ROM leaves less than 16 KiB for code, the generator
  falls back to SD streaming (256 KiB or more) or to a plain build, and
  says so.
- The raw part is chosen from the data-read profiles (`--rom-profile`).
  Without a profile nothing but bank 0 stays raw and every cache miss
  decodes. `compile.sh` therefore replays the recorded inputs once with the
  profile build when a streamed or packed project has no `romdata-*.prof`
  (sessions recorded with a plain build have none), and keeps the result
  with the sessions. `PACK` is stored in `.gbrmeta.json`; changing it
  rebuilds the game. For a ROM that fits in flash (Tetris), everything
  touched stays raw and packing changes nothing.

R.C. Pro-Am, 600 frames, M0+ estimator (not a device measurement):

| Build | Cycles/frame | `.text` |
| --- | --- | --- |
| plain (whole ROM in flash) | 359,596 | 218,780 |
| packed, 60 KiB raw | 336,881 (−6.3%) | 218,076 |
| packed, 4 KiB raw, asm decoder | 345,380 (−4.0%) | 210,352 |
| packed, 4 KiB raw, C decoder | 354,050 (−1.5%) | 210,512 |

The gain comes from the flash left to compiled code (68k instead of 188k
interpreted instructions over 400 frames), not from the decoder. The asm
and C decoders give the same pixel hash; with `--interp` all three builds
give identical hashes (every ROM read goes through the decoder). In fast
mode the frames differ slightly from the plain build, as with any change of
compiled coverage.

Measured compression (compressed size / original, 512-byte blocks,
Heatshrink-style 256-byte window): SML 68%, Batman 63%, R.C. Pro-Am 78%,
Tetris 73%, Mystic Quest 78%. FastLZ does 5 to 10 points worse on these
ROMs; zlib does 10 points better, but it needs Huffman tables in RAM.
Compressing ROM data meant for the SD card would not pay: decoding costs
more than the transfer it saves.

## Batch conversion (`compile.sh`)

`./compile.sh [rom dir] [output dir]` turns every `.gb`/`.gbc` of the
folder into games laid out the way the Gamebuino loader expects, one
folder per game under the output directory (default `./roms/meta/`), each
meant to be copied as is to the root of the SD card:

| File | Role |
| --- | --- |
| `<Game>/<Game>.bin` | firmware, named after the ROM with tags stripped (`Zerd (J) [!].gb` → `Zerd`) |
| `<Game>/ICON.BMP` | 32×32 menu icon |
| `<Game>/TITLESCREEN.BMP` | 80×64 title screen |
| `<Game>/<NAME>.SAV` | cartridge-RAM file, created once (battery or large RAM) |
| `<Game>/<NAME>.GB` | streamed ROM file (ROMs of 256 KiB and more) |
| `<Game>/.gbrmeta.json` | bookkeeping (ROM sha1, version, `PACK`); ignorable |

The icons come from thumbnails.libretro.com (`Named_Titles`): the
catalogue index is cached in `<output dir>/.cache/icons/`, region and
revision tags are ignored for matching, `(World)` then `(USA, Europe)`
are preferred. A local PNG (`<rom dir>/icons/<Game>.png`) takes
precedence; a failure only means no icon. `NO_ICONS=1` disables
downloads. ImageMagick or Pillow converts the PNG to 24-bit BMPs.

There is no ROM size limit: ROMs that do not fit in flash stream from the
card (see above).

Every firmware embeds the version of the META edition that produced it
(`gbrecomp-meta x.y.z` string in the .bin, also printed on the USB serial
port at boot). The version lives in `targets/meta/VERSION` only: the
recompiler stamps it (`GBL_GEN_VERSION`), `gbrecomp --meta-version` prints
it, the release archive's CMake project and zip name take it. Bump it when
the runtime, the frontend or the generated code change what a firmware
does. `compile.sh` and `tools/meta-compiler` compare it with the string
read back from each .bin (or `.gbrmeta.json` when the .bin has none): a firmware that is current
(same version, same ROM sha1) is kept as is; one produced by another
version is offered for rebuild. On a terminal the script is interactive:
it lists the ROMs with their state (new, current, vX → vY, ROM changed,
unknown version), asks which games to process (`1,3-5`, `a` = all), and
confirms version updates. Otherwise it processes everything that needs
work, keeping outdated firmwares unless `UPDATE=1` (`FORCE=1` redoes even
current ones; current ones only get missing icons).

- Precompute: 8 random-input runs per ROM in parallel (`AUTO_SEEDS`), one of
  30 minutes of game time and seven of 10 minutes, about 11 s on 8 cores.
- Several ROMs run in parallel (`JOBS`, default cores / 4), each with its
  log in `out/<game>/compile.log`. Five ROMs take about 70 s on 16 cores.
- Every run, random or played (`PLAY=1`), is kept in
  `<rom dir>/profiles/<sha1>/` (`.input`, `.cov`, `.prof`) and reused by
  later runs. `AUTO_REDO=1` redoes the random runs, and `REPLAY=1` replays
  the played ones.

A cross-platform Node port lives in `tools/meta-compiler/`:
`node tools/meta-compiler/compile.mjs [rom dir] [output dir]`. Same output
layout, same state files and icon cache (it interchanges compiled games,
profiles and `.gbrmeta.json` with `compile.sh`), same environment
variables plus `--all` / `--update` / `--force` / `--no-icons` / `--play`
/ `--pack` flags. In a terminal it runs a fullscreen interface (ink): a checkable
list of the ROMs with their state, then a dashboard with the games and
their current phase on one pane, global counters and the activity log on
the other (`q` stops launching new games, Ctrl-C kills the running
builds). `ink` and `react` are installed with npm on first run; a
numbered-list fallback needs no dependency. `compile.sh` stays the
reference.

Unique instructions found by random runs:

| Runs | SML | R.C. Pro-Am | Tetris | Mystic Quest |
| --- | --- | --- | --- | --- |
| 1 × 10 min | 4,735 | 8,118 | 3,315 | 10,781 |
| 8 × 10 min | 5,262 | 8,302 | 3,372 | 10,974 |
| 1 × 30 min | 5,578 | 8,221 | 3,374 | 10,802 |
| 12 × 30 min | 5,803 | 8,329 | 3,559 | 10,986 |

Beyond the default, the gain is under 5%, while the flash holds 3,500 to
8,000 compiled instructions.

Mystic Quest (256 KiB, MBC2), with the resident set taken from a
10-minute random-input run:
- 88 KiB of banks 1+ stay in flash and 152 KiB are read from the card;
- over 10 minutes of play with another random input, 27 sectors were read,
  at most 3 in one frame (about 1 ms at an estimated 0.3 ms per sector).

Random input only reaches the name entry and the first fight, so these
figures say nothing yet about the rest of the game. The SD timings are
computed, not measured. `rom_stream` in the host output reports sectors read
per frame for any input.

## Build options

Pass these as `-D` flags (`build_flags` in `platformio.ini`, or
`CMAKE_C_FLAGS` for the host).

| Define | Default | Effect |
| --- | --- | --- |
| `GBL_PPU_BATCH` | 8 | Scanlines per PPU event when no per-line STAT interrupt is enabled; 1 means one event per line. VRAM/OAM writes render pending lines first. |
| `GBL_LINE_SIGNATURE` | 1 | Signature of each line's inputs (registers, map row and tile versions, sprites), with per-row tile hashes cached |
| `META_LINE_SKIP` | 1 | Display rows whose signature did not change are neither rendered nor sent; changed rows are sent as runs of consecutive rows, each with its own address window. This is exact. |
| `GBL_FRAME_SIGNATURE` | 1 | Per-frame signature |
| `META_STATIC_SKIP` | 1 | Frames that cannot differ skip even the line checks; a change then shows up one frame late |
| `GBL_PPU_PAL32` | 0 | Converts pixels four at a time with a 2 KiB table (gm0's `pal32`); measured no faster |
| `GBL_GEN_OPTIMIZE_SIZE` | 1 | Compiles generated code with `-Os` |
| `GBL_SPIN` | 1 | Fast-forwards the wait loops the generator marked; 0 runs every iteration (identical results, for checking) |
| `GBL_INTERP_DELAY_LOOP` | 1 | Interpreter shortcut for `DEC A / JR NZ,-3` (not in coverage or trace builds) |
| `GBL_PPU_ASM` | 1 on Cortex-M0+ | Tile fetch and colour conversion in hand-written Thumb-1 (`gbl_ppu_asm.S`); the C loops are used on other targets |
| `GBL_PPU_RAMFUNC` | 1 | Runs those loops from SRAM (about 220 bytes, copied with `.data`), with no flash wait state |
| `GBL_CORE_ASM` | 1 on Cortex-M0+ | `gbl_rd`, `gbl_wr`, `gbl_push`, `gbl_pop`, `gbl_cp` and the OAM DMA copy in Thumb-1 (`gbl_core_asm.S`) |
| `GBL_TIMING_ASM` | 1 on Cortex-M0+ | PPU line state machine (`gbl_ppu_advance`), next event (`gbl_update_evt`), `gbl_sync`, the PPU register paths of `gbl_io_rd`/`gbl_io_wr` and the dispatch-cache hit of `gbl_lookup` in Thumb-1 (`gbl_timing_asm.S`) |
| `GBL_INTERP_ASM` | 1 on Cortex-M0+ (not in coverage or trace builds) | Interpreter loop and common opcodes in Thumb-1 (`gbl_interp_asm.S`); DAA, EI, RETI, HALT, STOP, PUSH/POP AF, SP arithmetic and `LD (nn),SP` go through the C `gbl_interp_op` |
| `GBL_RENDER_LINE_ASM` | follows `GBL_PPU_ASM` | `render_line` (sprite selection, line signature, frontend calls) in Thumb-1 (`gbl_ppu_asm.S`); needs `GBL_LINE_SIGNATURE` |
| `GBL_LOOKUP_CACHE` | 64 | Entries of the dispatch cache, a power of two (0 disables it) |
| `GBL_STREAM_SECTORS` | 4 | Sector cache of a streamed ROM (512 bytes each) |
| `GBL_ERAM_ON_SD` | 1 above 2 KiB of cartridge RAM | Cartridge RAM kept in the save file behind a 1 KiB cache |
| `GBL_ROM_PACKED` | set by `--meta-pack` | Streamed ROM sectors LZSS-compressed in flash instead of on the SD card |
| `GBL_UNPACK_ASM` | 1 on Cortex-M0+ | Thumb-1 decoder for those sectors |
| `GBL_STACK_GUARD` | 1 | C stack check on entry to every compiled region |
| `META_SWAP_AB` | 1 | META A = Game Boy B, META B = Game Boy A |
| `META_SD_SPI_HZ` | 24000000 | SPI clock for SD sector reads |
| `META_MERGE_MODES` | 1 (0 when streaming) | BLEND, DARK and TBLEND display modes |
| `META_AUDIO_DMA` | 1 | Audio samples go to the DAC by DMA (TC4-triggered DMAC channel 1, chained descriptors) instead of a 22 kHz TC4 interrupt (about 3% of the CPU); 0 restores the interrupt |
| `META_LCD_DMA` | 1 | Strips are sent by DMA through a queue: the DMA-complete interrupt programs the next window and starts the next strip, so rendering waits only when every buffer is in flight. At VBlank, if the bus is still busy, the button read is postponed by one frame (never two in a row). 0 writes pixels from the CPU. |
| `META_LCD_BUFFERS` | 3 | Strip buffers (triple buffering by default) |
| `META_STRIP_LINES` | 4 | Rows per strip |
| `META_MAX_SKIP` | 3 | Maximum run of frames skipped for speed |

The output is identical with every option on or off (SML host `run_hash`,
3,000 frames, two input scripts). The line-signature contract is checked on
every run: on SML 74–79% of lines were unchanged, with 0 false "unchanged"
among about 850k lines. It is also checked by the random-mutation part of
`meta_ppu_matches_per_pixel_reference`.

M0+ estimates on 1,600 SML frames:

| Configuration | Cycles per frame |
| --- | --- |
| No skipping, batched PPU, `-Os` | 396k |
| + pal32 | 397k |
| Static-frame skip only | 293k |
| Line skip only (exact) | 289k |
| Line skip + static-frame skip (default before the optimizations below) | 260k |

Optimizations measured with the same estimator (1,600 frames; SML with its
walk script, the other games with the autoplay input from `compile.sh`, the
same input for both columns; the ROMs are not in the repository):

| Game | Before | After | Change |
| --- | --- | --- | --- |
| Super Mario Land | 264,889 | 212,756 | −19.7% |
| Super R.C. Pro-Am | 394,604 | 257,643 | −34.7% |
| Batman: The Video Game | 366,461 | 297,278 | −18.9% |
| Burai Fighter Deluxe | 119,286 | 80,159 | −32.8% |
| Solar Striker | 334,739 | 245,722 | −26.6% |

The changes involved:

| Change | Main effect |
| --- | --- |
| Wait-loop fast-forward and the interpreter's DMA-wait shortcut | SML −10%, R.C. Pro-Am −26% (polling STAT and a RAM flag) |
| PPU state in one structure, branch-free STAT line, early return in `gbl_ppu_sync` | SML −6% |
| Cycles carried across labels | Generated code −13 to −19%, at +3.6 KiB of flash for the entry switches |
| Thumb-1 tile fetch and conversion, in SRAM | Loops −22 to −25% (about 22 cycles per tile and 27 per 8 pixels, instead of 30 and 35); frame −2.4% |
| No extraction copy when `SCX & 7 == 0` | Small; depends on the game |
| Cached earliest device event; no event update after scroll or palette writes | Batman −5% (raster interrupts) |

A second pass on the runtime, measured on the same generated code (SML,
R.C. Pro-Am, Batman: 212,756 / 257,643 / 297,278 cycles per frame before,
204,098 / 238,600 / 274,564 after, i.e. −4.1%, −7.4% and −7.6%):

| Change | Main effect |
| --- | --- |
| Assembly `gbl_push`, `gbl_pop`, `gbl_rd`, `gbl_wr` (`gbl_core_asm.S`): fast path with no stack frame (GCC saved 2 to 4 registers before testing) | −2 to −3.5% (push/pop), up to −1.7% (rd/wr) |
| Assembly fine-scroll conversion straight from the tile words (`gbl_ppu_convert_fine_asm`, about 32 cycles per 8 pixels instead of 40); sprite priority read from the same words | SML −1.8% |
| Dispatch cache of 16 entries in front of the binary search (`GBL_LOOKUP_CACHE`) | Batman −2% |
| Inline check of the next PPU deadline (`gbl_ppu_due`) instead of a call | Batman −2% |
| Interpreter reads opcode and operands directly when they share a mapped page | R.C. Pro-Am −1.8% |
| Timer and serial event cached apart from the PPU part | Neutral (kept) |

Splitting the PPU transition loop into small functions was tried and
dropped (+1.5 to +4%).

Third pass, gm0-style (tables, pointers, shortcuts), measured at −1.3% on
SML:
- the STAT interrupt sources are precomputed when STAT is written, and the
  LYC match when LY or LYC change;
- sprite selection uses two pointers that slide from one line to the next
  over a Y-sorted copy, instead of a binary search;
- scanline drawing is split: the light decision (sprites, signature) is
  separate from `draw_line`, whose large frame only lines actually drawn
  pay for.

A branchless write of IF was tried and reverted: it rewrote IF three times
per line (+0.5%).

Fourth pass (profile, gm0 comparison, one change at a time, each checked
against the state and picture hashes of five games):

| Change | Effect |
| --- | --- |
| `EI` followed by a loop head: the instruction after `EI` is emitted a second time on the `EI` path, with the delayed interrupt poll after it, instead of polling inside the loop body | The wait-for-flag loop after `EI` is fast-forwarded again: Solar Striker −26% |
| HRAM (`FF80-FFFE`) read and written directly by the assembly `gbl_rd`/`gbl_wr` on the unmapped-page path; `E000-EFFF` (echo RAM) mapped | Solar Striker −8% |
| Sprites drawn from 8-bit masks (`lo \| hi` opaque, clipped once; an 8-pixel window of the "taken" bitmap read from two bytes; eight fixed tests instead of a per-pixel loop); sprite palettes cached per OBP0/OBP1 | `draw_line` −15% on SML |
| Background fetched, converted and hashed only left of the window (gm0's `bgDrawLimitX`) | Lines with a window: only the visible background |

Tried and dropped:
- re-sorting the previous Y order of the OAM instead of sorting from OAM
  order: slower on SML, whose OAM is nearly in Y order;
- taking the start of mode 3 (dot 80) only when observable (made exact after
  a Batman divergence: an LYC edge pending after LCD on). A single PPU
  advance already processes several lines in one loop, so it gained nothing
  and cost STAT reads more;
- inlining the page lookup of `gbl_rd`/`gbl_wr` in generated code: with
  `-Os` GCC does not inline it and adds a call (+3%).

A 24-byte array initializer in the sprite code had become a `memset` call:
it is now cleared with explicit stores. Every change keeps the state hash identical on five
games (compiled and interpreter-only runs). The assembly is checked with
the pixel hash under the Thumb emulator, against the C versions
(`GBL_PPU_ASM=0`, `GBL_CORE_ASM=0`).

Hand-written assembly pays off in the innermost PPU loops and the small
helpers called by generated code. The rest
of the time is spread across the runtime, the event machinery and the
generated code, where removing work (wait loops, events, flushes) gains
more.

Fifth pass: the event machinery and the per-line code in Thumb-1, with
the loops unrolled where the state allows it. Estimator, 1,600 frames,
generated code and input from `compile.sh` (`coverage-auto1.input` or
`auto.input`, the ROMs are not in the repository); the generated code is
newer than in the tables above, so only the two columns compare.
Each change below is measured against the step before it:

| Game | C runtime (`c9e1bfb`) | Assembly | Change |
| --- | --- | --- | --- |
| Super Mario Land | 137,117 | 101,144 | −26.2% |
| Super R.C. Pro-Am | 257,584 | 215,421 | −16.4% |
| Batman: The Video Game | 272,918 | 218,423 | −20.0% |
| Burai Fighter Deluxe | 77,235 | 37,069 | −52.0% |
| Solar Striker | 222,186 | 185,326 | −16.6% |

| Change | Main effect |
| --- | --- |
| `gbl_ppu_advance` in Thumb-1: a whole line is straight-line code; stage, mode and line start are stored once per call | SML −12.9%, Burai −28.8% |
| The line loop instantiated three times for the STAT line: no source (updates omitted), LYC only (only line changes checked), general | SML −4.1%, Burai −9.0% |
| Whole lines of a skipped frame only counted (about 9 cycles each), the window line moved once by the number of lines at or below WY | Burai −11.0%, SML −2.4% |
| `gbl_sync` with the timer inline (GCC saved nine registers before finding nothing had ticked); `gbl_update_evt` recomputing a dirty event itself | Batman −4.8% |
| The frame signature multiplies by `gbl_sig_mul` (the constant cost a 25-instruction shift/add chain per PPU register write) | Part of the line above |
| OAM DMA resolves its source page once and copies 16 bytes per LDM/STM iteration instead of 160 `gbl_rd` calls (about 4,800 cycles per DMA) | Burai −11.2%, SML −4.4% |
| `gbl_io_rd`/`gbl_io_wr` serve LY, STAT, scroll, palette and window registers in Thumb-1; `gbl_lookup` checks the dispatch cache without a frame | Batman −4.2%, R.C. Pro-Am −1.3% |
| `render_line` in Thumb-1 (sprites sorted as 16-bit keys `x << 8 \| index`), tile-hash loop and `gbl_cp` | −1% to −3% |

Ten more games (`tools/m0bench/panel.txt`: SD streaming, MBC2, raster
effects, many sprites, scrolling) give the same hashes with both runtimes.
With the direct bus paths below included, the change in average cycles
per frame on all fifteen ranges from −14% to −53% (median −19%); the time
freed is 3.9% to 11% of the 60 Hz frame budget (804k cycles at 48 MHz,
emulation only). Heavy frames gain less (p99 −6% to −23%). Frames over
the budget: Prehistorik Man 180 → 33 of 1,599, Donkey Kong Land 24 → 21,
R.C. Pro-Am 16 → 14, Pinball Dreams 6 → 3; in what remains the
interpreter dominates. `m0_estimate.py` prints these per-frame figures
(`--heavy N` profiles the frames over the budget only) and runs streaming
projects: the whole ROM stands in for the SD card (the SD transfer time is
not modelled), and the save file is kept in that region like the host's
in-memory one. (Earlier Zelda figures came from a benchmark that dropped
save-RAM writes: the game crashed into an `RST 38` loop.)

Unmapped accesses no longer go through `gbl_rd_slow`/`gbl_wr_slow`'s six
range tests: the Thumb-1 `gbl_wr` jumps straight to the MBC write,
`gbl_vram_wr` and `gbl_io_wr`, `gbl_rd` to `gbl_io_rd` and, for a streamed
ROM chunk, reads the last sector used; `map_rom_bank` rebuilds the pages
only when the bank changes. Zelda −7.8%, Prehistorik Man −4.3%.

The frames still over the budget spent 40% to 60% of their time in the
interpreter (code the recorded sessions never reached: loading,
transitions). Its loop is now Thumb-1 (`gbl_interp_asm.S`): event check,
fetch, cycles and a 256-entry table of halfword offsets, with the common
opcodes and the CB prefix in assembly and the rest through the C
`gbl_interp_op`, from which the assembly-handled cases are compiled out
(the whole change costs about 0.2 KiB of flash). A transfer to code
that is not compiled starts a new block in place instead of returning to
the dispatcher, with the same block boundaries (the SP bound of the
innermost `gbl_exec` is kept in `gbl_exec_sp0`). Interpreted blocks
averaged three instructions, so the return, the dispatcher loop and the
call cost as much as the loop itself. Every game run entirely
interpreted (`BENCH_INTERP_ONLY`, 800 frames) is 10% to 24% faster with
the same hashes; in normal runs R.C. Pro-Am −2.6%, F-1 Race −5.3%,
Prehistorik Man −3.3%, Donkey Kong Land p99 −15%.

The loop itself (event check, fetch, dispatch) then still took about two
thirds of the interpreter's time, some 66 cycles per instruction against
29 in the handlers (R.C. Pro-Am, `BENCH_INTERP_ONLY`). Now the cycle table
lives in a register, `gbr.cyc` is loaded once, operands are read only by
the handlers that need them, EI branches past the next event check
instead of a flag tested every instruction, and the 64-instruction block
limit is gone (blocks end at a transfer or an interrupt, the C loop too;
the instruction count is published at each event). The dispatch table
also decodes what handlers used to test: one handler per condition
(JR/JP/CALL/RET cc), per ALU operation and operand kind (register, (HL),
immediate, flags inline), register or (HL) for LD, INC, DEC and CB, and
LDH / LD (C) go straight to HRAM or `gbl_io_rd`/`gbl_io_wr`. Flash +0.4
KiB, RAM +4 bytes. Same pixel and state hashes on twelve games;
entirely interpreted (400 frames) −7% to −27% (R.C. Pro-Am, Faceball,
Prehistorik Man, DKL −23%); normal runs −0.1% to −3.9%, p99 up to −8%.
The register offsets and
`gbl_exec`'s bound now share one RAM structure (`gbl_interp_ctx`) that
`r8` points to, which removes two flash literal loads per transfer
(−0.1% to −0.6%).

Tried and dropped, on the same panel: remembering the address range
around the last transfer target with no compiled entry, to skip
`gbl_lookup` (±0.3% in normal runs, +2% interpreted: the range test on
every transfer cost about what it saved); the whole
interpreter in SRAM (no gain, 2.4 KiB of RAM); fast-forwarding interpreted
wait loops (traced on the host: the hottest interpreted loops are the OAM
DMA delay loop in HRAM, which the `DEC A / JR NZ` shortcut already skips,
and memory fills; the one real wait, Altered Space's LY loop, runs about
five iterations per frame).

Measure these with `BENCH_PIXEL_HASH` and `BENCH_STATE_HASH` off: hashing
every line and the machine state costs about 200k cycles per frame in the
benchmark and inflates the frames over the budget (R.C. Pro-Am 31 with
hashing, 1 without). Use the hashes for equivalence, a run without them
for cost. `--callers FUNC` counts a function's calls by caller (also in
the frames over the budget) and `--waits N` lists the functions with the
most flash wait cycles.

Without hashing, the dispatcher (`gbl_lookup`, its miss path and
`gbl_exec`) took 3% to 8% of the frame, mostly binary searches after
cache misses: the 32-entry cache missed 24% (R.C. Pro-Am) to 61% (Zelda)
of the lookups, by capacity (other hash functions and 2- or 4-way sets
simulated on recorded lookup streams did not help; size did). Each
bank's dispatch table now carries an index (`gbl_bank_table.idx`, 50
bytes of flash per bank with code): the first entry of each 1 KiB block,
and a bit per 128-byte page holding an entry. A lookup in a page with
nothing compiled, typically the interpreter's transfers inside
uncompiled code, returns at once and is not cached, so it no longer
evicts entries (Faceball's misses drop by 90%); a miss searches one
1 KiB block (about 5 steps instead of 9). The cache has 64 entries
(+256 bytes; 128 entries gain another 0.5 point for 512 more bytes).

The generated PlatformIO project builds with its own copy of the Arduino
Zero variant (`platform/variants/gbl_meta`, LGPL like the original)
without the hardware UART objects `Serial` and `Serial1`, which the META
never uses: −1,160 bytes of RAM and −1.8 KiB of flash. The firmware draws
the same screens in gamebuino-emulator. Most games had less free RAM
than the 3 KiB C stack budget plus the 768-byte interrupt headroom, so
the stack was getting less than 3 KiB.

Since the commit before the interpreter work (`c7fd04d`), same pixel and
state hashes on twelve games; per frame without hashing:

| Game | Before | Now | Change | p99 | Frames over the budget |
| --- | --- | --- | --- | --- | --- |
| Faceball 2000 | 178,042 | 162,808 | −8.6% | −15.4% | 32 → 7 |
| F-1 Race | 83,967 | 77,119 | −8.2% | −0.2% | 2 → 1 |
| Altered Space | 68,642 | 63,894 | −6.9% | −18.0% | 8 → 5 |
| Prehistorik Man | 417,143 | 391,875 | −6.1% | −10.8% | 21 → 5 |
| Double Dragon | 167,104 | 157,590 | −5.7% | −6.1% | 14 → 9 |
| Zelda | 250,420 | 236,620 | −5.5% | −4.3% | 0 → 0 |
| R.C. Pro-Am | 204,756 | 195,785 | −4.4% | −11.9% | 7 → 1 |
| Donkey Kong Land | 308,111 | 300,266 | −2.5% | −12.9% | 13 → 5 |
| Mystic Quest | 103,712 | 101,225 | −2.4% | −4.4% | 1 → 1 |
| Batman | 212,085 | 209,939 | −1.0% | −0.3% | 0 → 0 |
| Kid Icarus | 149,687 | 149,155 | −0.4% | −0.1% | 0 → 0 |
| Super Mario Land | 100,272 | 100,038 | −0.2% | −0.1% | 0 → 0 |

(Estimator cycles, 1,600 frames, `BENCH_LINE_SKIP` and
`BENCH_STATIC_SKIP`.)

More runtime code in SRAM does not pay: `gbl_rd`, `gbl_wr`, `gbl_push`
and `gbl_pop` there cost +2% to +4%, because generated code in flash
reaches them through long-branch veneers; the event and IO functions
(`gbl_update_evt`, `gbl_sync`, `gbl_io_rd`/`gbl_io_wr`, `gbl_lookup`)
gain nothing (±0.5%). The PPU loops gain because they run long without
calls back to flash.

The OAM DMA copy (`gbl_oam_dma_asm`, once a frame, about 400 cycles) has
nothing to gain from the SAMD21 DMAC. The audio interrupt had: TC4
interrupted 22,049 times a second, and its handler, entry and exit cost
about 65 cycles, some 3% of the CPU (the estimator does not model
interrupts). With `META_AUDIO_DMA` (default), each TC4 wrap triggers a
DMAC beat on channel 1 that copies the next ring entry, already a 10-bit
DAC value, to the DAC. Each frame's samples get a descriptor (two at the
ring's wrap) linked from the previous frame's, and the chain ends on an
invalid descriptor: when emulation falls behind, the DMAC stops there and
the DAC holds the last sample as the interrupt did, and the next frame
restarts the channel after 256 samples of it. The samples played while the
channel runs are derived from `micros()` (same 48 MHz clock as TC4) and
only bound what can be queued. The two channels share the descriptor table
(`meta_dma.cpp`). RAM +688 bytes (a 768-entry ring of 16-bit values, eight
descriptors); the frame refill runs before the save flush, which can
block on the card. The ring and chaining logic were simulated with
frame-time jitter, heavy frames and 40 ms to 300 ms stalls (no sample
replayed out of order, no queued descriptor reused); the DMAC itself has
only been exercised on hardware by the display so far: if the sound is
wrong on a META, build with `-DMETA_AUDIO_DMA=0` (in `build_flags`) to get
the interrupt back.

Ten more games (`panel.txt`: software 3D, isometric, non-identity BGP,
RPG menus, beat 'em up) found three more things: the ALU helpers called by
generated code (`gbl_add`, `gbl_adc`, `gbl_sub`, `gbl_sbc`, `gbl_inc`,
`gbl_dec`) are Thumb-1 now like `gbl_cp` (every input checked by
`BENCH_ALU_CHECK`; −0.5% on Kid Icarus: GCC's were close, the cost is the
calls themselves), the dispatch cache has 32 entries (Faceball 2000 −2%,
frames over the budget 67 → 32; 128 bytes of RAM), and SMOOTH's line check
no longer mixes the display mode into each signature (a mode change voids
them all). On the 25 games the runtime is 9.9% to 52.8% faster than the C
runtime of `c9e1bfb` (median 21.7%), with the same hashes.

All fifteen games against the C runtime of `c9e1bfb`:

| Game | C runtime | Now | Change | p99 change | Frames over budget |
| --- | --- | --- | --- | --- | --- |
| Super Mario Land | 137,117 | 100,256 | −26.9% | −8.7% | 0 → 0 |
| Super R.C. Pro-Am | 257,584 | 207,752 | −19.3% | −20.6% | 16 → 7 |
| Batman | 272,918 | 212,739 | −22.1% | −14.0% | 0 → 0 |
| Burai Fighter Deluxe | 77,235 | 36,483 | −52.8% | −10.7% | 0 → 0 |
| Solar Striker | 222,186 | 186,201 | −16.2% | −6.9% | 0 → 0 |
| Tetris | 88,966 | 52,037 | −41.5% | −21.1% | 0 → 0 |
| Zelda: Link's Awakening | 296,849 | 244,204 | −17.7% | −11.5% | 1 → 0 |
| Donkey Kong Land | 363,432 | 307,845 | −15.3% | −24.0% | 24 → 14 |
| Mystic Quest | 148,991 | 104,404 | −29.9% | −16.6% | 1 → 1 |
| F-1 Race | 130,285 | 85,964 | −34.0% | −6.1% | 3 → 2 |
| Nemesis | 434,194 | 364,454 | −16.1% | −10.5% | 2 → 0 |
| Prehistorik Man | 521,876 | 417,977 | −19.9% | −26.7% | 180 → 21 |
| Ninja Gaiden Shadow | 180,462 | 138,512 | −23.2% | −14.2% | 0 → 0 |
| Pinball Dreams | 321,150 | 261,718 | −18.5% | −17.2% | 6 → 1 |
| Castlevania II | 192,351 | 160,186 | −16.7% | −9.6% | 1 → 0 |

(Cycles per frame, 1,600 frames, 3 KiB of C stack as on the META; frames
over budget out of 1,599.)

The C stack budget changes results. With the META's 3 KiB the stack
guard unwinds compiled calls, and where it does depends on every C
frame size; in fast builds the unwound code then takes interrupts at
other instruction boundaries, so two correct builds can end in different
(valid) states. Batman on the host ends in four different states with
256, 3, 2 and 1 KiB of stack. Equivalence checks of runtime changes
therefore use `BENCH_BIG_STACK` (128 KiB of emulated RAM, no unwinding);
the fifteen games give the C runtime's hashes there.

Every step keeps the pixel, signature and state hashes of the five games
equal to the C runtime's (`BENCH_PIXEL_HASH`, `BENCH_STATE_HASH`; see
Validation). The firmware grows by about 1.2 KiB of flash (the three
copies of the line loop are 0.7 KiB) and 56 bytes of RAM; `compile.sh`
fills the flash by bisection, so a game at the limit keeps slightly less
cold compiled code. Nothing here has been measured on a device.

## Validation

```bash
ctest --test-dir build -R meta --output-on-failure
```

- `meta_alu_helpers_match_desktop_runtime` compares every ALU/flag helper,
  exhaustively, with the desktop runtime.
- `meta_ppu_matches_per_pixel_reference` compares 300 random VRAM/OAM/register
  states against a per-pixel DMG reference. The states cover the window,
  8×16 sprites, flips, the ten-sprite limit and priorities.
- `meta_target_end_to_end` generates a synthetic ROM, profiles it,
  regenerates, and requires:
  - the precise build to match the interpreter bit for bit;
  - the fast build to reach identical WRAM results;
  - the generator to recognise the ROM's polling and delay loops, and
    `GBL_SPIN=0` to give the same frames, WRAM and machine state (registers
    and cycle counter at every VBlank) as the default build.
  - a streaming build of the same ROM (forced, with one data chunk in flash,
    one read through a 2-sector cache) to give the same state as the flash
    build;
  - a packed build (`--meta-pack`, 4 KiB raw, 2-sector cache) to give the
    same state as the flash build, with sectors actually decoded (the data
    tables are compressible, so back-references are exercised);
  - the packed build run with 0 to 1024 bytes of C stack (`--stack-bytes`)
    to finish (60 s timeout, catches the dispatcher livelock) with the
    interpreter's frames and WRAM, and 1 KiB to keep the full machine state.
- `meta_fat_finds_streamed_rom` builds FAT16 and FAT32 images with
  `mkfs.fat` and mtools, in the root and in a directory, behind a partition
  table, with a lower-case name and a file in four pieces. It requires the
  file read through `meta_fat.c` to be byte-identical, and a missing file or
  too many pieces to be reported.

The host prints `state_hash`, a hash of the registers, cycle counter, WRAM,
HRAM and IO at every VBlank, next to `run_hash`, which covers the pictures.

The assembly cannot run on the host. Build the estimator image with
`--cflag=-DBENCH_PIXEL_HASH` (optionally with `--cflag=-DGBL_PPU_ASM=0`):
it prints a hash of every rendered line and of every line signature.
`--cflag=-DBENCH_STATE_HASH` adds a hash of the registers, cycle counter,
WRAM, HRAM, IO and PPU timing state at every VBlank (compare with
`GBL_TIMING_ASM=0`, `GBL_CORE_ASM=0`, `GBL_RENDER_LINE_ASM=0`), and
`--cflag=-DBENCH_ALU_CHECK` checks every (A, v) pair of `gbl_cp` against
the C formulas (`alu errors: 0`). On five games (1,600 frames each,
window and 8×16 sprites included), C and assembly print the same hashes.
Compare runtimes with `--cflag=-DBENCH_BIG_STACK` (see above: with 3 KiB
of stack the result depends on C frame sizes). `-DBENCH_INTERP_ONLY` runs
every instruction through the interpreter (compare `GBL_INTERP_ASM=0` and
1), and `-DBENCH_FRAME_HASHES` with `--dump 0x10300000:<4 * frames>` keeps
the state hash of every frame to find where two builds part.
`m0_estimate.py --func NAME` lists every executed block of a function with
its cost.

For a real game, compare the builds on the host with the same input:

```bash
game_host --interp --frames 3000 --input run.input   # reference
game_host --frames 3000 --input run.input            # META build
```

A `--meta-precise` project must print the same `run_hash` as `--interp`. The
fast build may differ only on isolated frames, because interrupts are taken
at slightly different instruction boundaries. `--dump-frame N file.ppm`,
`--wav file.wav` and `--hash-every 1` help narrow down differences.

The firmware also runs in
[gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
(load `.pio/build/meta/firmware.bin` in its page; keys: arrows, J = A,
K = B, U = MENU, I = HOME; sound is emulated: TC4 interrupts and DAC
writes are modelled and played through WebAudio; `headless.js` records
them to a WAV with its 5th argument; this needs a firmware built with
`-DMETA_AUDIO_DMA=0`, since the emulator does not run TC4-triggered DMA).
Use the fork in `tools/gb-emu/` (from upstream `9631729`, MIT; see its
`LISEZMOI.md`):
upstream could not execute code from SRAM, where our firmware runs its
PPU assembly (`GBL_PPU_RAMFUNC`) — firmwares crashed on the first
rendered frame — and had no SD card, so streamed games stopped on the
red « ROM introuvable » screen. The fork decodes SRAM lazily and
emulates a SPI-mode SD card backed by a raw FAT image or, built on the
fly, by a local directory (game saves are rewritten into the folder's
files). Streamed games boot and render there too. `headless.js` drives
it from Node and prints the dominant screen colors:
`node headless.js <firmware.bin> [ticks] [batches] <sd.img | directory>`;
the web page gains a file picker for the card image.

To estimate the cost on a Cortex-M0+ without hardware:

```bash
python3 -m pip install unicorn capstone pyelftools   # in a venv
python3 targets/meta/tools/m0bench/m0_estimate.py output/game_meta \
  --frames 600 --input run.input --hot-blocks 5
```

The estimator builds the runtime and generated code bare-metal, with the same
compiler and `-O2`, runs them under Unicorn, and applies the M0+ cycle model
plus flash wait states. It is an estimate for comparing variants, not a
hardware measurement.

## Status (Super Mario Land, measured on the host)

The input script is `targets/meta/examples/sml_walk.input`; the ROM is not part
of the repository.

| Check | Result |
| --- | --- |
| Covered instructions | 3,479, from 1,600 scripted frames |
| Interpreted after compilation | 4% of instructions |
| Precise build vs interpreter | Bit-identical over 3,000 frames (gameplay and attract demo) |
| Fast build vs interpreter | 1–2 frame hashes differ out of 3,000 |
| Frames vs desktop runtime | Same game states for the first 850 frames |
| Firmware size | 160.3 KiB flash (65%), 27.0 KiB RAM with ~5.8 KiB left for the stack |
| M0+ estimate | About 225 emulated frames/s on average with the default options (213k cycles per frame; 185 before the optimizations above) |

The desktop comparison shows a phase difference of one frame. Only the
first 850 frames were compared, because input timing makes the two runs
drift apart after that. The M0+ estimate was not measured on a device.

## Limitations

- DMG only. CGB-only ROMs are rejected, and CGB features are not emulated.
- Supported mappers: none, MBC1, MBC2, MBC3 (without RTC) and MBC5.
  Cartridge RAM lives in the META's 32 KiB, so large RAM carts may not fit.
  PlatformIO reports RAM use. Battery saves go to a `.SAV` file on the SD
  card (see above).
- Timing model:
  - mode 3 always lasts 172 dots;
  - scanlines are rasterized all at once;
  - OAM DMA completes instantly;
  - STOP acts as a NOP;
  - link-cable transfers complete with no partner;
  - sound is quantized to one frame.
- 144 lines onto 128: the default TBLEND merges 15 line pairs and drops
  line 143; streaming builds (TILE) drop 16 lines.
- ROMs of 256 KiB and more need their ROM file on the SD card (see above).
  The SD reader has not run on a device yet.
- Frames are skipped under load, at most `META_MAX_SKIP` in a row.
- In the `tools/gb-emu` fork, plain, packed and streamed builds boot and
  render (streamed ones from an emulated SD card); the upstream emulator
  needs `compile.mjs --emu` builds and cannot run streamed games. Buttons follow the Gamebuino library order (down,
  left, right, up, A, B, MENU, HOME), as the emulator does.
