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
| Runtime (gbl) | `targets/meta/runtime/` — `gbl_core.c` (bus, dispatch, streaming, packed ROM, saves), `gbl_cpu.c` (interpreter), `gbl_ppu.c`, `gbl_apu.c`, asm in `gbl_core_asm.S` / `gbl_ppu_asm.S` / `gbl_timing_asm.S` (field offsets in `gbl_timing_layout.h`), switch values per console in `gbl_profile.h`, rules in `gbl_options.h` |
| Frontend | `targets/meta/platform/src/` (shared: main loop, menu, saves, states, stream, FAT, SD protocol), `platform/meta/` (META drivers: startup, SPI, LCD DMA, audio, SD bus, clock; bare metal), `platform/pokitto/` (Pokitto drivers) |
| Host harness | `targets/meta/host/gbl_host.c` (`_host`, `_profile`, `_play` targets) |
| Tests | `ctest -R meta` (8 tests); `tests/test_meta_target_e2e.py` is the main gate |
| Tools | `targets/meta/tools/m0bench/` (`m0_estimate.py` M0+ cycle model, `where.py` stall finder), `tools/gb-emu/` (gamebuino-emulator fork: SRAM execution, emulated SD card, `headless.js`), `tools/meta-compiler/` (Node port of `compile.sh`) |
| Batch | `compile.sh` (reference), `tools/meta-compiler/` (Node/ink port by another agent; uncommitted, in progress) |

## Done

- Coverage-driven compact generation, wait-loop fast-forward, EI duplication,
  lazy devices, asm bus/stack/PPU paths, sprite masks, fixed-palette tables.
- ROMs of 256 KiB and more stream from the SD card (4 KiB resident chunks
  chosen from data-read profiles, 8-sector LRU cache since 2026-10-03).
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

## Pokitto target (2026-10-03)

`--target pokitto` (and `TARGET=pokitto ./compile.sh`): same runtime,
generated code and frontend, bare-metal LPC11U68 drivers in
`targets/meta/platform/pokitto/`, console switches in
`platform/src/meta_target.h`, Makefile build with PlatformIO's
arm-none-eabi-gcc. Evidence: emulator only (`tools/meta-emu-sdl --target
pokitto`, a scratch copy with ACMD41 accepted for the card): Alleyway and
Link's Awakening (streamed, battery RAM file) run, C/menu/save/load of a
state work; menu "Ecran" adds gm0's 200x176 enlargement. The META firmware
from the shared files is byte-identical to the previous templates'
(Alleyway). Not run on a Pokitto; display, buttons, 72 MHz, flash wait
states and the loader jump match gm0 (`~/dev/gm0`, the author's Pokitto
Game Boy emulator, run on hardware), audio and the loader file layout do
not.
Pixel writes are CPU-driven (no DMA), in Thumb-1 from SRAM; estimator
(`m0_estimate.py --target pokitto`, four games): 2.0–3.5 ms a frame at
72 MHz against 2.5–3.6 ms for the META at 48 MHz, display 6–17% of it (see
targets/meta/README.md, Pokitto).

## META without the Arduino core (2026-10-03)

The META firmware is bare metal like the Pokitto's: `platform/meta/`
`meta_startup.c` (vector table, the Arduino Zero core's SystemInit clock
sequence, SysTick, the core's APBC clocks, VTOR and watchdog as
`initVariant` did) and `meta_spi.c` (SERCOM4 as the core's SPI library
set it, same `BAUD = 24 MHz / rate - 1`), CMSIS headers through
`platform_packages`; no framework, no variant, no USB stack (no serial
status line; `pio run -t upload` needs the bootloader already running).
The core capped SPI at 12 MHz: every firmware so far ran display, card
and buttons at 12 MHz. The display now runs at 24 MHz by default
(`META_LCD_SPI_MHZ`, the rate meta-lapinou drives the panel at on a META);
the menu's "SPI ecran" offers 12, 24, 36 and 48 MHz (SERCOM4 on its own
generator 6, from the PLL at 72 or 96 MHz for 36 and 48; 36 at most with
the CPU at 72). Buttons stay at 12 (at 24 the bits shift and UP is lost),
the card at 12 (`META_SD_SPI_HZ`). In `tools/gb-emu` (no PLL model) a
36/48 request falls back to 24 and the game runs.
Donkey Kong Land: −15.0 KiB flash, −1,880 B RAM; the RAM pays for
`GBL_STREAM_SECTORS` 8 (4 with 2 KiB of cartridge RAM in RAM), streamed
builds keep ~1.8 KiB of stack (2.0 before). Evidence: `ctest -R meta`,
Tetris and Donkey Kong Land (4 and 8 sectors) draw the same screens as
the Arduino builds in `tools/gb-emu`; RAM checked on packed (Addams
Family, Aretha MBC2, Battle Dodge Ball) and streamed (DKL, Link's
Awakening) builds. Not run on a META: clocks, DAC, SPI, the 72 MHz SPI
rates, the loader return.

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
- `output/gbrecomp-meta.zip`: built by `targets/meta/package/make_package.sh
  <profiles_dir>` from a git revision (its CMakeLists.txt, META.md and
  LISEZMOI.txt live in that folder; the version there, 0.2.0, is stamped
  into firmwares). Source package for testers (no ROMs, no
  binaries; built from a copy of the tree, validated by extracting it,
  building, running its tests and `compile.sh` on Tetris with and without
  `PACK=1`). `output/meta-sd-complete.zip`: personal SD image (contains
  ROMs; never distribute).

## Open items

- The C stack budget changes fast-build results: the stack guard's
  unwinding depends on C frame sizes (Batman ends in four states with 256,
  3, 2 and 1 KiB on the host). Valid states, but it makes host (large
  stack) and device (3 KiB) runs differ; equivalence checks use the M0
  benchmark's `BENCH_BIG_STACK`. The e2e test's "1 KiB keeps the full
  machine state" holds for its synthetic ROM, not in general.

- Frames still over the 60 Hz budget (estimator, without bench hashing,
  2026-10-01): Faceball 7, Double Dragon 9, Prehistorik Man 5, Donkey
  Kong Land 5, Altered Space 5, R.C. Pro-Am 1 of 1,599, mostly interpreted
  code. 2026-10-02: in those 1,600 frames from boot they are boot and
  screen transitions (blank screens, logos, "MISSION 1"), which the player
  does not see as slowdown. Over frames 3,000 to 12,000 of the same
  sessions, at the real flash budget (`compile.sh` bisection), none of
  Faceball, Double Dragon, R.C. Pro-Am or Altered Space goes over; Prehistorik
  Man 5, DKL 3, Contra 3 (Prehistorik Man and DKL are heavy in general:
  920 and 140 frames above 80% of the budget, interpreter about 6%). On a
  held-out random session (seed 99): Double Dragon 6, others 0 or 1.
  All of these games are flash-bound: 2,100 to 3,300 compiled instructions
  of 50,000 to 90,000 recorded. Measure gameplay with
  `--frames 12000 --frame-costs FILE` and skip the first 3,000 frames.
- Random runs (2026-10-02): one per core, full speed, 20 s of wall time
  (`AUTO_SECONDS`), inputs cut at the last frame played; the 20 s choice
  and its measurements are in the README (Batch conversion).
- Coverage (2026-10-02): `compile.sh` now adds the missing seeded auto
  sessions to ROMs recorded by the old single-session version (228 of
  728 stores had one session). Eight sessions against one, held-out
  session: Faceball −10.4% per frame, Contra −1.4%, Double Dragon −0.8%
  (over budget 6 → 4), Altered Space +1.0%. Tried and dropped: ranking
  compiled code by load-weighted executions (each frame's counts times
  that frame's instruction count): Double Dragon 6 → 0 frames over budget
  but +1% to +2.4% per frame on Double Dragon, Altered Space and R.C.
  Pro-Am, held-out and in-sample.
- Audio by DMA (`META_AUDIO_DMA`, default since 2026-10-01: TC4-triggered
  DMAC channel 1, chained descriptors) replaces the 22 kHz TC4 interrupt
  (~3% of the CPU). Simulated and built, not yet heard on a META: test it
  first on hardware; `-DMETA_AUDIO_DMA=0` restores the interrupt.
- Remaining M0 profile after the fifth pass: `render_line` and the tile
  fetch/convert loops on games that redraw (SML ~35% together), the
  interpreter on R.C. Pro-Am (~14%, a coverage question), the next-event
  recomputation on raster-heavy Batman (~11%), and generated code.

- Run on hardware: SD streaming, saves, asm paths, frame pacing, and now
  the bare-metal startup (clocks, DAC, SPI rates, HOME back to the loader).
- On a META: "SPI ecran" at 24, 36 and 48 MHz (frame rate in the menu's
  info line), then `-DMETA_SD_SPI_HZ=24000000` for the card.
- Option menu (HOME held ½ s, 2026-10-01): save states in `<NAME>.STA`,
  volume, palettes, frame skip, speed, 72 MHz (untested on a META), restart;
  checked in gamebuino-emulator and by the host `--state-test`.
- Packed builds have not run in gamebuino-emulator yet.
- Decide whether `PACK=1` should become the default for ROMs under 256 KiB
  whose code does not fit (measure more games than R.C. Pro-Am).
- `roms/bin/` should be regenerated with the current runtime and layout.
- Unrelated suite failures seen on 2026-09-30: `accuracy_runner_exit_policy`
  (empty PPU catalogue locally) and `crystal_challenge_rules_are_bounded_and_deterministic`.

## 2026-10-05 (soir) — 0.6.0, conversions et optimisations mesurées

Suite au rapport du jour (`META_EMU_RAPPORT_2026-10-05.md`) ; tout le
travail de session est commité en petites unités.

- **Versions par console** : META `targets/meta/VERSION` = 0.6.0, Pokitto
  `targets/meta/platform/pokitto/VERSION` = 0.6.0 (tampon `GBL_GEN_VERSION`
  par console, `--pokitto-version`, `compile.sh` interroge la version de son
  `TARGET`).  `TARGET=both` enchaîne les deux consoles, sélection demandée
  une seule fois ; sans TARGET, la console est demandée dans un terminal.
- **`build_flags` par jeu** : `<sortie>/<Jeu>/build_flags` (ex.
  `-D META_TBLEND=0`) est appliqué à chaque passe.  TBLEND faisait passer
  4 jeux streaming sous 2 Ko de pile (1668 o : DKL, Super DK GB, Pokemon
  Jaune, Zelda) ; avec le flag ils tiennent à 2340 o (Mystic Quest garde
  TBLEND, 2196 o).
- **Reconversions** : Gargoyle's Quest (0.4.0 Arduino du 02-10) et les 5
  jeux streaming sont en 0.6.0.  Émulateur : écrans corrects, 0 « pc fou »,
  Gargoyle `famine=0.0% relances=0` (famine audio terminée).  **Rappel** :
  valider les firmwares via leur `.BIN` (ou un zip) — passer un
  répertoire-carte ne charge pas le firmware (jamais supporté, ne pas
  confondre avec un écran noir).
- **Optimisations mesurées (T6/T7 du rapport, obsolètes)** : Mario Land et
  Zelda tournent à `relances=0 famine=0.0%` sur une minute ; Zelda : CPU
  libre ≈ 38 % (meta_micros 32,8 %, `gbl_interp_block` 2 %) — la boucle
  d'attente STAT/LY du rapport est couverte par la couverture complète.
  RING/LATENCY audio inchangés : les agrandir coûterait de la RAM (Zelda
  repasserait sous 2 Ko) sans gain mesurable.
- **Reste** : T8 (APU en Thumb-1 : `gbl_apu_render` 13,3 % sur Zelda,
  cible < 8 %) — réécriture assembleur à faire avec test d'équivalence ;
  T9 (WFI) ; T4 (validation sur vraie console) toujours en attente.
- **T13 clos** : `accuracy_runner_exit_policy` (`.FF...` : catalogues
  blargg absents) et `crystal_challenge_rules...` (aucune sortie) échouent
  de façon identique à l'état 9a65835, avant tout le travail du jour —
  préexistants et hors META.
- Tests hors META : les 2 échecs ci-dessus seulement ; `ctest -R meta` 12/12.

### 2026-10-05 (nuit) — gel du wrap corrigé, % au titre, rebuilds allégés

- **Gel définitif au wrap du tick** (lapinou frame ~7000, % wasm figé à 0) :
  la boucle de frame comparait `tick < cible` non signé — après le passage
  de 2^32 (89,5 s à 48 MHz) la cible rebasée passait SOUS le tick et plus
  rien ne s'exécutait.  Comparaison signée `(int32_t)(tickCount - cible)` ;
  lapinou vérifié sur 15 000 frames (~3 wraps).  Le % de vitesse est écrit
  dans le titre de la fenêtre native.
- **Boucle de frame batchée** (`step_batch`) : appel direct de step au lieu
  du pointeur — wasm node 34,6 -> 31,6 s / 3 600 frames (188 %), natif
  20,4 -> 19,3 s ; hash écran identique ; wasm + standalone régénérés.
  Gain suivant (bloc) : pré-décodage T16.
- **`.gbrmeta` garde le remplissage (« bpi »)** : un rebuild de version
  vérifie l'ancien remplissage et tente un cran de plus (2 builds au lieu
  de ~7) ; repli sur la recherche complète sinon.  Le profilage (sessions
  par sha1) était déjà partagé entre consoles et conservé aux montées de
  version — un rebuild n'est plus « une nuit ».

### 2026-10-05 (nuit II) — pokitto : horloge fidèle, timers par échéance

- **Flèches du pad pokitto** : fonctionnent (clavier, croix manette,
  EMU_INPUT) — la chaîne BTN -> pk_btn_gpio -> GPIO P1 est saine ; le test
  doit se faire avec un firmware 0.6.0 (un .bin d'avant la réécriture bare
  metal du 03-10 ne lit pas les boutons pareil).  Le stick droit alimente
  les ADC DAT8/9 (jeux lib), comme la référence.
- **Horloge pokitto fidèle** : le cœur est cadencé à la fréquence que le
  firmware configure (IRC 12 MHz, sinon 12 x M du PLL) au lieu de 45 MHz en
  dur — un jeu 72 MHz tournait à 62,5 %, son étiré de 60 %.  Mesuré :
  timer audio à 22 050 Hz exacts.
- **Timers LPC par échéance** : traités à chaque instruction (4-5x le coût
  du reste), maintenant à leur prochaine échéance avec invalidations
  (écritures CT/SysTick, COUNTFLAG part au ré-enable).
- **% du titre/HUD** : bug d'unité (secondes contre millisecondes) depuis
  le passage au timing fidèle — 0 % en permanence ; corrigé, le % est aussi
  dans le titre de la fenêtre native.
- **Reste** : le cœur pokitto atteint ~63-79 % du temps réel sur un
  Ryzen 6850H (accès mémoire LPC sans fast-path ; le META fait 295 %) —
  le son reste donc légèrement étiré sur les jeux 72 MHz tant que le cœur
  ne tourne pas à 100 % (fast-path mémoire = travail T16).

### 2026-10-05 (nuit III) — pokitto jouable : 62-80 % -> 98 %

Mesuré sur le lot « pokitto-gb.zip » de l'utilisateur (SML, Tennis,
Mystic Quest, SolarStriker, Zelda).  Trois causes, trois correctifs :

1. **Fast-path mémoire** : flash et SRAM principale du LPC = les mêmes
   tampons que le cœur META — accès directs au lieu du grand switch
   (pk_reg_peek/pk_reg_write) à chaque lecture/écriture.  MMIO, banques
   2 Ko et fault d'écriture flash gardent la voie lente (effets de bord).
2. **`meta_micros` sans division** (runtime pokitto, vaut pour la vraie
   console) : `(SYST_RVR - val) / clock_mhz` appelait `__udivsi3` à
   chaque tour du limiteur de frames — 47 % du cœur.  Pré-multiplication
   `us_mul` comme le META et son `frac_mul`.
3. **Croix directionnelle manette pokitto** absente de `pad_button_mask`.

Le sélecteur « Jeux » du navigateur est opérationnel : le JS et les
fonctions C existaient, les exports wasm manquaient.  Reste ~2 % sous le
temps réel (pacing) ; le fast-path des MMIO chaudes (LCD) est la suite
naturelle si on veut passer au-dessus.

### 2026-10-06 — pokitto à 48 MHz nominal (datasheet), tempo du son corrigé

Le défaut pokitto était l'overclock 72 MHz (META_CLOCK_DEFAULT = 72,
_OSCT=2 de PokittoLib) : hors datasheet (LPC11U6x : 50 MHz max) et — sur
les machines où l'hôte n'exécute pas 72 M ticks/s — jeu et son au ralenti
(son « deux fois trop rapide », 6,9 frames GB par frame émulée mesurées
par comptage des vblanks du firmware).  Défaut : 48 MHz nominal
(SYSPLLCTRL 0x23, comme PokittoLib) ; le 72 reste au menu « Processeur ».
L'émulateur cadence le cœur émulé sur la config réelle (pk_core_hz :
IRC 12 MHz hors PLL, 12 x M sinon — MAINCLKSEL + SYSPLLCTRL).

Audit timing face à la datasheet : SysTick sur l'horloge CPU (RVR =
MHz x 1000 - 1, 1 ms) ; CT32B0 sur PCLK = système (SYSAHBCLKDIV = 1),
MR1 = PCLK/22050 ; SSP (carte SD) divisé de PCLK, recalé à chaque
changement d'horloge ; pas de DMA dans le runtime (affichage et audio
pilotés CPU/GPIO).  Mesures post-correctif (SML 0.6.1) : RVR 47999,
ms_count 999,7 Hz, 58,9 fps GB, ~98 % du temps réel sur les cinq jeux
du lot utilisateur.

VERSION pokitto : 0.6.1 (meta_micros sans division + horloge 48 MHz
par défaut changent les firmwares) ; la META reste 0.6.0.  Reconvertir :
`TARGET=pokitto UPDATE=1 ./compile.sh` — le cache « bpi » et les sessions
partagées rendent l'opération rapide.

**Diagnostic « tout va trop vite » (son + vidéo)** : le % du titre n'est
plus borné à 100 — il montre la vraie vitesse.  Causes possibles d'un
>100 % : `EMU_NOPACE=1` dans l'environnement, ou binaire de l'émulateur
périmé face aux firmwares (avant pk_core_hz, le pokitto était cadencé à
45 MHz en dur : un firmware 72 MHz y tournait à 160 %).  Le build actuel,
mesuré sur SML 0.6.1 paced : 98 % du temps réel, 58,9 fps GB.

**Vérification tempo finale (2026-10-06)** : avec le binaire à jour et des
firmwares 0.6.1, la sonde EMU_DMA_TRACE mesure, chez le développeur ET sur
la machine de l'utilisateur, ms_count 999,9 Hz, 59,46 fps GB en temps
émulé, 59,73 fps GB mural (3300 frames / 55,25 s), et 21 726 latches
audio/s portant la musique (110k-1,1 M latches sonores par minute).
Le tempo pokitto est correct ; le « trop rapide » historique venait du
72 MHz par défaut (0.6.0) et/ou d'un cadencement 45 MHz en dur côté
émulateur, deux défauts corrigés.  Reste : la reprise franche audio (à
sec sous 100 % de pacing) est ajoutée ; le resampling fin (±2 %) n'est
pas nécessaire au tempo.
