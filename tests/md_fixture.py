#!/usr/bin/env python3
"""Seeded random 68000 programs for the Mega Drive PC recompiler test.

Valid code only: data registers take anything; memory goes through
absolute work-RAM addresses, d16(A0), d8(A0,A0.w), (A1)+, -(A2), d16(PC)
and the stack, with A0-A2 reset at the top of each loop.  The main loop
also calls a subroutine, waits for the vertical interrupt in an idle loop,
and takes exceptions (division by zero, CHK, TRAP, TRAPV) whose handlers
return.  Vertical and horizontal interrupts count in RAM.  A recompiled
project must end every run on the interpreter's run_hash."""
import random

RAM_A0, RAM_A1, RAM_A2 = 0xFF1000, 0xFF4000, 0xFF6000


def be16(v):
    return (v & 0xffff).to_bytes(2, "big")


class Asm:
    def __init__(self, size=0x8000):
        self.rom = bytearray(size)
        self.pc = 0

    def words(self, *ws):
        for w in ws:
            self.rom[self.pc:self.pc + 2] = be16(w)
            self.pc += 2


def ea_bits(mode, reg):
    return (mode << 3) | reg


class Gen:
    def __init__(self, rng, asm, data_at):
        self.rng = rng
        self.a = asm
        self.data_at = data_at

    # operands: (mode, reg, ext-words builder(size, pc_of_ext) -> list)
    def ram_abs_w(self):
        addr = self.rng.randrange(0xFF8000, 0xFFC000, 2)   # below the stack
        return (7, 0, lambda size, at: [addr & 0xffff])

    def ram_abs_l(self):
        addr = self.rng.randrange(0xFF3000, 0xFF3800, 2)
        return (7, 1, lambda size, at: [addr >> 16, addr & 0xffff])

    def d16a0(self):
        d = self.rng.randrange(0, 0x800, 2)
        return (5, 0, lambda size, at: [d])

    def d8a0x(self):
        d = self.rng.randrange(-64, 64, 2) & 0xff
        return (6, 0, lambda size, at: [0x8000 | d])          # d8(A0,A0.w)

    def pcrel(self):
        target = self.data_at + self.rng.randrange(0, 0x100, 2)
        return (7, 2, lambda size, at: [(target - at) & 0xffff])

    def imm(self):
        v = self.rng.getrandbits(32)
        return (7, 4, lambda size, at: [v >> 16, v & 0xffff] if size == 4 else [v & (0xff if size == 1 else 0xffff)])

    def dreg(self):
        n = self.rng.randrange(8)
        return (0, n, lambda size, at: [])

    def mem(self, word_only=False):
        choices = [self.ram_abs_w, self.ram_abs_l, self.d16a0, self.d8a0x]
        if word_only:
            choices += [lambda: (3, 1, lambda s, at: []), lambda: (4, 2, lambda s, at: [])]
        return self.rng.choice(choices)()

    def emit(self, op, *operands, size=2):
        """op word, then each operand's extension words in order."""
        self.a.words(op)
        for _, _, ext in operands:
            for w in ext(size, self.a.pc):
                self.a.words(w)

    def src(self, size, allow_imm=True):
        kinds = [self.dreg, lambda: self.mem(word_only=size != 1), self.pcrel]
        if allow_imm:
            kinds.append(self.imm)
        return self.rng.choice(kinds)()

    def dst(self, size):
        return self.rng.choice([self.dreg, lambda: self.mem(word_only=size != 1)])()

    def instruction(self):
        r = self.rng
        size = r.choice([1, 2, 4])
        sz2 = {1: 0, 2: 1, 4: 2}[size]
        kind = r.randrange(30)
        if kind == 0:                                     # MOVE
            s, d = self.src(size), self.dst(size)
            top = {1: 0x1000, 2: 0x3000, 4: 0x2000}[size]
            self.emit(top | (d[1] << 9) | (d[0] << 6) | ea_bits(s[0], s[1]), s, d, size=size)
        elif kind == 1:
            self.a.words(0x7000 | r.randrange(8) << 9 | r.randrange(256))
        elif kind in (2, 3):                              # ADD/SUB/AND/OR/CMP <ea>,Dn
            top = r.choice([0xd000, 0x9000, 0xc000, 0x8000, 0xb000])
            s = self.src(size)
            self.emit(top | r.randrange(8) << 9 | sz2 << 6 | ea_bits(s[0], s[1]), s, size=size)
        elif kind == 4:                                   # Dn,<mem> and EOR Dn,<ea>
            top = r.choice([0xd000, 0x9000, 0xc000, 0x8000, 0xb000])
            d = self.mem(word_only=size != 1) if top != 0xb000 else self.dst(size)
            self.emit(top | r.randrange(8) << 9 | (4 + sz2) << 6 | ea_bits(d[0], d[1]), d, size=size)
        elif kind == 5:                                   # immediate ALU
            k = r.choice([0, 1, 2, 3, 5, 6])
            d = self.dst(size)
            self.a.words((k << 9) | sz2 << 6 | ea_bits(d[0], d[1]))
            v = r.getrandbits(32)
            self.a.words(*([v >> 16, v & 0xffff] if size == 4 else [v & (0xff if size == 1 else 0xffff)]))
            for w in d[2](size, self.a.pc):
                self.a.words(w)
        elif kind == 6:                                   # ADDQ/SUBQ
            d = self.dst(size)
            self.emit(0x5000 | r.randrange(8) << 9 | r.randrange(2) << 8 | sz2 << 6 | ea_bits(d[0], d[1]), d, size=size)
        elif kind == 7:                                   # NEGX/CLR/NEG/NOT/TST
            d = self.dst(size)
            self.emit(0x4000 | r.choice([0, 2, 4, 6, 10]) << 8 | sz2 << 6 | ea_bits(d[0], d[1]), d, size=size)
        elif kind in (8, 9):                              # shifts on Dn, by immediate or register
            self.a.words(0xe000 | r.randrange(8) << 9 | r.randrange(2) << 8 | sz2 << 6 | r.randrange(2) << 5 |
                         r.randrange(4) << 3 | r.randrange(8))
        elif kind == 10:                                  # memory shift, one bit
            d = self.mem(word_only=True)
            self.emit(0xe0c0 | r.randrange(4) << 9 | r.randrange(2) << 8 | ea_bits(d[0], d[1]), d)
        elif kind == 11:                                  # MULU/MULS/DIVU/DIVS (Dn may be zero)
            s = r.choice([self.dreg, self.imm, self.d16a0])()
            top = r.choice([0xc0c0, 0xc1c0, 0x80c0, 0x81c0])
            self.emit(top | r.randrange(8) << 9 | ea_bits(s[0], s[1]), s, size=2)
        elif kind == 12:                                  # SWAP, EXT, EXG
            n = r.randrange(8)
            self.a.words(r.choice([0x4840 | n, 0x4880 | n, 0x48c0 | n, 0xc140 | r.randrange(8) << 9 | n]))
        elif kind == 13:                                  # bit operations
            dynamic = r.randrange(2)
            d = self.dst(1) if r.randrange(2) else self.dreg()
            op = (0x0100 | r.randrange(8) << 9) if dynamic else 0x0800
            self.a.words(op | r.randrange(4) << 6 | ea_bits(d[0], d[1]))
            if not dynamic:
                self.a.words(r.randrange(256))
            for w in d[2](1, self.a.pc):
                self.a.words(w)
        elif kind == 14:                                  # Scc
            d = self.dst(1)
            self.emit(0x50c0 | r.randrange(16) << 8 | ea_bits(d[0], d[1]), d, size=1)
        elif kind == 15:                                  # ADDX/SUBX/ABCD/SBCD on Dn
            top = r.choice([0xd100, 0x9100, 0xc100, 0x8100])
            sz = sz2 if top in (0xd100, 0x9100) else 0
            self.a.words(top | r.randrange(8) << 9 | sz << 6 | r.randrange(8))
        elif kind == 16:                                  # Bcc over one MOVEQ
            self.a.words(0x6002 | r.randrange(2, 16) << 8, 0x7000 | r.randrange(8) << 9 | r.randrange(256))
        elif kind == 17:                                  # DBcc loop around one instruction
            self.a.words(0x7e00 | r.randrange(1, 6))      # MOVEQ #n,D7
            loop = self.a.pc
            self.a.words(0x5280 | r.randrange(7))         # ADDQ.L #1,Dn (not D7)
            self.a.words(0x50cf | r.randrange(16) << 8, (loop - (self.a.pc + 2)) & 0xffff)   # DBcc D7
        elif kind == 18:                                  # MOVEM.L regs,-(A7) / (A7)+,regs
            mask = r.randrange(1, 0x100)                  # D0-D7
            rev = int(f"{mask:016b}"[::-1], 2)
            self.a.words(0x48e7, rev, 0x4cdf, mask)
        elif kind == 19:                                  # MOVEM.W to and from memory
            mask = r.randrange(1, 0x100)
            d = r.choice([self.d16a0, self.ram_abs_w])()
            self.a.words(0x4880 | ea_bits(d[0], d[1]), mask)
            for w in d[2](2, self.a.pc):
                self.a.words(w)
            self.a.words(0x4c80 | ea_bits(d[0], d[1]), mask)
            for w in d[2](2, self.a.pc):
                self.a.words(w)
        elif kind == 20:                                  # LEA/PEA
            self.a.words(0x41e8 | 3 << 9, r.randrange(0, 0x400, 2))   # LEA d16(A0),A3
            self.a.words(0x4868, r.randrange(0, 0x400, 2), 0x588f)     # PEA d16(A0); ADDQ.L #4,A7
        elif kind == 21:                                  # LINK/UNLK
            self.a.words(0x4e54, (-r.randrange(2, 64, 2)) & 0xffff, 0x4e5c)
        elif kind == 22:                                  # CCR operations and MOVE to/from SR
            self.a.words(r.choice([0x003c, 0x023c, 0x0a3c]), r.randrange(32))
            self.a.words(0x40c0 | r.randrange(8))         # MOVE SR,Dn
            self.a.words(0x46fc, 0x2000)                  # MOVE #$2000,SR
        elif kind == 23:                                  # TRAP, TRAPV, CHK
            self.a.words(r.choice([0x4e40, 0x4e76, 0x4180 | r.randrange(8) << 9 | r.randrange(8)]))
        elif kind == 24:                                  # NBCD, TAS
            d = self.dst(1)
            self.emit(r.choice([0x4800, 0x4ac0]) | ea_bits(d[0], d[1]), d, size=1)
        elif kind == 25:                                  # MOVEP between Dn and d16(A0)
            self.a.words(0x0108 | r.randrange(8) << 9 | r.randrange(4) << 6, r.randrange(0, 0x400, 2))
        elif kind == 26:                                  # CMPM.W (A1)+,(A1)+
            self.a.words(0xb349)
        elif kind == 27:                                  # ADDA/SUBA/CMPA on A3
            s = r.choice([self.dreg, self.imm])()
            op = r.choice([0xd0c0, 0xd1c0, 0x90c0, 0x91c0, 0xb0c0, 0xb1c0])
            self.emit(op | 3 << 9 | ea_bits(s[0], s[1]), s, size=4 if op & 0x0100 else 2)
        else:                                             # MOVE.W between memory forms
            s, d = self.mem(word_only=True), self.mem(word_only=True)
            self.emit(0x3000 | (d[1] << 9) | (d[0] << 6) | ea_bits(s[0], s[1]), s, d)


def random_code_image(seed, count=400):
    rng = random.Random(seed)
    a = Asm()
    rom = a.rom
    rom[0:4] = (0x00FFF000).to_bytes(4, "big")
    rom[4:8] = (0x200).to_bytes(4, "big")
    rom[0x100:0x10c] = b"SEGA GENESIS"
    handlers = 0x180
    a.pc = handlers
    rte = a.pc
    a.words(0x4e73)                                       # RTE
    vint = a.pc
    a.words(0x52b8, 0xf100, 0x4238, 0xf110, 0x4e73)       # ADDQ.L #1,$F100.w; CLR.B $F110.w; RTE
    hint = a.pc
    a.words(0x5278, 0xf104, 0x4e73)                       # ADDQ.W #1,$F104.w; RTE
    for v in range(2, 64):
        rom[v * 4:v * 4 + 4] = rte.to_bytes(4, "big")
    rom[28 * 4:28 * 4 + 4] = hint.to_bytes(4, "big")
    rom[30 * 4:30 * 4 + 4] = vint.to_bytes(4, "big")
    data_at = 0x7000
    for i in range(0x200):
        rom[data_at + i] = rng.randrange(256)
    g = Gen(rng, a, data_at)
    a.pc = 0x200
    a.words(0x33fc, 0x8164, 0x00c0, 0x0004)               # VDP: display, vertical interrupt
    a.words(0x33fc, 0x8014, 0x00c0, 0x0004)               # horizontal interrupt
    a.words(0x33fc, 0x8a00 | rng.randrange(8, 64), 0x00c0, 0x0004)
    a.words(0x46fc, 0x2000)                               # interrupts on
    loop = a.pc
    a.words(0x41f9, RAM_A0 >> 16, RAM_A0 & 0xffff)        # LEA RAM_A0,A0
    a.words(0x43f9, RAM_A1 >> 16, RAM_A1 & 0xffff)
    a.words(0x45f9, RAM_A2 >> 16, RAM_A2 & 0xffff)
    sub_call = a.pc
    a.words(0x4eb9, 0, 0)                                 # JSR sub (patched)
    for i in range(count):
        g.instruction()
        if i % 100 == 99:                                 # wait for the vertical interrupt
            a.words(0x11fc, 1, 0xf110)                    # MOVE.B #1,$F110.w
            wait = a.pc
            a.words(0x4a38, 0xf110, 0x66fa)               # TST.B $F110.w; BNE wait
            assert a.pc - 2 + 2 - 6 == wait
    a.words(0x6000, (loop - (a.pc + 2)) & 0xffff)         # BRA loop
    sub = a.pc
    rom[sub_call + 2:sub_call + 6] = sub.to_bytes(4, "big")
    for _ in range(count // 8):
        g.instruction()
    a.words(0x4e75)                                       # RTS
    assert a.pc < data_at
    return bytes(rom)
