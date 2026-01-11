# GameBoy Recompiler Roadmap

> Last updated: January 9, 2026 (Analyzed Session)

## Overview

This document tracks the implementation progress of the GameBoy static recompiler based on the architecture defined in [ARCH.md](ARCH.md).

---

## Phase 1: Foundation ✅ COMPLETE

**Goal**: Minimal working recompiler for simple 32KB ROMs (no banking)

| Task | Status | Notes |
|------|--------|-------|
| Project setup (CMake, directory structure) | ✅ | CMake with C++20, modular structure |
| ROM loader with header parsing | ✅ | Title, MBC type, ROM size, checksums |
| MBC type detection | ✅ | Detects ROM ONLY, MBC1-5 |
| SM83 instruction decoder (~500 opcodes) | ✅ | Full opcode + CB-prefix support |
| Basic IR builder | ✅ | Converts decoded instructions to IR |
| CPU context structure | ✅ | GBContext with registers, flags, memory |
| Memory bus implementation (no banking) | ✅ | gb_read8/gb_write8 in runtime |
| C code emitter | ✅ | Generates compilable C from IR |
| Simple test ROM working | ✅ | Test ROM executes correctly |

**Milestone Achieved**: Successfully recompiles and runs a 32KB no-MBC ROM.

---

## Phase 2: Control Flow & Analysis ✅ COMPLETE

**Goal**: Proper function detection and control flow

| Task | Status | Notes |
|------|--------|-------|
| Control flow analyzer | ✅ | Builds CFG from decoded instructions |
| Jump/call target identification | ✅ | Tracks JP, JR, CALL targets |
| Basic block construction | ✅ | Blocks split at jumps/labels |
| Function boundary detection | ✅ | Functions from call_targets |
| Handle conditional jumps (JP cc, JR cc) | ✅ | Correct target calculation |
| RST vector handling | ✅ | RST 00-38 as functions |
| Reachability analysis | ✅ | DFS from entry point |
| Interrupt vector stubs | ✅ | VBlank, LCD, Timer, Serial, Joypad |

**Milestone Achieved**: Handles ROMs with multiple functions, loops, and conditional branches.

---

## Phase 3: Bank Switching ✅ COMPLETE

**Goal**: Support for MBC1/MBC3/MBC5 games

| Task | Status | Notes |
|------|--------|-------|
| Bank tracker implementation | ✅ | Tracks rom_bank in GBContext |
| MBC1 support | ✅ | Bank register at 0x2000-0x3FFF |
| MBC3 support | 🔲 | Pokémon games, includes RTC |
| MBC5 support | ✅ | Same as MBC1 for basic banking |
| Cross-bank call detection | ✅ | Detects jumps between banks |
| Per-bank function generation | ✅ | func_XX_YYYY naming |
| Runtime bank dispatch | ✅ | gb_dispatch with bank switch |
| RAM banking support | ✅ | Basic ERAM with ram_bank |

**Milestone**: Tetris DX (512KB, 32 banks) recompiled successfully.

---

## Phase 4: PPU (Graphics) ✅ COMPLETE

**Goal**: Visual output

| Task | Status | Notes |
|------|--------|-------|
| Background rendering | ✅ | Tile-based with scroll |
| Window rendering | ✅ | Overlay window layer |
| Sprite rendering (8x8, 8x16) | ✅ | OAM-based with priority |
| Scanline timing | ✅ | Mode 0/1/2/3 transitions |
| VBlank interrupt | ✅ | Sets IF bit 0 |
| LCD STAT interrupt | ✅ | LYC=LY and mode interrupts |
| VRAM access timing | 🔲 | Not enforced (low priority) |
| Palette handling | ✅ | BGP, OBP0, OBP1, DMG green |
| SDL2 rendering backend | ✅ | ARGB8888, 3x scaling |
| OAM DMA transfers | ✅ | Via 0xFF46 write |

**Status**: Graphics render correctly (DMG mode). CGB colors missing.

---

## Phase 5: Interrupts & Timing ✅ COMPLETE

**Goal**: Accurate timing and interrupt handling

| Task | Status | Notes |
|------|--------|-------|
| Full interrupt controller | ✅ | VBlank/STAT/Timer/Joypad dispatch |
| Joypad input | ✅ | SDL keyboard mapped to P1 register |
| Cycle-accurate yielding | ✅ | gb_tick advances PPU |
| Timer (DIV, TIMA, TMA, TAC) | ✅ | Full timer implementation |
| Timer interrupt | ✅ | IF bit 2 on TIMA overflow |
| Joypad interrupt | ✅ | IF bit 4 on button press |

**Target**: Timing-sensitive games work.

---

## Phase 6: Audio � IN PROGRESS

**Goal**: Sound output

| Task | Status | Notes |
|------|--------|-------|
| Channel 1 (Pulse + sweep) | � | Structure exists, no sweep/envelope logic |
| Channel 2 (Pulse) | � | Basic structure exists |
| Channel 3 (Wave) | � | Wave RAM implemented, playback partial |
| Channel 4 (Noise) | � | Structure exists, LFSR missing logic |
| Audio mixing | 🔲 | Rudimentary callback, not fully hooked up |
| SDL2 audio backend | � | Callback in `platform_sdl.c` but disabled |

**Current State**: `audio.c` exists with register logic, `platform_sdl.c` has disabled audio init. No sound output yet.

---

## Phase 7: Polish & Optimization 🔲 NOT STARTED

**Goal**: Production quality

| Task | Status | Notes |
|------|--------|-------|
| IR optimization passes | 🔲 | Const prop, dead code elim |
| Test ROM compatibility | � | "Hybrid" interp mode helps significantly |
| Commercial game testing | 🔲 | |
| Debug overlay (ImGui) | 🔲 | |
| Performance profiling | 🔲 | |
| Save state support | 🔲 | |
| Save file support | 🔲 | Battery-backed RAM |

---

## Special: Hybrid Architecture ✅ COMPLETE

**Goal**: Support dynamically executed code (Test ROMs)

| Task | Status | Notes |
|------|--------|-------|
| Interpreter Fallback | ✅ | `gb_interpret` handles uncompiled code |
| Hybrid Dispatcher | ✅ | `gb_dispatch` calls interpretation if needed |
| Serial Output | ✅ | `0xFF02` writes print to stdout |

**Result**: `cpu_instrs.gb` and other test ROMs can run mixed static/dynamic code.

---

## Future Enhancements (Post-MVP)

| Feature | Status | Priority |
|---------|--------|----------|
| Game Boy Color support | 🔲 | High (CGB Palettes needed) |
| Link cable emulation | 🔲 | Low |
| Debugger integration | 🔲 | Medium |

---

## Analysis & Next Steps (Jan 9, 2026)

Based on codebase analysis:

1.  **Audio**: Primary target. Code exists (`audio.c`) but is incomplete. SDL backend needs to be enabled and mixing logic finished.
2.  **CGB Colors**: `ppu.c` only supports DMG palettes. Need to implement `0xFF68-0xFF6B` registers for CGB support.
3.  **Verification**: Test ROMs like `cpu_instrs.gb` should be verified with the new hybrid architecture.

**Immediate To-Do**:
1.  Complete Audio (Enable SDL backend, implement mixing).
2.  Verify `cpu_instrs.gb` output matches expectations.
