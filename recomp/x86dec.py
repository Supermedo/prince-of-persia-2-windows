"""Minimal 8086/80186 real-mode instruction decoder (for static recompilation).

decode(buf, pos) -> Insn | None
  Insn.op       mnemonic (lowercase)
  Insn.args     list of operands: ('r8', idx) ('r16', idx) ('sreg', idx) ('imm', val, size)
                ('mem', size, seg_default, base_expr, disp, seg_override)  base_expr: e.g. 'bx+si' or ''
                ('rel', target_offset) ('far', seg, off)
  Insn.size     operand size (8/16) where relevant
  Insn.length   bytes
  Insn.rep      '', 'rep', 'repne'
  Insn.seg      segment override prefix name or None
"""

R8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
R16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
SREG = ['es', 'cs', 'ss', 'ds']
EA = ['bx+si', 'bx+di', 'bp+si', 'bp+di', 'si', 'di', 'bp', 'bx']
ARITH = ['add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp']
SHIFT = ['rol', 'ror', 'rcl', 'rcr', 'shl', 'shr', 'sal', 'sar']
JCC = ['jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp', 'jl', 'jge', 'jle', 'jg']


class Insn:
    __slots__ = ('addr', 'op', 'args', 'size', 'length', 'rep', 'seg', 'raw')

    def __init__(self):
        self.args = []
        self.size = 16
        self.rep = ''
        self.seg = None

    def __repr__(self):
        return '%04x %s%s %s' % (self.addr, (self.rep + ' ') if self.rep else '', self.op,
                                 ', '.join(fmt(a) for a in self.args))


def fmt(a):
    k = a[0]
    if k in ('r8', 'r16'):
        return (R8 if k == 'r8' else R16)[a[1]]
    if k == 'sreg':
        return SREG[a[1]]
    if k == 'imm':
        return '0x%x' % a[1]
    if k == 'rel':
        return 'L%04x' % a[1]
    if k == 'far':
        return '%04x:%04x' % (a[1], a[2])
    if k == 'mem':
        _, size, sdef, base, disp, sov = a
        s = (sov or sdef) + ':'
        return '%s[%s%s%s]' % ('b' if size == 8 else 'w' if size == 16 else 'd', s, base,
                               ('%+d' % disp) if base else '0x%x' % (disp & 0xFFFF))
    return str(a)


def s8(v):
    return v - 256 if v & 0x80 else v


def s16(v):
    return v - 65536 if v & 0x8000 else v


class Dec:
    def __init__(self, buf, pos, base):
        self.b = buf; self.p = pos; self.base = base  # base: file offset of segment offset 0

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = self.b[self.p] | (self.b[self.p + 1] << 8); self.p += 2; return v

    def modrm(self, size, seg):
        m = self.u8()
        mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
        if mod == 3:
            return reg, (('r8' if size == 8 else 'r16'), rm)
        if mod == 0 and rm == 6:
            disp = self.u16(); base = ''; sdef = 'ds'
        else:
            base = EA[rm]
            sdef = 'ss' if rm in (2, 3, 6) else 'ds'
            disp = 0
            if mod == 1:
                disp = s8(self.u8())
            elif mod == 2:
                disp = s16(self.u16())
        return reg, ('mem', size, sdef, base, disp, seg)


def decode(buf, pos, base):
    """Decode instruction at file position pos; offsets are relative to base (segment start)."""
    d = Dec(buf, pos, base)
    ins = Insn()
    ins.addr = pos - base
    seg = None
    while True:
        o = d.u8()
        if o in (0x26, 0x2E, 0x36, 0x3E):
            seg = SREG[(o >> 3) & 3]
        elif o == 0xF3:
            ins.rep = 'rep'
        elif o == 0xF2:
            ins.rep = 'repne'
        elif o == 0xF0:
            pass
        else:
            break
    ins.seg = seg

    def fin(op, args=(), size=16):
        ins.op = op; ins.args = list(args); ins.size = size
        ins.length = d.p - pos
        ins.raw = bytes(buf[pos:d.p])
        return ins

    def rel8():
        v = s8(d.u8()); return ('rel', (d.p - base + v) & 0xFFFF)

    def rel16():
        v = d.u16(); return ('rel', (d.p - base + v) & 0xFFFF)

    # arithmetic 00-3F
    if o < 0x40 and (o & 7) < 6:
        op = ARITH[o >> 3]; k = o & 7
        if k == 0:
            r, e = d.modrm(8, seg); return fin(op, [e, ('r8', r)], 8)
        if k == 1:
            r, e = d.modrm(16, seg); return fin(op, [e, ('r16', r)], 16)
        if k == 2:
            r, e = d.modrm(8, seg); return fin(op, [('r8', r), e], 8)
        if k == 3:
            r, e = d.modrm(16, seg); return fin(op, [('r16', r), e], 16)
        if k == 4:
            return fin(op, [('r8', 0), ('imm', d.u8(), 8)], 8)
        if k == 5:
            return fin(op, [('r16', 0), ('imm', d.u16(), 16)], 16)
    if o in (0x06, 0x0E, 0x16, 0x1E):
        return fin('push', [('sreg', (o >> 3) & 3)])
    if o in (0x07, 0x17, 0x1F):
        return fin('pop', [('sreg', (o >> 3) & 3)])
    if o == 0x27: return fin('daa', size=8)
    if o == 0x2F: return fin('das', size=8)
    if o == 0x37: return fin('aaa', size=8)
    if o == 0x3F: return fin('aas', size=8)
    if 0x40 <= o <= 0x47: return fin('inc', [('r16', o & 7)])
    if 0x48 <= o <= 0x4F: return fin('dec', [('r16', o & 7)])
    if 0x50 <= o <= 0x57: return fin('push', [('r16', o & 7)])
    if 0x58 <= o <= 0x5F: return fin('pop', [('r16', o & 7)])
    if o == 0x60: return fin('pusha')
    if o == 0x61: return fin('popa')
    if o == 0x62:
        r, e = d.modrm(16, seg); return fin('bound', [('r16', r), e])
    if o == 0x68: return fin('push', [('imm', d.u16(), 16)])
    if o == 0x6A: return fin('push', [('imm', s8(d.u8()) & 0xFFFF, 16)])
    if o in (0x69, 0x6B):
        r, e = d.modrm(16, seg)
        imm = d.u16() if o == 0x69 else s8(d.u8()) & 0xFFFF
        return fin('imul3', [('r16', r), e, ('imm', imm, 16)])
    if o == 0x6C: return fin('insb', size=8)
    if o == 0x6D: return fin('insw')
    if o == 0x6E: return fin('outsb', size=8)
    if o == 0x6F: return fin('outsw')
    if 0x70 <= o <= 0x7F: return fin(JCC[o & 15], [rel8()])
    if o in (0x80, 0x82):
        r, e = d.modrm(8, seg); return fin(ARITH[r], [e, ('imm', d.u8(), 8)], 8)
    if o == 0x81:
        r, e = d.modrm(16, seg); return fin(ARITH[r], [e, ('imm', d.u16(), 16)], 16)
    if o == 0x83:
        r, e = d.modrm(16, seg); return fin(ARITH[r], [e, ('imm', s8(d.u8()) & 0xFFFF, 16)], 16)
    if o == 0x84:
        r, e = d.modrm(8, seg); return fin('test', [e, ('r8', r)], 8)
    if o == 0x85:
        r, e = d.modrm(16, seg); return fin('test', [e, ('r16', r)], 16)
    if o == 0x86:
        r, e = d.modrm(8, seg); return fin('xchg', [e, ('r8', r)], 8)
    if o == 0x87:
        r, e = d.modrm(16, seg); return fin('xchg', [e, ('r16', r)], 16)
    if o == 0x88:
        r, e = d.modrm(8, seg); return fin('mov', [e, ('r8', r)], 8)
    if o == 0x89:
        r, e = d.modrm(16, seg); return fin('mov', [e, ('r16', r)], 16)
    if o == 0x8A:
        r, e = d.modrm(8, seg); return fin('mov', [('r8', r), e], 8)
    if o == 0x8B:
        r, e = d.modrm(16, seg); return fin('mov', [('r16', r), e], 16)
    if o == 0x8C:
        r, e = d.modrm(16, seg); return fin('mov', [e, ('sreg', r & 3)], 16)
    if o == 0x8D:
        r, e = d.modrm(16, seg); return fin('lea', [('r16', r), e])
    if o == 0x8E:
        r, e = d.modrm(16, seg); return fin('mov', [('sreg', r & 3), e], 16)
    if o == 0x8F:
        r, e = d.modrm(16, seg); return fin('pop', [e])
    if o == 0x90: return fin('nop')
    if 0x91 <= o <= 0x97: return fin('xchg', [('r16', 0), ('r16', o & 7)])
    if o == 0x98: return fin('cbw')
    if o == 0x99: return fin('cwd')
    if o == 0x9A:
        off = d.u16(); sg = d.u16(); return fin('callf', [('far', sg, off)])
    if o == 0x9B: return fin('wait')
    if o == 0x9C: return fin('pushf')
    if o == 0x9D: return fin('popf')
    if o == 0x9E: return fin('sahf')
    if o == 0x9F: return fin('lahf')
    if o == 0xA0: return fin('mov', [('r8', 0), ('mem', 8, 'ds', '', d.u16(), seg)], 8)
    if o == 0xA1: return fin('mov', [('r16', 0), ('mem', 16, 'ds', '', d.u16(), seg)], 16)
    if o == 0xA2: return fin('mov', [('mem', 8, 'ds', '', d.u16(), seg), ('r8', 0)], 8)
    if o == 0xA3: return fin('mov', [('mem', 16, 'ds', '', d.u16(), seg), ('r16', 0)], 16)
    if o == 0xA4: return fin('movsb', size=8)
    if o == 0xA5: return fin('movsw')
    if o == 0xA6: return fin('cmpsb', size=8)
    if o == 0xA7: return fin('cmpsw')
    if o == 0xA8: return fin('test', [('r8', 0), ('imm', d.u8(), 8)], 8)
    if o == 0xA9: return fin('test', [('r16', 0), ('imm', d.u16(), 16)], 16)
    if o == 0xAA: return fin('stosb', size=8)
    if o == 0xAB: return fin('stosw')
    if o == 0xAC: return fin('lodsb', size=8)
    if o == 0xAD: return fin('lodsw')
    if o == 0xAE: return fin('scasb', size=8)
    if o == 0xAF: return fin('scasw')
    if 0xB0 <= o <= 0xB7: return fin('mov', [('r8', o & 7), ('imm', d.u8(), 8)], 8)
    if 0xB8 <= o <= 0xBF: return fin('mov', [('r16', o & 7), ('imm', d.u16(), 16)], 16)
    if o in (0xC0, 0xC1):
        sz = 8 if o == 0xC0 else 16
        r, e = d.modrm(sz, seg); return fin(SHIFT[r], [e, ('imm', d.u8(), 8)], sz)
    if o == 0xC2: return fin('ret', [('imm', d.u16(), 16)])
    if o == 0xC3: return fin('ret')
    if o == 0xC4:
        r, e = d.modrm(16, seg); return fin('les', [('r16', r), e])
    if o == 0xC5:
        r, e = d.modrm(16, seg); return fin('lds', [('r16', r), e])
    if o == 0xC6:
        r, e = d.modrm(8, seg); return fin('mov', [e, ('imm', d.u8(), 8)], 8)
    if o == 0xC7:
        r, e = d.modrm(16, seg); return fin('mov', [e, ('imm', d.u16(), 16)], 16)
    if o == 0xC8:
        a = d.u16(); b = d.u8(); return fin('enter', [('imm', a, 16), ('imm', b, 8)])
    if o == 0xC9: return fin('leave')
    if o == 0xCA: return fin('retf', [('imm', d.u16(), 16)])
    if o == 0xCB: return fin('retf')
    if o == 0xCC: return fin('int', [('imm', 3, 8)])
    if o == 0xCD: return fin('int', [('imm', d.u8(), 8)])
    if o == 0xCE: return fin('into')
    if o == 0xCF: return fin('iret')
    if o in (0xD0, 0xD1, 0xD2, 0xD3):
        sz = 8 if o in (0xD0, 0xD2) else 16
        r, e = d.modrm(sz, seg)
        cnt = ('imm', 1, 8) if o in (0xD0, 0xD1) else ('r8', 1)
        return fin(SHIFT[r], [e, cnt], sz)
    if o == 0xD4: d.u8(); return fin('aam', size=8)
    if o == 0xD5: d.u8(); return fin('aad', size=8)
    if o == 0xD7: return fin('xlat', size=8)
    if 0xD8 <= o <= 0xDF:
        r, e = d.modrm(16, seg); return fin('esc', [('imm', o, 8), e])
    if o == 0xE0: return fin('loopne', [rel8()])
    if o == 0xE1: return fin('loope', [rel8()])
    if o == 0xE2: return fin('loop', [rel8()])
    if o == 0xE3: return fin('jcxz', [rel8()])
    if o == 0xE4: return fin('in', [('r8', 0), ('imm', d.u8(), 8)], 8)
    if o == 0xE5: return fin('in', [('r16', 0), ('imm', d.u8(), 8)], 16)
    if o == 0xE6: return fin('out', [('imm', d.u8(), 8), ('r8', 0)], 8)
    if o == 0xE7: return fin('out', [('imm', d.u8(), 8), ('r16', 0)], 16)
    if o == 0xE8: return fin('call', [rel16()])
    if o == 0xE9: return fin('jmp', [rel16()])
    if o == 0xEA:
        off = d.u16(); sg = d.u16(); return fin('jmpf', [('far', sg, off)])
    if o == 0xEB: return fin('jmp', [rel8()])
    if o == 0xEC: return fin('in', [('r8', 0), ('r16', 2)], 8)
    if o == 0xED: return fin('in', [('r16', 0), ('r16', 2)], 16)
    if o == 0xEE: return fin('out', [('r16', 2), ('r8', 0)], 8)
    if o == 0xEF: return fin('out', [('r16', 2), ('r16', 0)], 16)
    if o == 0xF4: return fin('hlt')
    if o == 0xF5: return fin('cmc')
    if o in (0xF6, 0xF7):
        sz = 8 if o == 0xF6 else 16
        r, e = d.modrm(sz, seg)
        if r in (0, 1):
            imm = d.u8() if sz == 8 else d.u16()
            return fin('test', [e, ('imm', imm, sz)], sz)
        return fin(['', '', 'not', 'neg', 'mul', 'imul', 'div', 'idiv'][r], [e], sz)
    if o == 0xF8: return fin('clc')
    if o == 0xF9: return fin('stc')
    if o == 0xFA: return fin('cli')
    if o == 0xFB: return fin('sti')
    if o == 0xFC: return fin('cld')
    if o == 0xFD: return fin('std')
    if o == 0xFE:
        r, e = d.modrm(8, seg)
        if r < 2:
            return fin(['inc', 'dec'][r], [e], 8)
        return None
    if o == 0xFF:
        r, e = d.modrm(16, seg)
        if r == 0: return fin('inc', [e])
        if r == 1: return fin('dec', [e])
        if r == 2: return fin('calli', [e])
        if r == 3: return fin('callfi', [e])
        if r == 4: return fin('jmpi', [e])
        if r == 5: return fin('jmpfi', [e])
        if r == 6: return fin('push', [e])
        return None
    return None
