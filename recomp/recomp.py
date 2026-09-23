"""Static recompiler: prince.exe (16-bit x86, MS C 6 + overlays) -> C.

  python recomp.py [extra_entries.txt]   -> ../src/gen/*.c, ../src/gen/dispatch.c

Each function is identified by (space, link segment, offset).  Generated C manipulates the
CPU state declared in cpu.h and the flat real-mode memory MEM[].  Calls keep an emulated stack
(the original code addresses arguments through BP), and every RET simply returns from the C
function.  Far/indirect calls go through the runtime dispatcher, which knows which overlay is
currently loaded at each segment.
"""
import os, sys, struct, collections


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, 'w').write(text)
from x86dec import decode, R8, R16, SREG, JCC
from image import load, LOADSEG, GAME_DIR

ROOT_END = 0x2344
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'gen')

spaces, HDR, EXE = load()

# The FM (AdLib / Sound Blaster) MIDI driver is loaded by the game at runtime.  The runtime copies
# it to the fixed segment FM_SEG (above conventional memory) and runs this recompiled copy there;
# its OPL port writes go to the native OPL2 emulator (src/opl.c).
# Runtime hooks: (space, link seg, offset) -> C function called just before that instruction.
HOOKS = {
    ('root', 0x02cc, 0x12a5): 'rt_frame_hook',   # per-frame game update (before the call to 0000:1000)
    ('root', 0x052d, 0x0546): 'rt_savemenu_hook',  # "Saved Games" screen: log what led to it
    ('root', 0x1de8, 0x000c): 'rt_fatal_hook',     # the game's "print error and exit" routine
}

FM_SEG = 0xD000
FM_DRV = os.path.join(GAME_DIR, 'msb_pro.drv')   # Sound Blaster Pro FM driver, recompiled with the game
FM_ENTRIES = set()
if os.path.exists(FM_DRV):
    from image import Space
    _d = bytearray(open(FM_DRV, 'rb').read())
    _fm = Space('fm', FM_SEG - LOADSEG, _d, 0x7FFF0000, set())
    _fm.ovl_id = 100
    _fm.flags = 1                      # never the target of statically known calls
    spaces['fm'] = _fm
    # entry DRV:0100, its int 8 calibration ISR, and the function table it calls through (0x123)
    FM_ENTRIES = {0x100, 0x3d2} | {struct.unpack_from('<H', _d, 0x123 + 2 * i)[0] for i in range(16)}


def spaces_for(linkseg, off):
    """Code spaces that may hold code at link address linkseg:off."""
    if linkseg < ROOT_END:
        s = spaces['root']
        return [s] if s.contains(linkseg, off) else []
    return [s for n, s in spaces.items() if n != 'root' and getattr(s, 'flags', 0) != 1
            and s.contains(linkseg, off)]


class Func:
    def __init__(self, space, seg, off):
        self.space = space; self.seg = seg; self.off = off
        self.insns = {}          # offset -> Insn
        self.labels = set()
        self.calls = []          # (kind, target...) discovered
        self.bad = False
        self.tables = {}         # offset of jmpi -> list of targets

    @property
    def name(self):
        return 'f_%s_%04x_%04x' % (self.space.name, self.seg, self.off)


def dec_at(space, seg, off):
    p = space.pos(seg, off)
    if p < 0 or p >= space.size - 1:
        return None
    base = p - off                      # data index of segment offset 0 (may be negative)
    buf = space.data
    # decode needs buf indices; handle negative base by padding logic
    try:
        ins = decode(buf, p, base)
    except IndexError:
        return None
    return ins


def find_table(fn, ins):
    """jmp word ptr cs:[bx+T] : read the jump table (count from the last cmp before it)."""
    a = ins.args[0]
    if a[0] != 'mem' or a[5] != 'cs' or a[3] != 'bx':
        return None
    table = a[4] & 0xFFFF
    # bound: most recent 'cmp reg16, imm' at lower address within 40 bytes
    n = None
    for o in sorted(fn.insns, reverse=True):
        if o >= ins.addr:
            continue
        if ins.addr - o > 40:
            break
        i = fn.insns[o]
        if i.op == 'cmp' and i.args[1][0] == 'imm' and i.args[0][0] in ('r16', 'r8'):
            n = i.args[1][1] + 1
            break
        if i.op in ('sub',) and i.args[1][0] == 'imm' and i.args[0][0] == 'r16':
            continue
    sp = fn.space
    targets = []
    count = n if n is not None and n <= 256 else 64
    for k in range(count):
        p = sp.pos(fn.seg, table + 2 * k)
        if p < 0 or p + 2 > sp.size:
            break
        t = struct.unpack_from('<H', sp.data, p)[0]
        if n is None and (abs(t - ins.addr) > 0x2000):
            break
        targets.append(t)
    return targets


def trace(fn, entries_global):
    work = [fn.off]
    seen = set()
    while work:
        o = work.pop()
        while True:
            if o in seen:
                break
            if o != fn.off and o in entries_global and o in fn.insns:
                break
            seen.add(o)
            ins = dec_at(fn.space, fn.seg, o)
            if ins is None:
                fn.bad = True
                break
            fn.insns[o] = ins
            op = ins.op
            nxt = (o + ins.length) & 0xFFFF
            if op in ('jmp',) and ins.args[0][0] == 'rel':
                t = ins.args[0][1]
                if t != fn.off and t in entries_global:
                    fn.calls.append(('tail', fn.seg, t))
                    break
                fn.labels.add(t); o = t
                continue
            if op in JCC or op in ('loop', 'loope', 'loopne', 'jcxz'):
                t = ins.args[0][1]
                fn.labels.add(t); work.append(t)
                o = nxt
                continue
            if op == 'call':
                t = ins.args[0][1]
                fn.calls.append(('near', fn.seg, t))
                o = nxt
                continue
            if op == 'callf':
                s, t = ins.args[0][1], ins.args[0][2]
                fn.calls.append(('far', (s - LOADSEG) & 0xFFFF, t))
                o = nxt
                continue
            if op == 'jmpf':
                s, t = ins.args[0][1], ins.args[0][2]
                fn.calls.append(('far', (s - LOADSEG) & 0xFFFF, t))
                break
            if op == 'jmpi':
                tb = find_table(fn, ins)
                if tb:
                    tb = [t for t in tb if dec_at(fn.space, fn.seg, t) is not None]
                    fn.tables[o] = tb
                    for t in tb:
                        fn.labels.add(t); work.append(t)
                break
            if op in ('ret', 'retf', 'iret', 'jmpfi', 'hlt'):
                break
            o = nxt
    return fn


# ------------------------------------------------------------------------------ C generation

REG16 = {'ax': 'AX', 'cx': 'CX', 'dx': 'DX', 'bx': 'BX', 'sp': 'SP', 'bp': 'BP', 'si': 'SI', 'di': 'DI'}
REG8 = {'al': 'AL', 'cl': 'CL', 'dl': 'DL', 'bl': 'BL', 'ah': 'AH', 'ch': 'CH', 'dh': 'DH', 'bh': 'BH'}
SEGR = {'es': 'ES', 'cs': 'CS', 'ss': 'SS', 'ds': 'DS'}


class Gen:
    def __init__(self, fn):
        self.fn = fn
        self.cs = (fn.seg + LOADSEG) & 0xFFFF
        self.out = []

    def segval(self, s):
        return '0x%04x' % self.cs if s == 'cs' else SEGR[s]

    def ea(self, a):
        _, size, sdef, base, disp, sov = a
        seg = self.segval(sov or sdef)
        terms = [REG16[b] for b in base.split('+')] if base else []
        off = ' + '.join(terms + ['%d' % disp]) if terms else '0x%04x' % (disp & 0xFFFF)
        return seg, '(uint16_t)(%s)' % off

    def rd(self, a, size=None):
        k = a[0]
        if k == 'r8': return REG8[R8[a[1]]]
        if k == 'r16': return REG16[R16[a[1]]]
        if k == 'sreg': return SEGR[SREG[a[1]]] if SREG[a[1]] != 'cs' else '0x%04x' % self.cs
        if k == 'imm': return '0x%x' % a[1]
        if k == 'mem':
            s, o = self.ea(a)
            return ('RB(%s, %s)' if a[1] == 8 else 'RW(%s, %s)') % (s, o)
        raise ValueError(a)

    def wr(self, a, val):
        k = a[0]
        if k == 'r8': return '%s = (uint8_t)(%s);' % (REG8[R8[a[1]]], val)
        if k == 'r16': return '%s = (uint16_t)(%s);' % (REG16[R16[a[1]]], val)
        if k == 'sreg':
            if SREG[a[1]] == 'cs':
                return '/* mov cs */;'
            return '%s = (uint16_t)(%s);' % (SEGR[SREG[a[1]]], val)
        if k == 'mem':
            s, o = self.ea(a)
            return ('WB(%s, %s, %s);' if a[1] == 8 else 'WW(%s, %s, %s);') % (s, o, val)
        raise ValueError(a)

    def emit(self, s):
        self.out.append('    ' + s)

    def goto(self, t):
        if t in self.fn.insns:
            if t <= self.cur:
                return '{ POLL(); goto L_%04x; }' % t     # backward jump: let interrupts in
            return 'goto L_%04x;' % t
        return '{ rt_badjump(0x%04x, 0x%04x, 0x%04x); return; }' % (self.cs, self.cur, t)

    def gen_ins(self, ins):
        op = ins.op; a = ins.args; sz = ins.size; S = '8' if sz == 8 else '16'
        nxt = (ins.addr + ins.length) & 0xFFFF
        E = self.emit
        if op == 'mov':
            E(self.wr(a[0], self.rd(a[1]))); return
        if op in ('add', 'adc', 'sub', 'sbb', 'and', 'or', 'xor'):
            E(self.wr(a[0], 'op_%s%s(%s, %s)' % (op, S, self.rd(a[0]), self.rd(a[1])))); return
        if op == 'cmp':
            E('op_sub%s(%s, %s);' % (S, self.rd(a[0]), self.rd(a[1]))); return
        if op == 'test':
            E('op_and%s(%s, %s);' % (S, self.rd(a[0]), self.rd(a[1]))); return
        if op in ('inc', 'dec', 'neg', 'not'):
            E(self.wr(a[0], 'op_%s%s(%s)' % (op, 'S' if False else S, self.rd(a[0])) if op != 'not'
                      else '~(%s)' % self.rd(a[0]))); return
        if op in ('rol', 'ror', 'rcl', 'rcr', 'shl', 'shr', 'sal', 'sar'):
            o2 = 'shl' if op == 'sal' else op
            E(self.wr(a[0], 'op_%s%s(%s, %s)' % (o2, S, self.rd(a[0]), self.rd(a[1])))); return
        if op == 'xchg':
            E('{ uint%s_t t_ = %s; %s %s }' % (S, self.rd(a[0]), self.wr(a[0], self.rd(a[1])), self.wr(a[1], 't_'))); return
        if op == 'lea':
            s, o = self.ea(a[1]); E(self.wr(a[0], o)); return
        if op in ('les', 'lds'):
            s, o = self.ea(a[1])
            E('{ uint16_t o_ = %s; %s %s = RW(%s, (uint16_t)(o_ + 2)); }' % (o, self.wr(a[0], 'RW(%s, o_)' % s),
                                                                         'ES' if op == 'les' else 'DS', s)); return
        if op == 'push':
            E('PUSH(%s);' % self.rd(a[0])); return
        if op == 'pop':
            if a[0][0] == 'mem':
                E('{ uint16_t v_ = POP(); %s }' % self.wr(a[0], 'v_'))
            else:
                E(self.wr(a[0], 'POP()'))
            return
        if op == 'pushf': E('PUSH(get_flags());'); return
        if op == 'popf': E('set_flags(POP());'); return
        if op == 'pusha': E('{ uint16_t s_ = SP; PUSH(AX); PUSH(CX); PUSH(DX); PUSH(BX); PUSH(s_); PUSH(BP); PUSH(SI); PUSH(DI); }'); return
        if op == 'popa': E('{ DI = POP(); SI = POP(); BP = POP(); POP(); BX = POP(); DX = POP(); CX = POP(); AX = POP(); }'); return
        if op == 'cbw': E('AX = (uint16_t)(int16_t)(int8_t)AL;'); return
        if op == 'cwd': E('DX = (AX & 0x8000) ? 0xFFFF : 0;'); return
        if op == 'lahf': E('AH = (uint8_t)get_flags();'); return
        if op == 'sahf': E('set_flags((get_flags() & 0xFF00) | AH);'); return
        if op == 'clc': E('FC = 0;'); return
        if op == 'stc': E('FC = 1;'); return
        if op == 'cmc': E('FC ^= 1;'); return
        if op == 'cld': E('FD = 0;'); return
        if op == 'std': E('FD = 1;'); return
        if op == 'cli': E('FI = 0;'); return
        if op == 'sti': E('FI = 1;'); return
        if op in ('nop', 'wait'): return
        if op == 'mul':
            E('op_mul%s(%s);' % (S, self.rd(a[0]))); return
        if op == 'imul':
            E('op_imul%s(%s);' % (S, self.rd(a[0]))); return
        if op == 'div':
            E('op_div%s(%s);' % (S, self.rd(a[0]))); return
        if op == 'idiv':
            E('op_idiv%s(%s);' % (S, self.rd(a[0]))); return
        if op == 'imul3':
            E(self.wr(a[0], 'op_imul3(%s, %s)' % (self.rd(a[1]), self.rd(a[2])))); return
        if op in ('daa', 'das', 'aaa', 'aas', 'aam', 'aad'):
            E('op_%s();' % op); return
        if op == 'xlat':
            E('AL = RB(%s, (uint16_t)(BX + AL));' % self.segval(ins.seg or 'ds')); return
        if op in ('movsb', 'movsw', 'stosb', 'stosw', 'lodsb', 'lodsw', 'cmpsb', 'cmpsw', 'scasb', 'scasw'):
            seg = self.segval(ins.seg or 'ds')
            E('str_%s(%s, %d);' % (op, seg, {'': 0, 'rep': 1, 'repne': 2}[ins.rep])); return
        if op == 'in':
            port = self.rd(a[1])
            E('%s = port_in%s(%s);' % (self.rd(a[0]), S, port)); return
        if op == 'out':
            E('port_out%s(%s, %s);' % (S, self.rd(a[0]), self.rd(a[1]))); return
        if op == 'int':
            E('do_int(0x%x, 0x%04x, 0x%04x);' % (a[0][1], self.cs, nxt)); return
        if op == 'into':
            E('if (FO) do_int(4, 0x%04x, 0x%04x);' % (self.cs, nxt)); return
        if op == 'enter':
            E('{ PUSH(BP); uint16_t f_ = SP; BP = f_; SP -= 0x%x; }' % a[0][1]); return
        if op == 'leave':
            E('SP = BP; BP = POP();'); return
        if op == 'bound':
            return
        if op == 'esc':
            E('fpu_esc(0x%x);' % a[0][1]); return
        if op == 'hlt':
            E('rt_halt(0x%04x, 0x%04x); return;' % (self.cs, ins.addr)); return
        # ---- control flow
        if op in JCC:
            cond = {'jo': 'FO', 'jno': '!FO', 'jb': 'FC', 'jae': '!FC', 'je': 'FZ', 'jne': '!FZ',
                    'jbe': '(FC || FZ)', 'ja': '(!FC && !FZ)', 'js': 'FS', 'jns': '!FS', 'jp': 'FP',
                    'jnp': '!FP', 'jl': '(FS != FO)', 'jge': '(FS == FO)', 'jle': '(FZ || FS != FO)',
                    'jg': '(!FZ && FS == FO)'}[op]
            E('if (%s) %s' % (cond, self.goto(a[0][1]))); return
        if op == 'jcxz':
            E('if (CX == 0) %s' % self.goto(a[0][1])); return
        if op == 'loop':
            E('if (--CX != 0) %s' % self.goto(a[0][1])); return
        if op == 'loope':
            E('if (--CX != 0 && FZ) %s' % self.goto(a[0][1])); return
        if op == 'loopne':
            E('if (--CX != 0 && !FZ) %s' % self.goto(a[0][1])); return
        if op == 'jmp':
            t = a[0][1]
            if t in self.fn.insns and not (t == self.fn.off and False):
                E(self.goto(t))
            else:
                E('%s(); return;' % fname(self.fn.space, self.fn.seg, t, 'near'))
            return
        if op == 'call':
            t = a[0][1]
            E('PUSH(0x%04x); %s();' % (nxt, fname(self.fn.space, self.fn.seg, t, 'near'))); return
        if op == 'callf':
            s, t = a[0][1], a[0][2]
            E('PUSH(0x%04x); PUSH(0x%04x); %s' % (self.cs, nxt, farcall(s, t))); return
        if op == 'jmpf':
            s, t = a[0][1], a[0][2]
            E('%s return;' % farcall(s, t)); return
        if op == 'calli':
            E('{ uint16_t t_ = %s; PUSH(0x%04x); call_near(0x%04x, t_); }' % (self.rd(a[0]), nxt, self.cs)); return
        if op == 'callfi':
            s, o = self.ea(a[0])
            E('{ uint16_t o_ = RW(%s, %s), s_ = RW(%s, (uint16_t)(%s + 2)); PUSH(0x%04x); PUSH(0x%04x); call_far(s_, o_); }'
              % (s, o, s, o, self.cs, nxt)); return
        if op == 'jmpi':
            tb = self.fn.tables.get(ins.addr)
            if tb:
                E('switch (%s) {' % self.rd(a[0]))
                for t in sorted(set(tb)):
                    if t in self.fn.insns:
                        E('  case 0x%04x: goto L_%04x;' % (t, t))
                E('  default: rt_badjump(0x%04x, 0x%04x, %s); return; }' % (self.cs, ins.addr, self.rd(a[0])))
            else:
                E('call_near(0x%04x, %s); return;' % (self.cs, self.rd(a[0])))
            return
        if op == 'jmpfi':
            s, o = self.ea(a[0])
            E('call_far(RW(%s, (uint16_t)(%s + 2)), RW(%s, %s)); return;' % (s, o, s, o)); return
        if op == 'ret':
            n = a[0][1] if a else 0
            E('POP(); %s%sreturn;' % (('SP += %d; ' % n) if n else '', 'RETCHK(); ' if getattr(self, 'framed', False) else '')); return
        if op == 'retf':
            n = a[0][1] if a else 0
            E('POP(); POP(); %s%sreturn;' % (('SP += %d; ' % n) if n else '', 'RETCHK(); ' if getattr(self, 'framed', False) else '')); return
        if op == 'iret':
            E('POP(); POP(); set_flags(POP()); return;'); return
        raise NotImplementedError('%s at %s:%04x' % (op, self.fn.name, ins.addr))

    def gen(self):
        fn = self.fn
        self.out.append('void %s(void) {' % fn.name)
        self.out.append('    cur_fn = "%s"; SPCHK();' % fn.name)
        first = fn.insns.get(fn.off)
        self.framed = bool(first and first.op == 'push' and first.args and first.args[0][0] == 'r16'
                           and R16[first.args[0][1]] == 'bp')
        if self.framed:
            self.out.append('    RETCHK_ENTER();')
        if fn.off not in fn.labels:
            pass
        order = sorted(fn.insns)
        # entry may not be the lowest offset
        if order and order[0] != fn.off:
            self.out.append('    goto L_%04x;' % fn.off)
            fn.labels.add(fn.off)
        prev_end = None
        for idx, o in enumerate(order):
            ins = fn.insns[o]
            self.out.append('L_%04x:' % o)
            hook = HOOKS.get((fn.space.name, fn.seg, o))
            if hook:
                self.out.append('    %s();' % hook)
            self.cur = o
            try:
                self.gen_ins(ins)
            except NotImplementedError as e:
                self.emit('rt_unimpl(0x%04x, 0x%04x); return; /* %s */' % (self.cs, o, e))
            last = ins
            prev_end = (o + ins.length) & 0xFFFF
            if last.op not in ('jmp', 'ret', 'retf', 'iret', 'jmpf', 'jmpi', 'jmpfi', 'hlt'):
                if prev_end not in fn.insns:
                    # fallthrough into code that was never decoded (end of traced code)
                    self.emit('%s(); return; /* fallthrough */' % fname(fn.space, fn.seg, prev_end, 'near'))
                elif idx + 1 >= len(order) or order[idx + 1] != prev_end:
                    # The next instruction in memory is not the next one emitted: an overlapping
                    # decode (a jump into the middle of an instruction) sits in between, and
                    # falling into it would execute a bogus instruction.  Jump to the real one.
                    fn.labels.add(prev_end)
                    self.emit('goto L_%04x;' % prev_end)
        self.out.append('}')
        return '\n'.join(self.out)


NEEDED = set()          # functions referenced by name (space, seg, off)


def fname(space, seg, off, kind):
    NEEDED.add((space.name, seg, off))
    return 'f_%s_%04x_%04x' % (space.name, seg, off)


def farcall(s, t):
    ls = (s - LOADSEG) & 0xFFFF
    sps = spaces_for(ls, t)
    if len(sps) == 1 and sps[0].name == 'root':
        NEEDED.add(('root', ls, t))
        return 'f_root_%04x_%04x();' % (ls, t)
    return 'call_far(0x%04x, 0x%04x);' % (s, t)


def main():
    extra = []
    if len(sys.argv) > 1 and os.path.exists(sys.argv[1]):
        for line in open(sys.argv[1]):
            line = line.split('#')[0].strip()
            if line:
                sp_name, so = line.split()
                s, o = [int(v, 16) for v in so.split(':')]
                extra.append((sp_name, s, o))
    # ---- discovery
    entries = collections.defaultdict(set)        # space name -> set of (seg, off)
    entries['root'].add((HDR['cs'], HDR['ip']))
    for (n, s, o) in extra:
        entries[n].add((s, o))
    for o in FM_ENTRIES:
        entries['fm'].add((FM_SEG - LOADSEG, o))
    funcs = {}
    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        for name in list(entries):
            space = spaces[name]
            ent_offsets = collections.defaultdict(set)
            for (s, o) in entries[name]:
                ent_offsets[s].add(o)
            for (s, o) in list(entries[name]):
                key = (name, s, o)
                if key in funcs:
                    continue
                fn = trace(Func(space, s, o), ent_offsets[s])
                funcs[key] = fn
                changed = True
                for c in fn.calls:
                    kind, cs, co = c
                    if kind in ('near', 'tail'):
                        if (cs, co) not in entries[name]:
                            entries[name].add((cs, co))
                    else:
                        for sp2 in spaces_for(cs, co):
                            if (cs, co) not in entries[sp2.name]:
                                entries[sp2.name].add((cs, co))
    # re-trace with the final entry sets so tail jumps are recognised consistently
    final = {}
    for name in entries:
        ent_offsets = collections.defaultdict(set)
        for (s, o) in entries[name]:
            ent_offsets[s].add(o)
        for (s, o) in entries[name]:
            final[(name, s, o)] = trace(Func(spaces[name], s, o), ent_offsets[s])
    funcs = final
    print('discovery rounds', rounds, 'functions', len(funcs))
    bad = [k for k, f in funcs.items() if f.bad]
    print('functions with decode problems', len(bad))
    # ---- generate
    os.makedirs(OUT, exist_ok=True)
    per_space = collections.defaultdict(list)
    for key, fn in sorted(funcs.items()):
        per_space[key[0]].append(fn)
    written = set()
    for name, fns in per_space.items():
        chunks = [fns[i:i + 150] for i in range(0, len(fns), 150)]
        written |= {'%s_%02d.c' % (name, ci) for ci in range(len(chunks))}
        for ci, ch in enumerate(chunks):
            src = ['#include "../cpu.h"', '#include "protos.h"', '']
            for fn in ch:
                src.append(Gen(fn).gen())
                src.append('')
            open(os.path.join(OUT, '%s_%02d.c' % (name, ci)), 'w').write('\n'.join(src))
    # chunk files left over from an earlier, larger function set would define duplicates
    import re as _re
    for f in os.listdir(OUT):
        if _re.match(r'^\w+_\d\d\.c$', f) and f not in written:
            os.remove(os.path.join(OUT, f))
            obj = os.path.join(OUT, '..', '..', 'obj', f[:-2] + '.o')
            if os.path.exists(obj):
                os.remove(obj)
    # prototypes for everything defined or referenced
    allnames = set('f_%s_%04x_%04x' % k for k in funcs)
    missing = set('f_%s_%04x_%04x' % k for k in NEEDED) - allnames
    with open(os.path.join(OUT, 'protos.h'), 'w') as f:
        for n in sorted(allnames | missing):
            f.write('void %s(void);\n' % n)
    # stubs for referenced-but-undiscovered functions (should be rare: fallthrough into gaps)
    with open(os.path.join(OUT, 'stubs.c'), 'w') as f:
        f.write('#include "../cpu.h"\n#include "protos.h"\n')
        for n in sorted(missing):
            _, sp_, s, o = n.split('_')
            f.write('void %s(void) { rt_missing("%s", 0x%s, 0x%s); }\n' % (n, sp_, s, o))
    # dispatch tables: per space, (runtime seg, off) -> function
    with open(os.path.join(OUT, 'dispatch.c'), 'w') as f:
        f.write('#include "../cpu.h"\n#include "protos.h"\n#include "../dispatch.h"\n')
        for name in sorted(per_space):
            fns = sorted(per_space[name], key=lambda fn: (fn.seg, fn.off))
            f.write('const FnEntry tab_%s[] = {\n' % name)
            for fn in fns:
                f.write('  {0x%04x, 0x%04x, %s},\n' % ((fn.seg + LOADSEG) & 0xFFFF, fn.off, fn.name))
            f.write('  {0, 0, 0}};\n')
        f.write('const SpaceTab space_tabs[] = {\n')
        for name in sorted(per_space):
            sp = spaces[name]
            f.write('  {"%s", %d, 0x%04x, %d, %d, tab_%s},\n' % (
                name, getattr(sp, 'ovl_id', 0), (sp.linkseg + LOADSEG) & 0xFFFF, sp.size, sp.fileoff, name))
        f.write('  {0, 0, 0, 0, 0, 0}};\n')
    print('missing (stubbed):', len(missing))
    print('written to', os.path.abspath(OUT))


if __name__ == '__main__':
    main()
