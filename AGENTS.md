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
- MSX (`--system msx`): the SG-1000 runtime built with `SG_MSX` (slots through a
  table of 8 KiB windows, PPI, AY-3-8910 in `msx_ay.c`, BIOS simulated in C by
  `OUT (n),A` traps in `msx.c`; layout rule in `targets/common/runtime/m0_msx_layout.h`).
  `tests/test_msx_e2e.py` (synthetic cartridge `tests/msx_fixture.py`). The SG-1000
  firmware must stay identical byte for byte. Notes: `docs/MSX.fr.md`.
- PICO-8 (`--system pico8`): `recompiler/src/system/pico8/p8c.c` compiles a
  cartridge's Lua (femto8's reader and compiler, `FEMTO8_DIR`) to C;
  `targets/pico8/runtime/` (values, tables, collector, drawing, sound, API in
  `p8_natives.def`), `targets/pico8/platform/p8_main.cpp` (META/Pokitto/ESPboy
  frontend),
  `targets/pico8/host/p8_host.c` (harness). Rule: the screen must equal
  femto8's (`tests/test_pico8_e2e.py` holds a recorded hash; compare Celeste
  against a femto8 built with scripted input and no sleeping when touching the
  runtime), also with `-DP8_GC_STRESS`. Notes: `docs/PICO8.fr.md`.
- `recompiler/`: `gbrecomp`, from gb-recompiled. The M0 backend is
  `recompiler/src/codegen/meta_emitter.cpp` (`--target meta`, `--target pokitto`).
- `targets/meta/runtime/`: the `gbl` runtime shared by both consoles; every
  optimisation switch, per console, is in `gbl_profile.h` (one switch, one
  define, independent).
- `targets/common/platform/`: the frontend every console shares (option
  menu, save states, FAT, SD protocol, pad decoding); console facts in
  `m0_target.h`.  Common files are named `m0_*`, never `meta_*`.
- `targets/meta/platform/`: the Game Boy frontend of its own (`gb_main.cpp`,
  `gbl_save`, `gbl_stream`, `gbl_display`, `gbl_palettes`).
- `targets/consoles/meta/`, `targets/consoles/pokitto/`, `targets/consoles/espboy/`: bare-metal
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
- Bump `targets/meta/VERSION` (META) or `targets/consoles/pokitto/VERSION`
  when a change alters what that console's firmwares do.

To try firmwares without a console, use [gm0](https://github.com/r043v/gm0),
the META/Pokitto/ESPboy emulator kept in its own repository. For the ESPboy it
executes the firmware's own Xtensa code and emulates the chip's ROM and the
Espressif SDK by function: time there is an estimate. `ESPBOY_GM0=/path/to/gm0`
makes `tests/test_espboy_target.py` and `tests/test_sega_espboy.py` run each
built firmware in it (`ESPBOY_PIO` for the build).

- New output targets: pc (SDL2 window, the card as `sd.img`; the firmware
  builds and runs head first in CI — `tests/test_pc_target.py`, SDL2
  required), odroid (ODROID-GO, ESP32) and aka (Gamebuino AKA, ESP32-S3)
  share `targets/common/consoles/esp32/` and build through PlatformIO
  (`ODROID_PIO`/`AKA_PIO` env vars for the real build; `AKA_GM0` exercises
  the experimental AKA application machine with an ELF or matching symbols;
  ODROID is not emulated, device checks remain manual). Which system may generate
  for which console comes from the capability matrix in
  `recompiler/src/system.cpp` (`TargetFacts` vs `min_ram_bytes`) — see
  `--list-systems`/`--list-targets` and `docs/MULTI_SYSTEM.fr.md`.
- NES (`--target meta` and the desktop harness,
  `recompiler/src/system/nes/generator.cpp`, runtime in
  `targets/nes/runtime/`): 6502 with cycles, per-line 2C02 with sprite-0
  and loopy v/t, NROM only. The 6502 code is recompiled to C
  (`recompiler/src/system/nes/emitter.cpp`, `nes_gen.h`; the dispatcher
  and its interpreter fallback in `6502.c` under `NES_NATIVE`;
  `--nes-interp` for an interpreter-only project); profiles come from the
  harness (`--interp --coverage f`, then `--coverage f`). Rule: a
  `--meta-precise` project prints the `--interp` run_hash, from the static
  walk and with a profile (`tests/test_nes_e2e.py`, synthetic cartridges);
  with a ROM, SMB precise = interpreter with an input script. Regions never
  tail-call (Thumb-1 GCC does not): every exit returns to the dispatcher.
  SMB must render identically through the harness before and after a
  runtime change (hashes + PPM captures); APU changes: identical `--wav`. PC Engine is
  still at the inspection stage (`--info`, `--disasm` with the
  6502/HuC6280 decoders in `recompiler/src/cpu/`); no pipeline is wired.
  The AKA CORE v1.4 wiring follows the official Gamebuino_AKA_lib
  (`targets/consoles/aka/`, `docs/AKA.fr.md`).

## Hygiene

- No ROMs, built firmwares or ROM-derived data in the repository or in a
  release archive (`targets/meta/package/make_package.sh` refuses them).
- Generated projects go to `out/` or `output/`, logs and captures to `logs/`
  (all ignored).
- Do not claim compatibility, accuracy or performance beyond the evidence
  actually collected.
