# Gamebuino META target: status and handoff (2026-09-30)

Where the `--target meta` work stands, so it can be picked up from a clean
context. The reference for design, options and workflows is
[targets/meta/README.md](../targets/meta/README.md); this file only records
state, evidence and open items.

## Branch

`meta-target` (not pushed, not merged). Commits are authored as
`r043v <noferov@gmail.com>` with a `Co-Authored-By: Claude` trailer.

| Area | Where |
| --- | --- |
| Generator | `recompiler/src/codegen/meta_emitter.cpp`, options in `recompiler/include/recompiler/codegen/meta_emitter.h`, CLI in `recompiler/src/main.cpp` (`--target meta`, `--meta-*`, `--rom-profile`) |
| Runtime (gbl) | `targets/meta/runtime/` — `gbl_core.c` (bus, dispatch, streaming, packed ROM, saves), `gbl_cpu.c` (interpreter), `gbl_ppu.c`, `gbl_apu.c`, asm in `gbl_core_asm.S` / `gbl_ppu_asm.S` / `gbl_timing_asm.S` (field offsets in `gbl_timing_layout.h`), every switch in `gbl_options.h` |
| META frontend | `targets/meta/platform/src/` (LCD DMA, audio, SD/FAT, saves, stream) |
| Host harness | `targets/meta/host/gbl_host.c` (`_host`, `_profile`, `_play` targets) |
| Tests | `ctest -R meta` (7 tests); `tests/test_meta_target_e2e.py` is the main gate |
| Tools | `targets/meta/tools/m0bench/` (`m0_estimate.py` M0+ cycle model, `where.py` stall finder), `tools/emu/` (gamebuino-emulator driver) |
| Batch | `compile.sh` (reference), `tools/meta-compiler/` (Node/ink port by another agent; uncommitted, in progress) |

## Done

- Coverage-driven compact generation, wait-loop fast-forward, EI duplication,
  lazy devices, asm bus/stack/PPU paths, sprite masks, fixed-palette tables.
- ROMs of 256 KiB and more stream from the SD card (4 KiB resident chunks
  chosen from data-read profiles, 4-sector LRU cache).
- Battery saves (`.SAV` on the card, write-back cache above 2 KiB).
- A/B swap (`META_SWAP_AB`), firmware version stamp (`gbrecomp-meta x.y.z`
  string, read back by `compile.sh`).
- `compile.sh`: parallel random sessions, session store keyed by ROM SHA-1
  (`roms/gb/profiles/<sha1>/`), per-game folders in `roms/meta/<Game>/`,
  icons, version-aware rebuilds, `PACK=1`.
- `--meta-pack`: LZSS-compressed ROM sectors in flash with a Thumb-1 decoder
  (`GBL_ROM_PACKED`, `GBL_UNPACK_ASM`).
- Dispatcher livelock on a nearly exhausted C stack fixed
  (`GBL_EXEC_HEADROOM`), guarded by the e2e stack sweep.
- Fifth optimisation pass, Thumb-1 (`GBL_TIMING_ASM`,
  `GBL_RENDER_LINE_ASM`, more of `GBL_CORE_ASM`): PPU line state machine
  unrolled per line with quiet / LYC-only / general copies and counted
  skipped lines, `gbl_sync`, next-event recomputation, PPU register
  reads/writes, dispatch-cache hit, `render_line`, page-resolved OAM DMA,
  `gbl_cp`. Estimator: −16% to −52% cycles per frame on five games, same
  hashes as the C runtime. Firmware +1.2 KiB flash, +56 B RAM.

## Evidence (label these correctly)

- Host: `state_hash` / `run_hash` equality between builds for the same input
  (`--interp` vs compiled, plain vs streamed vs packed, `GBL_SPIN=0`, stack
  budgets). Fast mode only guarantees WRAM results; `--meta-precise` must
  match `--interp` frame for frame.
- M0+ numbers come from `m0_estimate.py` (Unicorn + cycle model + flash wait
  states). They are estimates for comparing variants, never device
  measurements. Asm vs C equivalence on M0 uses `--cflag=-DBENCH_PIXEL_HASH`
  (pixels and every line signature) and `--cflag=-DBENCH_STATE_HASH`
  (machine and PPU timing state at every VBlank), plus
  `--cflag=-DBENCH_ALU_CHECK` for the exhaustive `gbl_cp` check; build the
  reference with the `GBL_*_ASM` switches at 0. The host tests only see
  the C versions.
- Nothing in this branch has been measured on a physical META. The SD reader,
  saves and streaming have run on the host and FAT images only; the firmware
  runs in gamebuino-emulator.

Latest estimator results are in the README (fourth optimisation pass and the
packed R.C. Pro-Am table).

## Comparing variants on the estimator

Copy a generated project's `src/*_gen.c`, `src/*_rom.c` and
`src/gbl_config.h` plus the runtime under test into a fresh directory, then:

```bash
python3 targets/meta/tools/m0bench/m0_estimate.py <dir> --frames 600 \
  --input <session.input> --build-dir <dir>/b \
  --cflag=-DBENCH_LINE_SKIP --cflag=-DBENCH_STATIC_SKIP [--cflag=-D...]
```

Runs are independent and can go in parallel. If one never finishes, run
`where.py <dir>/b/bench.elf 20000000`: a frozen frame counter with one hot
function is a livelock, not slowness.

## Local artifacts (not in git)

- `roms/gb/` ROMs and `roms/gb/profiles/<sha1>/` recorded sessions
  (`coverage-*.cov`, `coverage-*.input`, `romdata-*.prof`).
- `roms/bin/`: 235 firmwares from an older runtime and the older flat layout;
  current `compile.sh` writes `roms/meta/<Game>/`.
- `output/gbrecomp-meta.zip`: source package for testers (no ROMs, no
  binaries; built from a copy of the tree, validated by extracting it,
  building, running its tests and `compile.sh` on Tetris with and without
  `PACK=1`). `output/meta-sd-complete.zip`: personal SD image (contains
  ROMs; never distribute).

## Open items

- Remaining M0 profile after the fifth pass: `render_line` and the tile
  fetch/convert loops on games that redraw (SML ~35% together), the
  interpreter on R.C. Pro-Am (~14%, a coverage question), the next-event
  recomputation on raster-heavy Batman (~11%), and generated code.

- Run on hardware: SD streaming, saves, asm paths, frame pacing.
- Packed builds have not run in gamebuino-emulator yet.
- Decide whether `PACK=1` should become the default for ROMs under 256 KiB
  whose code does not fit (measure more games than R.C. Pro-Am).
- `roms/bin/` should be regenerated with the current runtime and layout.
- Unrelated suite failures seen on 2026-09-30: `accuracy_runner_exit_policy`
  (empty PPU catalogue locally) and `crystal_challenge_rules_are_bounded_and_deterministic`.
