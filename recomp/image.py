"""prince.exe code spaces: root image + MS-LINK overlays, with relocations applied.

A code "space" is a contiguous code blob loaded at a known link-time segment:
  space 'root'  : root image, link segment 0x0000
  space 'oN'    : overlay N (2..17), link segment from the overlay descriptor table

All segment constants are relocated by LOADSEG (the runtime load segment of the image),
so generated code contains final segment values.
"""
import os
import struct

GAME_DIR = os.environ.get('POP2_GAMEDIR', 'D:/Dos/prince 2.pc')   # holds PRINCE.EXE and the .DAT files
EXE_PATH = os.path.join(GAME_DIR, 'prince.exe')
LOADSEG = 0x0100          # runtime: PSP at 0x00F0, image at 0x0100
OVL_DESC = 150347         # file offset of overlay descriptor #2 (18-byte records)
N_OVL = 16                # overlays 2..17


class Space:
    def __init__(self, name, linkseg, data, fileoff, reloc_sites):
        self.name = name
        self.linkseg = linkseg          # link-time segment of data[0]
        self.data = data                # bytearray, relocated
        self.fileoff = fileoff          # file offset of data[0]
        self.reloc = reloc_sites        # set of byte offsets (within data) holding relocated words
        self.size = len(data)

    def contains(self, seg, off):
        lin = (seg - self.linkseg) * 16 + off
        return 0 <= lin < self.size

    def pos(self, seg, off):
        return (seg - self.linkseg) * 16 + off


def load():
    x = open(EXE_PATH, 'rb').read()
    h = struct.unpack_from('<14H', x, 0)
    cblp, cp, crlc, hdr = h[1], h[2], h[3], h[4]
    lfarlc = h[12]
    img0 = hdr * 16
    img1 = cp * 512 - (512 - cblp if cblp else 0)
    root = bytearray(x[img0:img1])
    sites = set()
    for i in range(crlc):
        off, seg = struct.unpack_from('<HH', x, lfarlc + 4 * i)
        p = seg * 16 + off
        v = struct.unpack_from('<H', root, p)[0]
        struct.pack_into('<H', root, p, (v + LOADSEG) & 0xFFFF)
        sites.add(p)
    spaces = {'root': Space('root', 0, root, img0, sites)}
    header = dict(ss=h[7], sp=h[8], ip=h[10], cs=h[11], minalloc=h[5], maxalloc=h[6])
    for k in range(N_OVL):
        o = OVL_DESC + 18 * k
        seg, f2, pos, hi, fl, f8, nrel, fc, fe, size = struct.unpack_from('<HHHBBHHHHH', x, o)
        rpos = (pos + hi * 65536) * 16
        code = rpos + ((nrel + 3) >> 2) * 16
        data = bytearray(x[code:code + size * 16])
        sites = set()
        for i in range(nrel):
            roff, rseg = struct.unpack_from('<HH', x, rpos + 4 * i)
            p = (rseg - seg) * 16 + roff
            if 0 <= p < len(data) - 1:
                v = struct.unpack_from('<H', data, p)[0]
                struct.pack_into('<H', data, p, (v + LOADSEG) & 0xFFFF)
                sites.add(p)
        name = 'o%d' % fe
        spaces[name] = Space(name, seg, data, code, sites)
        spaces[name].flags = fl
        spaces[name].ovl_id = fe
    return spaces, header, x


if __name__ == '__main__':
    sp, hdr, _ = load()
    print(hdr)
    for n, s in sp.items():
        print(n, hex(s.linkseg), s.size, 'file', s.fileoff, 'relocs', len(s.reloc))
