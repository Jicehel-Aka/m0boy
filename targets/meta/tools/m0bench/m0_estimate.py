#!/usr/bin/env python3
"""Estimate Gamebuino META (SAMD21, Cortex-M0+ @ 48 MHz) cost of a gbl project.

Builds a bare-metal image of the generated project's runtime and code with the
same compiler and optimisation level as the PlatformIO firmware, runs it under
Unicorn, and converts executed Thumb instructions into cycles with the
Cortex-M0+ timing model (loads/stores 2, BL 3, taken branches 2, LDM/STM/
PUSH/POP 1+N) plus one flash wait state per non-sequential fetch and literal
load (SAMD21 needs RWS=1 at 48 MHz).

This is an estimate, not a hardware measurement: it ignores cache hits,
DMA/bus contention, interrupts, and the Arduino core.  Use it to compare
variants and to find where time goes.

--target pokitto: the Pokitto (LPC11U68) instead: its display pipeline is
built in (platform/pokitto, the CPU writes every pixel to GPIO, counted like
any store), 72 MHz, and --flash-wait / --seq-wait describe its flash, whose
behaviour at 72 MHz is not modelled from documentation here: run both bounds
(--seq-wait 0 and 0.5) and label the result as a range.

Requires: unicorn, capstone, pyelftools; arm-none-eabi-gcc (PlatformIO's
toolchain-gccarmnoneeabi is found automatically).
"""
import argparse
import bisect
import collections
import os
import shutil
import subprocess
import sys
import time

from capstone import CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB, Cs
from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_SP

HERE = os.path.dirname(os.path.abspath(__file__))
BUTTONS = {"a": 1, "b": 2, "select": 4, "start": 8, "right": 16, "left": 32, "up": 64, "down": 128}


def find_gcc():
    gcc = shutil.which("arm-none-eabi-gcc")
    if gcc:
        return gcc
    cand = os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gcc")
    if os.path.exists(cand):
        return cand
    sys.exit("arm-none-eabi-gcc not found (install PlatformIO's atmelsam platform)")


def write_inputs(path, input_file, frames):
    rows = []
    if input_file:
        for line in open(input_file):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            frame, btn = line.split()[:2]
            mask = int(btn, 16) if btn.startswith("0x") else 0
            if not btn.startswith("0x"):
                for tok in btn.split("+"):
                    if tok != "none":
                        mask |= BUTTONS[tok]
            if int(frame) <= frames:  # later rows are never reached
                rows.append((int(frame), mask))
    if not rows:
        rows = [(0xFFFF, 0)]
    with open(path, "w") as f:
        f.write("static const struct { uint32_t frame; uint8_t buttons; } bench_inputs[] = {\n")
        for fr, m in rows:
            f.write("    {%d, %d},\n" % (fr, m))
        f.write("};\n")


def build(project, out_dir, frames, opt, input_file, extra, compiler="gcc"):
    src = os.path.join(project, "src")
    os.makedirs(out_dir, exist_ok=True)
    write_inputs(os.path.join(out_dir, "bench_input.h"), input_file, frames)
    # NES projects have no gbl_* files: the interpreter runtime, the
    # cartridge and a bench main of their own.
    nes = os.path.isfile(os.path.join(src, "nes.c"))
    if nes:
        nes_sources = {"nes.c", "6502.c", "ppu.c", "ppu_asm.S", "apu.c", "nes_cart.c", "nes_gen.c"}
        sources = sorted(os.path.join(src, f) for f in os.listdir(src) if f in nes_sources)
        sources.append(os.path.join(HERE, "bench_main_nes.c"))
        if os.path.isfile(os.path.join(src, "nes_gen.c")):  # recompiled 6502 code
            extra = extra + ["-DNES_NATIVE"]
    else:
        sources = sorted(os.path.join(src, f) for f in os.listdir(src)
                         if (f.endswith(".c") or f.endswith(".S")) and (f.startswith("gbl_") or f.endswith("_gen.c") or f.endswith("_rom.c")))
        sources.append(os.path.join(HERE, "bench_main.c"))
    # -DBENCH_META_DISPLAY=<mode>: the META display pipeline instead of the
    # benchmark's simple row policy.
    if any(f.startswith("-DBENCH_META_DISPLAY") for f in extra):
        plat = os.path.normpath(os.path.join(HERE, "..", "..", "platform"))
        sources += [os.path.join(plat, "meta", "gbl_display.c"), os.path.join(plat, "meta", "gbl_display_asm.S"),
                    os.path.join(plat, "src", "gbl_palettes.c")]
        extra = extra + ["-I", os.path.join(plat, "src"), "-I", os.path.join(plat, "meta")]
    # -DBENCH_POKITTO_DISPLAY=<mode> (--target pokitto): the Pokitto's.
    if any(f.startswith("-DBENCH_POKITTO_DISPLAY") for f in extra):
        plat = os.path.normpath(os.path.join(HERE, "..", "..", "platform"))
        pk = os.path.join(plat, "pokitto")
        sources += [os.path.join(pk, "pokitto_display.c"), os.path.join(pk, "pokitto_lcd.c"),
                    os.path.join(pk, "pokitto_lcd_asm.S"), os.path.join(plat, "src", "gbl_palettes.c")]
        extra = extra + ["-DGBL_TARGET_POKITTO=1", "-I", pk, "-I", os.path.join(plat, "src")]
    # Streaming projects: the whole ROM stands in for the SD card (placed in
    # its own region by bench.ld; the SD transfer time is not modelled).
    host = os.path.join(project, "host")
    if os.path.isdir(host):
        sources += sorted(os.path.join(host, f) for f in os.listdir(host) if f.endswith("_romfull.c"))
    elf = os.path.join(out_dir, "bench.elf")
    cmd = [find_gcc(), "-mcpu=cortex-m0plus", "-mthumb", "-" + opt, "-g", "-std=gnu11",
           "-ffunction-sections", "-fdata-sections", "--param", "max-inline-insns-single=500",
           "-Wno-unused-label", "-DBENCH_FRAMES=%d" % frames, "-I", src, "-I", out_dir,
           "-nostartfiles", "--specs=nano.specs", "-Wl,--gc-sections",
           "-T", os.path.join(HERE, "bench.ld"), "-o", elf] + extra + sources
    if compiler == "gcc":
        subprocess.check_call(cmd)
    else:
        # Compile with Clang, retain GNU assembly and the embedded link ABI.
        sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "toolchain")))
        from arm_clang import clang_command
        flags = ["-mcpu=cortex-m0plus", "-mthumb", "-" + opt, "-g", "-std=gnu11",
                 "-ffunction-sections", "-fdata-sections", "-Wno-unused-label",
                 "-DBENCH_FRAMES=%d" % frames, "-I", src, "-I", out_dir] + extra
        objects = []
        for i, source in enumerate(sources):
            obj = os.path.join(out_dir, "%d_%s.o" % (i, os.path.basename(source)))
            subprocess.check_call(clang_command(find_gcc(), "c", flags + ["-c", source, "-o", obj]))
            objects.append(obj)
        subprocess.check_call([find_gcc(), "-mcpu=cortex-m0plus", "-mthumb", "-nostartfiles",
                               "--specs=nano.specs", "-Wl,--gc-sections",
                               "-T", os.path.join(HERE, "bench.ld"), "-o", elf] + objects)
    return elf


def classify(insn):
    """Return (cycles, wait_states) for one Thumb instruction on a Cortex-M0+."""
    m = insn.mnemonic
    ops = insn.op_str
    if m in ("bl",):
        return 3, 1
    if m in ("bx", "blx"):
        return 2, 1
    if m == "b":
        return 2, 1
    if m.startswith("b") and len(m) in (3, 5) and m not in ("bic", "bics", "bkpt"):
        return 1.5, 0.5  # conditional: assume taken half of the time
    if m in ("push", "pop", "ldm", "stm", "ldmia", "stmia"):
        n = ops.count(",") + 1 if "{" in ops else 1
        if "-" in ops:
            n += 2
        if m == "pop" and "pc" in ops:
            return 3 + n, 1
        return 1 + n, 0
    if m.startswith("ldr") or m.startswith("str"):
        return 2, 1 if "pc" in ops else 0
    return 1, 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project", help="generated gbl project directory (contains src/)")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--input", help="gbl input script (frame buttons)")
    ap.add_argument("--opt", default="O2")
    ap.add_argument("--compiler", choices=("gcc", "clang"), default="gcc")
    ap.add_argument("--build-dir", default=None)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--cflag", action="append", default=[], help="extra compiler flag")
    ap.add_argument("--hot-blocks", type=int, default=0, help="print the N most expensive basic blocks")
    ap.add_argument("--dump", action="append", default=[], metavar="SYMBOL",
                    help="write the final contents of a data symbol (or ADDRESS:SIZE) to <build-dir>/<name>.bin")
    ap.add_argument("--frame-costs", metavar="FILE",
                    help="write the estimated cycles of every frame, one line per frame (frame number, cycles)")
    ap.add_argument("--heavy", type=int, default=0,
                    help="profile only the frames over the 60 Hz budget: print the N costliest functions")
    ap.add_argument("--func", action="append", default=[],
                    help="print every executed block of this function, in address order")
    ap.add_argument("--target", choices=("meta", "pokitto"), default="meta",
                    help="pokitto: its display pipeline, 72 MHz and its flash parameters")
    ap.add_argument("--display-mode", type=int, default=0,
                    help="--target pokitto: 0 1:1, 1 200x176, 2 200x176 entier")
    ap.add_argument("--mhz", type=float, default=None, help="CPU clock (META 48, Pokitto 72)")
    ap.add_argument("--flash-wait", type=float, default=None,
                    help="wait cycles per non-sequential flash fetch or literal load (META 1, Pokitto 2)")
    ap.add_argument("--seq-wait", type=float, default=0.0,
                    help="extra wait cycles per instruction fetched from flash sequentially")
    ap.add_argument("--waits", type=int, default=0, metavar="N",
                    help="print the N functions with the most flash wait cycles (candidates for SRAM)")
    ap.add_argument("--callers", action="append", default=[], metavar="FUNC",
                    help="count the calls of FUNC by calling function (all frames and, with --heavy, "
                         "the frames over the budget)")
    args = ap.parse_args()

    pokitto = args.target == "pokitto"
    mhz = args.mhz or (72.0 if pokitto else 48.0)
    fw = args.flash_wait if args.flash_wait is not None else (2.0 if pokitto else 1.0)
    sw = args.seq_wait
    cflags = list(args.cflag)
    if pokitto:
        cflags.append("-DBENCH_POKITTO_DISPLAY=%d" % args.display_mode)
    out_dir = args.build_dir or os.path.join(args.project, "m0bench")
    elf_path = build(args.project, out_dir, args.frames, args.opt, args.input, cflags, args.compiler)

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(0, 0x80000)
    big_stack = any(f == "-DBENCH_BIG_STACK" for f in args.cflag)
    uc.mem_map(0x20000000, 0x20000 if big_stack else 0x8000)
    uc.mem_map(0x10000000, 0x400000)  # stand-in SD card (bench.ld SDROM)
    if pokitto:
        uc.mem_map(0xA0000000, 0x5000)  # LPC GPIO: the display's pixel writes
    symbols = []
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD" and seg["p_filesz"]:
                uc.mem_write(seg["p_paddr"], seg.data())
        symtab = elf.get_section_by_name(".symtab")
        data_syms = {s.name: (s["st_value"], s["st_size"]) for s in symtab.iter_symbols()
                     if s["st_info"]["type"] == "STT_OBJECT"}
        done = None
        frame_fn = None
        pixel_hash_addr = None
        state_hash_addr = None
        alu_errors_addr = None
        for s in symtab.iter_symbols():
            if s["st_info"]["type"] == "STT_FUNC" and s["st_size"]:
                symbols.append((s["st_value"] & ~1, s["st_size"], s.name))
            if s.name == "bench_frame":
                frame_fn = s["st_value"] & ~1
            if s.name == "bench_done":
                done = s["st_value"] & ~1
            if s.name == "bench_pixel_hash":
                pixel_hash_addr = s["st_value"]
            if s.name == "bench_state_hash":
                state_hash_addr = s["st_value"]
            if s.name == "bench_alu_errors":
                alu_errors_addr = s["st_value"]
        text = elf.get_section_by_name(".text")
        flash_used = text["sh_size"]
    symbols.sort()
    starts = [s[0] for s in symbols]

    sp = int.from_bytes(uc.mem_read(0x4000, 4), "little")
    if big_stack:
        sp = 0x20020000  # see bench_main.c
    pc = int.from_bytes(uc.mem_read(0x4004, 4), "little")
    uc.reg_write(UC_ARM_REG_SP, sp)

    counts = collections.Counter()
    sizes = {}
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    # Running cycle total, for the cost of each frame (between two calls of
    # bench_frame, i.e. VBlank to VBlank).
    block_cost = {}
    running = [0.0]
    frame_marks = []
    budget = mhz * 1e6 / 59.7275
    frame_blocks = collections.Counter()  # blocks of the current frame (--heavy)
    heavy_blocks = collections.Counter()
    heavy_frames = [0]

    def on_block(uc_, addr, size, _data):
        counts[addr] += 1
        sizes[addr] = size
        c = block_cost.get((addr, size))
        if c is None:
            c = 0.0
            for insn in md.disasm(bytes(uc_.mem_read(addr, size)), addr):
                cy, w = classify(insn)
                c += cy + (0 if addr >= 0x20000000 else w * fw + sw)
            block_cost[(addr, size)] = c
        running[0] += c
        if args.heavy:
            frame_blocks[(addr, size)] += 1

    def on_frame(_uc, _addr, _size, _data):
        if args.heavy and len(frame_marks) >= 1 and running[0] - frame_marks[-1] > budget:
            heavy_blocks.update(frame_blocks)
            heavy_frames[0] += 1
            for f in args.callers:
                heavy_callers[f].update(frame_callers[f])
        frame_blocks.clear()
        for f in args.callers:
            frame_callers[f].clear()
        frame_marks.append(running[0])

    func_addr = {name: a for a, _, name in symbols}
    callers = {f: collections.Counter() for f in args.callers}
    frame_callers = {f: collections.Counter() for f in args.callers}
    heavy_callers = {f: collections.Counter() for f in args.callers}

    def caller_name(lr):
        i = bisect.bisect_right(starts, lr & ~1) - 1
        return symbols[i][2] if i >= 0 and (lr & ~1) < symbols[i][0] + symbols[i][1] else "%08x" % lr

    def on_call(uc_, addr, _size, fname):
        lr = uc_.reg_read(UC_ARM_REG_LR)
        name = caller_name(lr)
        callers[fname][name] += 1
        frame_callers[fname][name] += 1

    for f in args.callers:
        if f not in func_addr:
            sys.exit("--callers: no function %s" % f)
        uc.hook_add(UC_HOOK_CODE, on_call, f, begin=func_addr[f], end=func_addr[f])

    uc.hook_add(UC_HOOK_BLOCK, on_block)
    if frame_fn is not None:
        uc.hook_add(UC_HOOK_CODE, on_frame, begin=frame_fn, end=frame_fn)
    t0 = time.time()
    uc.emu_start(pc | 1, done)
    wall = time.time() - t0

    per_func = collections.Counter()
    func_ws = collections.Counter()
    per_block = collections.Counter()
    block_func = {}
    total_cyc = total_ws = total_ins = 0.0
    for addr, n in counts.items():
        code = bytes(uc.mem_read(addr, sizes[addr]))
        cyc = ws = ins = 0
        in_ram = addr >= 0x20000000  # SRAM code: no flash wait states
        for insn in md.disasm(code, addr):
            c, w = classify(insn)
            cyc += c
            ws += 0 if in_ram else w * fw + sw
            ins += 1
        total_cyc += cyc * n
        total_ws += ws * n
        total_ins += ins * n
        i = bisect.bisect_right(starts, addr) - 1
        name = symbols[i][2] if i >= 0 and addr < symbols[i][0] + symbols[i][1] else "?"
        per_func[name] += (cyc + ws) * n
        func_ws[name] += ws * n
        per_block[addr] = (cyc + ws) * n
        block_func[addr] = name

    for name in args.dump:
        if ":" in name:  # ADDRESS:SIZE
            a, n = name.split(":")
            addr, size, name = int(a, 0), int(n, 0), a
        else:
            addr, size = data_syms[name]
        with open(os.path.join(out_dir, name + ".bin"), "wb") as f:
            f.write(bytes(uc.mem_read(addr, size)))
    frames = args.frames
    if pixel_hash_addr is not None:
        print("pixel hash: %08x" % int.from_bytes(uc.mem_read(pixel_hash_addr, 4), "little"))
    if alu_errors_addr is not None:
        print("alu errors: %d" % int.from_bytes(uc.mem_read(alu_errors_addr, 4), "little"))
    if state_hash_addr is not None:
        print("state hash: %08x" % int.from_bytes(uc.mem_read(state_hash_addr, 4), "little"))
    est = total_cyc + total_ws
    per_frame = est / frames
    print("image: %s (.text %d bytes), %d frames, emulated in %.1fs" % (elf_path, flash_used, frames, wall))
    print("instructions: %.0f (%.0f per frame)" % (total_ins, total_ins / frames))
    print("estimated M0+ cycles per frame: %.0f core + %.0f flash wait = %.0f" %
          (total_cyc / frames, total_ws / frames, per_frame))
    print("=> %.1f emulated frames/s at %g MHz with every frame rendered (DMG: 59.7)" % (mhz * 1e6 / per_frame, mhz))
    if pokitto:
        print("   (Pokitto model: flash wait %g per non-sequential fetch, %g per sequential instruction; "
              "display mode %d; audio interrupt not included)" % (fw, sw, args.display_mode))
    if len(frame_marks) > 2:
        # Skip frame 0 (boot): costs of whole VBlank-to-VBlank frames.
        per = sorted(b - a for a, b in zip(frame_marks[1:], frame_marks[2:]))
        pick = lambda q: per[min(len(per) - 1, int(q * len(per)))]
        print("frame cost: median %.0f, p95 %.0f, p99 %.0f, max %.0f cycles; "
              "%.1f%% of the 60 Hz budget on average, %d frames over %.0f%% of it" %
              (pick(0.5), pick(0.95), pick(0.99), per[-1], 100 * sum(per) / len(per) / budget,
               sum(1 for c in per if c > 0.5 * budget), 50))
        print("frames over the 60 Hz budget (%.0f cycles, emulation only): %d of %d" %
              (budget, sum(1 for c in per if c > budget), len(per)))
        if args.frame_costs:
            with open(args.frame_costs, "w") as f:
                for i, (a, b) in enumerate(zip(frame_marks[1:], frame_marks[2:])):
                    f.write("%d %.0f\n" % (i + 1, b - a))
    groups = collections.Counter()
    for name, c in per_func.items():
        if (name.startswith("r") and "_" in name and name[1:].split("_")[0].isdigit()) or \
                (name.startswith("r_") and name[2:].isdigit()):  # NES regions: r_<n>
            groups["generated code"] += c
        elif name.startswith(("pokitto_", "gbl_platform_line", "meta_display", "meta_smooth", "smooth_")) or \
                name in ("at", "command", "data", "reg", "strip_slot", "claim_strip"):
            groups["display"] += c
        elif name in ("gbl_ppu_sync", "render_line", "decode8", "tile_row"):
            groups["PPU"] += c
        elif name == "gbl_interp_block":
            groups["interpreter"] += c
        elif name in ("nes6502_run", "nes6502_interp", "nes_bus_read", "nes_bus_write",
                      "nes_gen_lookup", "nes_gen_enter"):
            groups["6502 + bus"] += c
        elif name.startswith("nes_ppu") or name == "render_line_nes":
            groups["PPU"] += c
        elif name.startswith("nes_apu"):
            groups["APU"] += c
        else:
            groups["runtime/other"] += c
    print("\nshare by area:")
    for g, c in groups.most_common():
        print("  %-16s %5.1f%%  (%.0f cycles/frame)" % (g, 100 * c / est, c / frames))
    print("\ntop functions:")
    for name, c in per_func.most_common(args.top):
        print("  %-28s %5.1f%%" % (name, 100 * c / est))
    if args.heavy and heavy_frames[0]:
        hf = collections.Counter()
        for (addr, size), n in heavy_blocks.items():
            i = bisect.bisect_right(starts, addr) - 1
            name = symbols[i][2] if i >= 0 and addr < symbols[i][0] + symbols[i][1] else "?"
            hf[name] += block_cost[(addr, size)] * n
        htot = sum(hf.values())
        print("\nframes over the budget: %d, %.0f cycles each on average; top functions:" %
              (heavy_frames[0], htot / heavy_frames[0]))
        for name, c in hf.most_common(args.heavy):
            print("  %-28s %5.1f%%" % (name, 100 * c / htot))
    if args.waits:
        size_of = {name: size for _, size, name in symbols}
        print("\nflash wait cycles by function (what SRAM would save):")
        for name, w in func_ws.most_common(args.waits):
            print("  %-28s %5.1f%% of all cycles, %7.0f per frame, %5d bytes" %
                  (name, 100 * w / est, w / frames, size_of.get(name, 0)))
    for f in args.callers:
        print("\ncalls of %s: %.0f per frame; by caller:" % (f, sum(callers[f].values()) / frames))
        for name, n in callers[f].most_common(12):
            print("  %-28s %10.1f per frame" % (name, n / frames))
        if args.heavy and heavy_frames[0]:
            print("  in the frames over the budget: %.0f per frame" %
                  (sum(heavy_callers[f].values()) / heavy_frames[0]))
            for name, n in heavy_callers[f].most_common(12):
                print("    %-26s %10.1f per frame" % (name, n / heavy_frames[0]))
    for fname in args.func:
        print("\nfunction %s: %.1f%%" % (fname, 100 * per_func[fname] / est))
        for addr in sorted(per_block):
            if block_func[addr] != fname:
                continue
            print("  block %08x: %.2f%%, %d executions, %.1f cycles each" %
                  (addr, 100 * per_block[addr] / est, counts[addr], per_block[addr] / counts[addr]))
            for insn in md.disasm(bytes(uc.mem_read(addr, sizes[addr])), addr):
                print("    %08x  %-8s %s" % (insn.address, insn.mnemonic, insn.op_str))
    for addr, c in per_block.most_common(args.hot_blocks):
        print("\nblock %08x: %.1f%%, %d executions" % (addr, 100 * c / est, counts[addr]))
        for insn in md.disasm(bytes(uc.mem_read(addr, sizes[addr])), addr):
            print("    %08x  %-8s %s" % (insn.address, insn.mnemonic, insn.op_str))


if __name__ == "__main__":
    main()

