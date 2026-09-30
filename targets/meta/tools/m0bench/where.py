"""Run an m0_estimate bench image (<build-dir>/bench.elf) for a bounded
number of instructions and report where it spends its time (basic blocks
per function) and how many frames it completed.  Use it when an estimator
run never finishes: a stuck frame counter with one hot function points at a
livelock (this is how the gbl_exec stack-headroom bug was found).

    python3 where.py <build-dir>/bench.elf <instructions>
"""
import sys, collections
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_BLOCK
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_PC

elf = ELFFile(open(sys.argv[1], 'rb'))
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0, 0x40000)
uc.mem_map(0x20000000, 0x8000)
for seg in elf.iter_segments():
    if seg['p_type'] == 'PT_LOAD' and seg['p_filesz']:
        uc.mem_write(seg['p_paddr'], seg.data())
symtab = elf.get_section_by_name('.symtab')
syms = sorted((s['st_value'] & ~1, s['st_size'], s.name) for s in symtab.iter_symbols()
              if s['st_info']['type'] == 'STT_FUNC' and s['st_size'])
starts = [s[0] for s in syms]
import bisect


def name(a):
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and a < syms[i][0] + syms[i][1]:
        return syms[i][2]
    return '?'


cnt = collections.Counter()
uc.hook_add(UC_HOOK_BLOCK, lambda u, a, s, d: cnt.__setitem__(name(a), cnt[name(a)] + 1))
sp = int.from_bytes(uc.mem_read(0x4000, 4), 'little')
pc = int.from_bytes(uc.mem_read(0x4004, 4), 'little')
uc.reg_write(UC_ARM_REG_SP, sp)
gf = [s for s in symtab.iter_symbols() if s.name == 'gbl_frame'][0]['st_value']
try:
    uc.emu_start(pc | 1, 0xFFFFFFFF, count=int(sys.argv[2]))
except Exception as e:
    print('exception', e)
p = uc.reg_read(UC_ARM_REG_PC)
print('pc', hex(p), name(p), 'sp', hex(uc.reg_read(UC_ARM_REG_SP)),
      'frame', int.from_bytes(uc.mem_read(gf, 4), 'little'))
print(cnt.most_common(8))
