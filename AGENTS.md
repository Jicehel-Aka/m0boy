# Working on m0boy

m0boy recompiles Game Boy ROMs into native firmwares for the Gamebuino META
(SAMD21) and the Pokitto (LPC11U68). Start with `README.md`, then
`targets/meta/README.md` (technical notes) and `docs/META_STATUS_2026-09-30.md`
(state, evidence labels, open items).

## Layout

- `recompiler/include/m0boy/`, `recompiler/src/system.cpp`: source CPU/system
  registry and console-independent byte reader. `--system` is the source
  console; `--target` is the destination. See `docs/MULTI_SYSTEM.fr.md`.
- `recompiler/src/cpu/sm83/`: SM83 decoder, independent of the GB cartridge
  parser. `recompiler/src/system/gameboy/`: GB cartridge, bank mapping and
  analysis. Legacy `recompiler/rom.h` and `recompiler/decoder.h` are forwarding
  includes. `recompiler/src/cpu/z80/`: Z80 disassembly, sharing encoding
  helpers with SM83 in `include/m0boy/cpu/encoding.h`.
  `recompiler/src/system/sg1000/`: unbanked cartridge inspection, Z80 -> C
  recompiler (`emitter.cpp`, regions as in the META backend) and project
  generation. `targets/sg1000/runtime/`: runtime shared by recompiled code and
  the fallback Z80 interpreter (event timing, lazy TMS9918A rendering, SN76489,
  `sg_gen.h`); `targets/sg1000/platform/`: META/Pokitto frontend.
- SG-1000 rule: a `--meta-precise` project must print the same `run_hash` as
  `--interp` (host harness), and `out_mismatch=0` (device renderer = reference).
- `recompiler/`: `gbrecomp`, from gb-recompiled. The M0 backend is
  `recompiler/src/codegen/meta_emitter.cpp` (`--target meta`, `--target pokitto`).
- `targets/meta/runtime/`: the `gbl` runtime shared by both consoles; every
  optimisation switch, per console, is in `gbl_profile.h` (one switch, one
  define, independent).
- `targets/meta/platform/src/`: shared frontend (main loop, menu, saves,
  states, streaming, FAT, SD protocol, palettes); console facts in `meta_target.h`.
- `targets/meta/platform/meta/`, `targets/meta/platform/pokitto/`: bare-metal
  drivers and project templates of each console.
- `targets/meta/host/`, `targets/meta/tools/m0bench/`: host harness and the
  Cortex-M0+ cycle estimator.
- `compile.sh`, `tools/meta-compiler/`: batch conversion (shell and Node).
- `tools/gb-emu/`: gamebuino-emulator fork used for profiling runs.

## Build and test

Always use CMake with Ninja.

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

`meta_pokitto_target` also links the Pokitto firmware when
`arm-none-eabi-gcc` is available (PATH or PlatformIO's toolchain).

## Rules for generated behaviour

- A `--meta-precise` project must print the same `run_hash` as `--interp`
  for the same input; the PlatformIO build (`pio run -d <project>`) must
  still fit the RAM/Flash report.
- A change to the shared frontend must keep both consoles building. When it
  is meant to be META-neutral, compare `firmware.bin` with a build from the
  previous sources.
- Runtime changes touching dispatch, streaming or `--meta-pack` rely on the
  e2e packed build and its C-stack sweep (`--stack-bytes`, livelock timeout).
- Assembly paths cannot run on the host: compare them with the C versions on
  the estimator with `--cflag=-DBENCH_PIXEL_HASH`, and use
  `targets/meta/tools/m0bench/where.py` when an estimator run never finishes.
  SG-1000 renderer asm (`sg_render_asm.S`): build a firmware with
  `-DSG_SELFTEST` and read `sg_selftest_result` (1 = no differing line) in gm0;
  check both panel orders (`SG_PANEL_SWAP` 0 and 1).  Game Gear background
  asm (`gg_render_asm.S`): `-DGG_SELFTEST`, `gg_selftest_result`, likewise.
- Game Gear changes: `tests/test_gg_e2e.py` (synthetic banked cartridge,
  `tests/gg_fixture.py`) and, with ROMs, precise native = interpreter on the
  whole collection.
- Label `m0_estimate.py` numbers as estimates, never as device measurements.
- Bump `targets/meta/VERSION` (META) or `targets/meta/platform/pokitto/VERSION`
  when a change alters what that console's firmwares do.

To try firmwares without a console, use [gm0](https://github.com/r043v/gm0),
the META/Pokitto emulator kept in its own repository.

## Hygiene

- No ROMs, built firmwares or ROM-derived data in the repository or in a
  release archive (`targets/meta/package/make_package.sh` refuses them).
- Generated projects go to `out/` or `output/`, logs and captures to `logs/`
  (all ignored).
- Do not claim compatibility, accuracy or performance beyond the evidence
  actually collected.
