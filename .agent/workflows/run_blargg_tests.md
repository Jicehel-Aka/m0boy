---
description: Run Blargg's Test ROMs
---
This workflow recompiles and runs the specified Blargg test ROMs.

# 1. CPU Instructions
```bash
./build/bin/gbrecomp roms/cpu_instrs.gb -o output/cpu_instrs
cmake -G Ninja -S output/cpu_instrs -B output/cpu_instrs/build
ninja -C output/cpu_instrs/build
SDL_VIDEODRIVER=dummy ./output/cpu_instrs/build/cpu_instrs > logs/cpu_instrs.log 2>&1 &
```

# 2. Instruction Timing
```bash
./build/bin/gbrecomp roms/instr_timing.gb -o output/instr_timing
cmake -G Ninja -S output/instr_timing -B output/instr_timing/build
ninja -C output/instr_timing/build
SDL_VIDEODRIVER=dummy ./output/instr_timing/build/instr_timing > logs/instr_timing.log 2>&1 &
```

# 3. Memory Timing
```bash
./build/bin/gbrecomp roms/mem_timing1.gb -o output/mem_timing1
cmake -G Ninja -S output/mem_timing1 -B output/mem_timing1/build
ninja -C output/mem_timing1/build
SDL_VIDEODRIVER=dummy ./output/mem_timing1/build/mem_timing1 > logs/mem_timing1.log 2>&1 &
```

# 4. Halt Bug
```bash
./build/bin/gbrecomp roms/halt_bug.gb -o output/halt_bug
cmake -G Ninja -S output/halt_bug -B output/halt_bug/build
ninja -C output/halt_bug/build
SDL_VIDEODRIVER=dummy ./output/halt_bug/build/halt_bug > logs/halt_bug.log 2>&1 &
```
