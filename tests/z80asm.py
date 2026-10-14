"""Small two-pass Z80 assembler for the synthetic test cartridges.

Supports the instructions the fixtures use: org/db/dw, labels, numbers
(0x.., decimal, 'c'), +/- expressions on labels, and the common load,
arithmetic, jump, stack, I/O and block forms.  Not a general assembler.
"""
import re

R8 = {'b': 0, 'c': 1, 'd': 2, 'e': 3, 'h': 4, 'l': 5, '(hl)': 6, 'a': 7}
RP = {'bc': 0, 'de': 1, 'hl': 2, 'sp': 3}
RP2 = {'bc': 0, 'de': 1, 'hl': 2, 'af': 3}
CC = {'nz': 0, 'z': 1, 'nc': 2, 'c': 3, 'po': 4, 'pe': 5, 'p': 6, 'm': 7}
ALU = {'add': 0, 'adc': 1, 'sub': 2, 'sbc': 3, 'and': 4, 'xor': 5, 'or': 6, 'cp': 7}
SIMPLE = {'nop': [0], 'halt': [0x76], 'di': [0xf3], 'ei': [0xfb], 'ret': [0xc9], 'exx': [0xd9],
          'ldir': [0xed, 0xb0], 'lddr': [0xed, 0xb8], 'ldi': [0xed, 0xa0], 'otir': [0xed, 0xb3],
          'outi': [0xed, 0xa3], 'outd': [0xed, 0xab], 'reti': [0xed, 0x4d], 'retn': [0xed, 0x45], 'neg': [0xed, 0x44],
          'rlca': [0x07], 'rrca': [0x0f], 'rla': [0x17], 'rra': [0x1f], 'daa': [0x27], 'cpl': [0x2f],
          'scf': [0x37], 'ccf': [0x3f], 'im 1': [0xed, 0x56], 'ex de,hl': [0xeb], "ex af,af'": [0x08],
          'jp (hl)': [0xe9], 'ld sp,hl': [0xf9]}


class Asm:
    def __init__(self, size):
        self.rom = bytearray(b'\xff' * size)
        self.labels = {}

    def value(self, text, final):
        text = text.strip()
        total, sign = 0, 1
        for token in re.findall(r"[+-]|[^+-]+", text):
            token = token.strip()
            if token == '+': sign = 1; continue
            if token == '-': sign = -1; continue
            if re.fullmatch(r"'.'", token): v = ord(token[1])
            elif re.fullmatch(r'0x[0-9a-fA-F]+|\d+', token): v = int(token, 0)
            elif token in self.labels: v = self.labels[token]
            elif final: raise ValueError('unknown symbol ' + token)
            else: v = 0
            total += sign * v
            sign = 1
        return total

    def encode(self, line, pc, final):
        """bytes of one instruction"""
        v = lambda t: self.value(t, final)
        m = line.split(None, 1)
        op, args = m[0], (m[1].replace(' ', '') if len(m) > 1 else '')
        key = (op + ' ' + args).strip() if args else op
        if key in SIMPLE: return SIMPLE[key]
        a = args.split(',') if args else []
        w = lambda n: [n & 255, (n >> 8) & 255]
        if op == 'ld':
            d, s = a
            if d in R8 and s in R8: return [0x40 | R8[d] << 3 | R8[s]]
            if d in RP and not s.startswith('('): return [0x01 | RP[d] << 4] + w(v(s))
            if d in R8 and not s.startswith('('): return [0x06 | R8[d] << 3, v(s) & 255]
            if d == 'a' and s in ('(bc)', '(de)'): return [0x0a if s == '(bc)' else 0x1a]
            if s == 'a' and d in ('(bc)', '(de)'): return [0x02 if d == '(bc)' else 0x12]
            if d == 'a' and s.startswith('('): return [0x3a] + w(v(s[1:-1]))
            if s == 'a' and d.startswith('('): return [0x32] + w(v(d[1:-1]))
            if d == 'hl' and s.startswith('('): return [0x2a] + w(v(s[1:-1]))
            if s == 'hl' and d.startswith('('): return [0x22] + w(v(d[1:-1]))
            if d in RP and s.startswith('('): return [0xed, 0x4b | RP[d] << 4] + w(v(s[1:-1]))
            if s in RP and d.startswith('('): return [0xed, 0x43 | RP[s] << 4] + w(v(d[1:-1]))
            if d.startswith('(ix') or d.startswith('(iy'):
                p = 0xdd if 'ix' in d else 0xfd
                off = v(d[3:-1] or '0') & 255
                if s in R8: return [p, 0x70 | R8[s], off]
                return [p, 0x36, off, v(s) & 255]
            if s.startswith('(ix') or s.startswith('(iy'):
                p = 0xdd if 'ix' in s else 0xfd
                return [p, 0x46 | R8[d] << 3, v(s[3:-1] or '0') & 255]
            if d in ('ix', 'iy'): return [0xdd if d == 'ix' else 0xfd, 0x21] + w(v(s))
        if op in ('add', 'adc', 'sbc') and a[0] == 'hl':
            if op == 'add': return [0x09 | RP[a[1]] << 4]
            return [0xed, (0x4a if op == 'adc' else 0x42) | RP[a[1]] << 4]
        if op in ALU:
            s = a[-1]
            if s in R8: return [0x80 | ALU[op] << 3 | R8[s]]
            return [0xc6 | ALU[op] << 3, v(s) & 255]
        if op in ('add', 'adc', 'sbc') and a[0] == 'hl':
            if op == 'add': return [0x09 | RP[a[1]] << 4]
            return [0xed, (0x4a if op == 'adc' else 0x42) | RP[a[1]] << 4]
        if op in ('inc', 'dec'):
            if a[0] in R8: return [(0x04 if op == 'inc' else 0x05) | R8[a[0]] << 3]
            if a[0] in RP: return [(0x03 if op == 'inc' else 0x0b) | RP[a[0]] << 4]
        if op in ('push', 'pop'):
            if a[0] in ('ix', 'iy'): return [0xdd if a[0] == 'ix' else 0xfd, 0xe5 if op == 'push' else 0xe1]
            return [(0xc5 if op == 'push' else 0xc1) | RP2[a[0]] << 4]
        if op == 'jp':
            if len(a) == 2: return [0xc2 | CC[a[0]] << 3] + w(v(a[1]))
            return [0xc3] + w(v(a[0]))
        if op == 'call':
            if len(a) == 2: return [0xc4 | CC[a[0]] << 3] + w(v(a[1]))
            return [0xcd] + w(v(a[0]))
        if op == 'ret' and a: return [0xc0 | CC[a[0]] << 3]
        if op in ('jr', 'djnz'):
            target = v(a[-1])
            off = (target - (pc + 2)) if final else 0
            if final and not -128 <= off <= 127: raise ValueError('jr out of range: ' + line)
            if op == 'djnz': return [0x10, off & 255]
            if len(a) == 2: return [0x20 | CC[a[0]] << 3, off & 255]
            return [0x18, off & 255]
        if op == 'out': return [0xd3, v(a[0][1:-1]) & 255]
        if op == 'in': return [0xdb, v(a[1][1:-1]) & 255]
        if op == 'rst': return [0xc7 | (v(a[0]) & 0x38)]
        if op in ('bit', 'set', 'res'):
            base = {'bit': 0x40, 'res': 0x80, 'set': 0xc0}[op]
            return [0xcb, base | (v(a[0]) & 7) << 3 | R8[a[1]]]
        if op in ('rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', 'srl'):
            return [0xcb, ['rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', None, 'srl'].index(op) << 3 | R8[a[0]]]
        raise ValueError('cannot assemble: ' + line)

    def assemble(self, source, base=0):
        """source: text; addresses are file offsets (org sets them)."""
        for final in (False, True):
            pc = base
            for raw in source.splitlines():
                line = raw.split(';')[0].strip().lower()
                if not line: continue
                if line.endswith(':'): self.labels[line[:-1]] = pc_addr(pc); continue
                op = line.split(None, 1)
                if op[0] == 'org':
                    pc = self.value(op[1], True); continue
                if op[0] == '.phase':  # code assembled for another address (banked)
                    continue
                if op[0] in ('db', 'dw'):
                    vals = [self.value(t, final) for t in op[1].split(',')]
                    data = bytes(x & 255 for x in vals) if op[0] == 'db' else b''.join(
                        bytes([x & 255, (x >> 8) & 255]) for x in vals)
                else:
                    data = bytes(self.encode(line, pc_addr(pc), final))
                if final: self.rom[pc:pc + len(data)] = data
                pc += len(data)
        return self.rom


def pc_addr(offset):
    """Z80 address of a file offset: pages above 1 are assembled for slot 2."""
    return offset if offset < 0x8000 else 0x8000 + (offset & 0x3fff)
