#!/usr/bin/env python3
# Minimal Xbox 360 STFS (LIVE / PIRS / CON) reader: prints the package metadata and file list, and
# extracts the files.
#   stfs.py <package> [outdir]     list (and extract to outdir)
#   stfs.py --install <package>    extract a DLC / content package where the port finds content:
#                                  $XDG_DATA_HOME/crackdown-recomp/content/<content type>/<package file name>/
#                                  (+ <package file name>.name holding its display name)
#                                  A title update goes to $XDG_DATA_HOME/crackdown-recomp/update/ (update:\);
#                                  its default.xexp then patches default.xex (devtools/xexpatch.cpp).
import struct, sys, os

def u24le(b, o): return b[o] | b[o + 1] << 8 | b[o + 2] << 16
def u24be(b, o): return b[o] << 16 | b[o + 1] << 8 | b[o + 2]

class Stfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(0x1000 * 16)
        self.h = h
        self.magic = h[:4]
        self.header_size = struct.unpack_from('>I', h, 0x340)[0]
        self.content_type = struct.unpack_from('>I', h, 0x344)[0]
        self.title_id = struct.unpack_from('>I', h, 0x360)[0]
        self.media_id = struct.unpack_from('>I', h, 0x354)[0]
        self.version = struct.unpack_from('>I', h, 0x358)[0]
        self.base_version = struct.unpack_from('>I', h, 0x35C)[0]
        self.display_name = h[0x411:0x411 + 0x80].decode('utf-16-be', 'ignore').split('\0')[0]
        self.description = h[0xD11:0xD11 + 0x80].decode('utf-16-be', 'ignore').split('\0')[0]
        vd = 0x379
        self.block_sep = h[vd + 2]
        self.ft_count = struct.unpack_from('<H', h, vd + 3)[0]
        self.ft_block = u24le(h, vd + 5)
        self.sex = (~self.block_sep) & 1  # 0 female, 1 male (two hash table copies)
        self.first_hash = (self.header_size + 0xFFF) & 0xFFFFF000

    def backing(self, b):
        r = (((b + 0xAA) // 0xAA) << self.sex) + b
        if b < 0xAA: return r
        if b < 0x70E4: return r + (((b + 0x70E4) // 0x70E4) << self.sex)
        return (1 << self.sex) + r + (((b + 0x70E4) // 0x70E4) << self.sex)

    def addr(self, b): return (self.backing(b) << 12) + self.first_hash

    def level0_hash_addr(self, b):
        # backing block of the level-0 hash table covering block b
        if b < 0xAA: n = 0
        else:
            n = (b // 0xAA) * (0xAB << self.sex)  # each group: 0xAA data blocks + its table(s)
            if b >= 0xAA: n += ((b // 0x70E4) + 1) << self.sex
            if b >= 0x70E4: n += 1 << self.sex
        return (n << 12) + self.first_hash + ( b % 0xAA ) * 0x18

    def read_block(self, b):
        self.f.seek(self.addr(b))
        return self.f.read(0x1000)

    def next_block(self, b):
        self.f.seek(self.level0_hash_addr(b))
        e = self.f.read(0x18)
        return u24be(e, 0x15)

    def read_chain(self, start, count, consecutive):
        out = bytearray(); b = start
        for i in range(count):
            out += self.read_block(b)
            b = b + 1 if consecutive else self.next_block(b)
        return bytes(out)

    def files(self):
        table = self.read_chain(self.ft_block, self.ft_count, True)
        entries = []
        for i in range(0, len(table), 0x40):
            e = table[i:i + 0x40]
            flags = e[0x28]
            nl = flags & 0x3F
            if nl == 0: continue
            name = e[:nl].decode('latin-1')
            entries.append(dict(name=name, dir=bool(flags & 0x80), consecutive=bool(flags & 0x40), blocks=u24le(e, 0x29),
                                start=u24le(e, 0x2F), parent=struct.unpack_from('>h', e, 0x32)[0], size=struct.unpack_from('>I', e, 0x34)[0]))
        return entries

    def path_of(self, entries, i):
        parts = []
        while i >= 0 and i < len(entries):
            parts.append(entries[i]['name']); i = entries[i]['parent']
            if i == 0xFFFF or i < 0: break
        return '/'.join(reversed(parts))

def content_root():
    base = os.environ.get('XDG_DATA_HOME') or os.path.join(os.environ.get('HOME', '.'), '.local', 'share')
    return os.path.join(base, 'crackdown-recomp', 'content')

def update_root():
    return os.path.join(os.path.dirname(content_root()), 'update')

def main():
    install = sys.argv[1] == '--install'
    if install: del sys.argv[1]
    s = Stfs(sys.argv[1])
    if install and s.content_type == 0x000B0000:
        # a title update: its files are the game's update:\ (the runtime's update folder); its
        # default.xexp is applied to the disc's default.xex with devtools/xexpatch.cpp
        sys.argv[2:] = [update_root()]
    elif install:
        name = os.path.basename(sys.argv[1])
        dst = os.path.join(content_root(), '%08X' % s.content_type, name)
        sys.argv[2:] = [dst]
        os.makedirs(dst, exist_ok=True)
        open(dst + '.name', 'w', encoding='utf-8').write(s.display_name)
    print('magic %s type %08X title %08X media %08X version %08X base %08X sex %d header %X' % (s.magic, s.content_type, s.title_id, s.media_id, s.version, s.base_version, s.sex, s.header_size))
    print('name: %r  description: %r' % (s.display_name, s.description))
    ents = s.files()
    out = sys.argv[2] if len(sys.argv) > 2 else None
    for i, e in enumerate(ents):
        p = s.path_of(ents, i)
        print('%-60s %10d %s' % (p, e['size'], 'dir' if e['dir'] else ''))
        if out and not e['dir']:
            dst = os.path.join(out, p)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            data = s.read_chain(e['start'], e['blocks'], e['consecutive'])[:e['size']]
            open(dst, 'wb').write(data)

main()
