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
from unicorn import UC_ARCH_ARM, UC_HOOK_BLOCK, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
from unicorn.arm_const import UC_ARM_REG_SP

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


def write_inputs(path, input_file):
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
            rows.append((int(frame), mask))
    if not rows:
        rows = [(0xFFFF, 0)]
    with open(path, "w") as f:
        f.write("static const struct { uint16_t frame; uint8_t buttons; } bench_inputs[] = {\n")
        for fr, m in rows:
            f.write("    {%d, %d},\n" % (fr, m))
        f.write("};\n")


def build(project, out_dir, frames, opt, input_file, extra):
    src = os.path.join(project, "src")
    os.makedirs(out_dir, exist_ok=True)
    write_inputs(os.path.join(out_dir, "bench_input.h"), input_file)
    sources = sorted(os.path.join(src, f) for f in os.listdir(src)
                     if (f.endswith(".c") or f.endswith(".S")) and (f.startswith("gbl_") or f.endswith("_gen.c") or f.endswith("_rom.c")))
    sources.append(os.path.join(HERE, "bench_main.c"))
    elf = os.path.join(out_dir, "bench.elf")
    cmd = [find_gcc(), "-mcpu=cortex-m0plus", "-mthumb", "-" + opt, "-g", "-std=gnu11",
           "-ffunction-sections", "-fdata-sections", "--param", "max-inline-insns-single=500",
           "-Wno-unused-label", "-DBENCH_FRAMES=%d" % frames, "-I", src, "-I", out_dir,
           "-nostartfiles", "--specs=nano.specs", "-Wl,--gc-sections",
           "-T", os.path.join(HERE, "bench.ld"), "-o", elf] + extra + sources
    subprocess.check_call(cmd)
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
    ap.add_argument("--build-dir", default=None)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--cflag", action="append", default=[], help="extra compiler flag")
    ap.add_argument("--hot-blocks", type=int, default=0, help="print the N most expensive basic blocks")
    ap.add_argument("--func", action="append", default=[],
                    help="print every executed block of this function, in address order")
    args = ap.parse_args()

    out_dir = args.build_dir or os.path.join(args.project, "m0bench")
    elf_path = build(args.project, out_dir, args.frames, args.opt, args.input, args.cflag)

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(0, 0x40000)
    uc.mem_map(0x20000000, 0x8000)
    symbols = []
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD" and seg["p_filesz"]:
                uc.mem_write(seg["p_paddr"], seg.data())
        symtab = elf.get_section_by_name(".symtab")
        done = None
        pixel_hash_addr = None
        state_hash_addr = None
        alu_errors_addr = None
        for s in symtab.iter_symbols():
            if s["st_info"]["type"] == "STT_FUNC" and s["st_size"]:
                symbols.append((s["st_value"] & ~1, s["st_size"], s.name))
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
    pc = int.from_bytes(uc.mem_read(0x4004, 4), "little")
    uc.reg_write(UC_ARM_REG_SP, sp)

    counts = collections.Counter()
    sizes = {}

    def on_block(_uc, addr, size, _data):
        counts[addr] += 1
        sizes[addr] = size

    uc.hook_add(UC_HOOK_BLOCK, on_block)
    t0 = time.time()
    uc.emu_start(pc | 1, done)
    wall = time.time() - t0

    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    per_func = collections.Counter()
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
            ws += 0 if in_ram else w
            ins += 1
        total_cyc += cyc * n
        total_ws += ws * n
        total_ins += ins * n
        i = bisect.bisect_right(starts, addr) - 1
        name = symbols[i][2] if i >= 0 and addr < symbols[i][0] + symbols[i][1] else "?"
        per_func[name] += (cyc + ws) * n
        per_block[addr] = (cyc + ws) * n
        block_func[addr] = name

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
    print("=> %.1f emulated frames/s at 48 MHz with every frame rendered (DMG: 59.7)" % (48e6 / per_frame))
    groups = collections.Counter()
    for name, c in per_func.items():
        if name.startswith("r") and "_" in name and name[1:].split("_")[0].isdigit():
            groups["generated code"] += c
        elif name in ("gbl_ppu_sync", "render_line", "decode8", "tile_row"):
            groups["PPU"] += c
        elif name == "gbl_interp_block":
            groups["interpreter"] += c
        else:
            groups["runtime/other"] += c
    print("\nshare by area:")
    for g, c in groups.most_common():
        print("  %-16s %5.1f%%  (%.0f cycles/frame)" % (g, 100 * c / est, c / frames))
    print("\ntop functions:")
    for name, c in per_func.most_common(args.top):
        print("  %-28s %5.1f%%" % (name, 100 * c / est))
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

