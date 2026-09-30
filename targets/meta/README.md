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
| TILE (default) | Drops the last line of each tile row (8k+7), keeping lines 63 and 95; fonts usually leave that line blank, so text stays intact (taken from gm0) |
| BLEND | Merges lines 8 and 9 of every 9 with averaged colours (1 KiB table) |
| DARK | Same merge, keeping the darker shade |
| CROP | No scaling: DMG lines 8–135 |
| SKIP | Drops the 9th line of every 9 |

`meta_display_panel_matches_reference` runs the real display pipeline
against a simulated ST7735 (row windows included) and checks every mode
pixel for pixel over 400 frames of random changes. `<name>_metaview
--mode … --dump N file.ppm` writes the exact META screen of a game on the
host.

The buttons are read at 12 MHz in the Gamebuino library bit order. Build with
`-DMETA_BUTTONS_LAPINOU_ORDER` (and `-DMETA_BUTTONS_SPI_HZ=24000000`) for the
order observed when reading at 24 MHz.

## Saves and large cartridge RAM

Battery-backed cartridges (`GBL_BATTERY`, from header byte 0x147) keep
their cartridge RAM in a file on the SD card, `GBL_SAVE_NAME` (e.g.
`MQ.SAV`). `compile.sh` creates it at exactly the size of the RAM (filled
with 0xFF, an erased cartridge) and never overwrites it. Copy it to the
card once, in the root directory or one directory below: the console
rewrites its 512-byte sectors in place and never touches the FAT.

- RAM up to 2 KiB (MBC2's 512 half-bytes, for instance) stays in RAM. It is
  loaded at boot; writes go through the slow path and mark sectors dirty.
- Larger RAM (`GBL_ERAM_ON_SD`, the default above 2 KiB: 8 KiB for Link's
  Awakening, 32 KiB for Pokémon) lives only in the file, behind a two-sector
  write-back cache. Without that, those games did not fit in RAM (8 KiB on
  top of 29 KiB). Such builds, like streaming ones, keep two display strips
  and drop the BLEND and DARK modes. Zerd no Densetsu (8 KiB) builds at
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
  instead of three and drop the BLEND and DARK modes. RAM then stays at the
  usual level: 28.9 KiB.

`compile.sh` records the profiles, regenerates with them, and copies the ROM
next to the binary under its card name.

## Compressed ROM in flash (`--meta-pack`, `PACK=1`)

With `--meta-pack` (`PACK=1` for `compile.sh`), the streaming machinery
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
  decodes.

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

- Precompute: 8 random-input runs per ROM in parallel (`AUTO_SEEDS`), one of
  30 minutes of game time and seven of 10 minutes, about 11 s on 8 cores.
- Several ROMs run in parallel (`JOBS`, default cores / 4), each with its
  log in `out/<game>/compile.log`. Five ROMs take about 70 s on 16 cores.
- Every run, random or played (`PLAY=1`), is kept in
  `<rom dir>/profiles/<sha1>/` (`.input`, `.cov`, `.prof`) and reused by
  later runs. `AUTO_REDO=1` redoes the random runs, and `REPLAY=1` replays
  the played ones.

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
| `GBL_CORE_ASM` | 1 on Cortex-M0+ | `gbl_rd`, `gbl_wr`, `gbl_push`, `gbl_pop` in Thumb-1 (`gbl_core_asm.S`) |
| `GBL_LOOKUP_CACHE` | 16 | Entries of the dispatch cache (0 disables it) |
| `GBL_STREAM_SECTORS` | 4 | Sector cache of a streamed ROM (512 bytes each) |
| `GBL_ERAM_ON_SD` | 1 above 2 KiB of cartridge RAM | Cartridge RAM kept in the save file behind a 1 KiB cache |
| `GBL_ROM_PACKED` | set by `--meta-pack` | Streamed ROM sectors LZSS-compressed in flash instead of on the SD card |
| `GBL_UNPACK_ASM` | 1 on Cortex-M0+ | Thumb-1 decoder for those sectors |
| `GBL_STACK_GUARD` | 1 | C stack check on entry to every compiled region |
| `META_SWAP_AB` | 1 | META A = Game Boy B, META B = Game Boy A |
| `META_SD_SPI_HZ` | 24000000 | SPI clock for SD sector reads |
| `META_MERGE_MODES` | 1 (0 when streaming) | BLEND and DARK display modes |
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
    tables are compressible, so back-references are exercised).
- `meta_fat_finds_streamed_rom` builds FAT16 and FAT32 images with
  `mkfs.fat` and mtools, in the root and in a directory, behind a partition
  table, with a lower-case name and a file in four pieces. It requires the
  file read through `meta_fat.c` to be byte-identical, and a missing file or
  too many pieces to be reported.

The host prints `state_hash`, a hash of the registers, cycle counter, WRAM,
HRAM and IO at every VBlank, next to `run_hash`, which covers the pictures.

The assembly loops cannot run on the host. Build the estimator image with
`--cflag=-DBENCH_PIXEL_HASH` (optionally with `--cflag=-DGBL_PPU_ASM=0`):
it prints a hash of every rendered line. On five games (1,500 frames each,
window and 8×16 sprites included), C and assembly print the same hash.

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
[gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator) (load
`.pio/build/meta/firmware.bin` in its page; keys: arrows, J = A, K = B,
U = MENU, I = HOME; no sound, TC4 and the DAC are not emulated).
`tools/emu/meta_emu_run.ts` drives it headless for screenshots and a frame
rate in the emulator's 20-MIPS time model.

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
- The display drops every ninth DMG line to fit 144 lines into 128.
- ROMs of 256 KiB and more need their ROM file on the SD card (see above).
  The SD reader has not run on a device yet.
- Frames are skipped under load, at most `META_MAX_SKIP` in a row.
- The META frontend runs in gamebuino-emulator (title screen and gameplay,
  full speed in its 20-MIPS model) but has not yet run on a device in this
  repository's history. Buttons follow the Gamebuino library order (down,
  left, right, up, A, B, MENU, HOME), as the emulator does.
