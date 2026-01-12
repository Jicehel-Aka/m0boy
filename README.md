# GB Recompiled

A **static recompiler** for original GameBoy ROMs that translates Z80 assembly directly into portable, modern C code. Run your favorite classic games without a traditional emulator—just compile and play.

![Compatibility](https://img.shields.io/badge/compatibility-98.9%25-brightgreen)
![License](https://img.shields.io/badge/license-MIT-blue)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)

---

## Features

- **High Compatibility**: Successfully recompiles **98.9%** of the tested ROM library (1592/1609 ROMs) ** MOST OF THE GAMES ARE NOT FULLY PLAYABLE YET**
- **Native Performance**: Generated C code compiles to native machine code
- **Complete Runtime**: Includes a full GameBoy runtime library with:
  - Accurate PPU (graphics) emulation with scanline rendering
  - Audio subsystem (APU) with all 4 channels
  - Memory Bank Controller support (MBC1, MBC3, MBC5)
  - Timer, interrupts, and joypad input
- **SDL2 Platform Layer**: Ready-to-run with keyboard/controller input and window display
- **Debugging Tools**: Trace logging, instruction limits, and screenshot capture
- **Cross-Platform**: Works on macOS, Linux, and Windows (via CMake + Ninja)

---

## Quick Start

### Prerequisites

- **CMake** 3.15+
- **Ninja** build system
- **SDL2** development libraries
- A C/C++ compiler (Clang, GCC, or MSVC)

### Building

```bash
# Clone and enter the repository
git clone https://github.com/yourname/gb-recompiled.git
cd gb-recompiled

# Configure and build
cmake -G Ninja -B build .
ninja -C build
```

### Recompiling a ROM

```bash
# Generate C code from a ROM
./build/bin/gbrecomp path/to/game.gb -o output/game

# Build the generated project
cmake -G Ninja -S output/game -B output/game/build
ninja -C output/game/build

# Run!
./output/game/build/game
```

---

## Usage

### Basic Recompilation

```bash
./build/bin/gbrecomp <rom.gb> -o <output_dir>
```

The recompiler will:
1. Load and parse the ROM header
2. Analyze control flow across all memory banks
3. Decode instructions and track bank switches
4. Generate C source files with the runtime library

### Debugging Options

| Flag | Description |
|------|-------------|
| `--trace` | Print every instruction during analysis |
| `--limit <N>` | Stop analysis after N instructions |
| `--verbose` | Show detailed analysis statistics |

**Example:**
```bash
# Debug a problematic ROM
./build/bin/gbrecomp game.gb -o output/game --trace --limit 5000
```

### Runtime Options

When running a recompiled game:

| Option | Description |
|--------|-------------|
| `--input <script>` | Automate input from a script file |
| `--dump-frames <list>` | Dump specific frames as screenshots |
| `--screenshot-prefix <path>` | Set screenshot output path |

---

## How It Works

### 1. Analysis Phase
The recompiler performs static control flow analysis:
- Discovers all reachable code starting from entry points (`0x100`, interrupt vectors)
- Tracks bank switches to follow cross-bank calls and jumps
- Detects computed jumps (e.g., `JP HL`) and resolves jump tables
- Separates code from data using heuristics

### 2. Code Generation
Instructions are translated to C:
```c
// Original: LD A, [HL+]
ctx->a = gb_read8(ctx, ctx->hl++);

// Original: ADD A, B
gb_add8(ctx, ctx->b);

// Original: JP NZ, 0x1234
if (!ctx->flag_z) { func_00_1234(ctx); return; }
```

Each ROM bank becomes a separate C file with functions for reachable code blocks.

### 3. Runtime Execution
The generated code links against `libgbrt`, which provides:
- Memory-mapped I/O (`gb_read8`, `gb_write8`)
- CPU flag manipulation
- PPU scanline rendering
- Audio sample generation
- Timer and interrupt handling

---

## Compatibility

See [COMPATIBILITY.md](COMPATIBILITY.md) for the full test report.
Recompilation doesn't mean fully playable. Most of the games are not fully playable yet and some are not even playable.
Some working examples:
- Tetris
- Mickey Mouse (glitched graphics)

| Status | Count | Percentage |
|--------|-------|------------|
| ✅ SUCCESS | 1592 | 98.94% |
| ❌ RECOMPILE_FAIL | 1 | 0.06% |
| ⚠️ RUN_TIMEOUT | 1 | 0.06% |
| 🔧 EXCEPTION | 7 | 0.44% |

---

## Known Limitations

1. **RAM Execution**: Code that runs from RAM (self-modifying code, copied routines) cannot be statically recompiled. Games like `cpu_instrs.gb` use this technique.

2. **Computed Jumps**: Complex jump tables using `JP HL` require heuristic detection. If not resolved, some code paths may be missed.

3. **Timing Accuracy**: The recompiler prioritizes correctness over cycle-perfect timing. Some games with strict timing requirements may have minor glitches.

4. **Game Boy Color**: While MBC5 and basic banking are supported, full CGB-specific features (double-speed mode, CGB palettes) are not yet complete.

---

## Development

### Project Architecture

The recompiler uses a multi-stage pipeline:

```
ROM → Decoder → IR Builder → Analyzer → C Emitter → Output
         ↓           ↓            ↓
     Opcodes   Intermediate   Control Flow
               Representation   Graph
```

Key components:
- **Decoder** (`decoder.h`): Parses raw bytes into structured opcodes
- **IR Builder** (`ir_builder.h`): Converts opcodes to intermediate representation
- **Analyzer** (`analyzer.h`): Builds control flow graph and tracks bank switches
- **C Emitter** (`c_emitter.h`): Generates C code from IR

---

## License

This project is licensed under the MIT License.

**Note**: GameBoy is a trademark of Nintendo. This project does not include any copyrighted ROM data. You must provide your own legally obtained ROM files.

---

## Acknowledgments

- [Pan Docs](https://gbdev.io/pandocs/) - The definitive GameBoy technical reference
- [mgbdis](https://github.com/mattcurrie/mgbdis) - GameBoy disassembler (included in tools/)
- The gbdev community for extensive documentation and test ROMs
- [N64Recomp](https://github.com/Mr-Wiseguy/N64Recomp) - The original recompiler that inspired this project

---

## Contributing

Contributions are welcome! Please open an issue first to discuss major changes.

Areas of interest:
- Game Boy Color support improvements
- Audio accuracy enhancements
- Performance optimizations
- Debugging tools
- Improve compatibility with more ROMs