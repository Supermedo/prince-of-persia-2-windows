"""Find likely function entry points that runtime discovery hasn't reached yet.

Pass 1: far pointers anywhere in the image (an offset word followed by a relocated segment
word) that point at a function prologue -> exact seg:off.
Pass 2: MSC prologues `push bp; mov bp,sp` (55 8B EC) or `inc bp; push bp; mov bp,sp`
(45 55 8B EC) right after a function end (ret/retf/iret/nop); CS = the nearest known code
segment below it (CS values of functions already recompiled in that space).

  python scan_prologues.py >> extra_entries.txt
"""
import re
import sys
import image

sp, _, _ = image.load()
known = {}                                   # space -> set of (seg, off) with link-time seg
segs = {}
cur = None
for line in open('../src/gen/dispatch.c'):
    m = re.match(r'const FnEntry tab_(\w+)\[\]', line)
    if m:
        cur = m.group(1); known[cur] = set(); segs[cur] = set(); continue
    m = re.match(r'\s*\{0x([0-9a-f]+), 0x([0-9a-f]+), f_', line)
    if m and cur:
        s = (int(m.group(1), 16) - image.LOADSEG) & 0xFFFF
        o = int(m.group(2), 16)
        known[cur].add((s, o)); segs[cur].add(s)


def prologue(d, p):
    return d[p:p + 3] == b'\x55\x8b\xec' or d[p:p + 4] == b'\x45\x55\x8b\xec'


# pass 1: far pointers that point at a prologue
out = []
for xn, x in sp.items():
    for r in sorted(x.reloc):
        if r < 2:
            continue
        g = ((x.data[r] | x.data[r + 1] << 8) - image.LOADSEG) & 0xFFFF
        o = x.data[r - 2] | x.data[r - 1] << 8
        for name, s in sp.items():
            if name in known and s.contains(g, o) and prologue(s.data, s.pos(g, o)) and (g, o) not in known[name]:
                known[name].add((g, o)); segs[name].add(g)
                out.append('%s %04x:%04x' % (name, g, o))
n1 = len(out)

# pass 2: prologues after a function end
for name, s in sp.items():
    if name not in known:
        continue
    d = s.data
    kl = {s.pos(a, b) for a, b in known[name]}
    bases = sorted({(g - s.linkseg) * 16 for g in segs[name]})
    for m in re.finditer(rb'(?:\x45)?\x55\x8b\xec', bytes(d)):
        p = m.start()
        if p in kl or (d[p] == 0x45 and p + 1 in kl) or (d[p] == 0x55 and p > 0 and d[p - 1] == 0x45):
            continue
        prev_ok = p == 0 or d[p - 1] in (0xC3, 0xCB, 0x90, 0xCF) or (p >= 3 and d[p - 3] in (0xC2, 0xCA))
        if not prev_ok:
            continue
        b = [x for x in bases if x <= p and p - x < 0x10000]
        if not b:
            continue
        base = b[-1]
        seg = s.linkseg + base // 16
        out.append('%s %04x:%04x' % (name, seg, p - base))
print('\n'.join(out))
print('%d far-pointer entries, %d prologue candidates' % (n1, len(out) - n1), file=sys.stderr)
