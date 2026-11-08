#!/usr/bin/env python3
# The recompiler gives up on a jump out of the function it is compiling ("// ERROR ...", then a bare
# PPC_RETURN). When the code there is only `li rD,imm` (addi rD,0,imm) instructions ending in `blr`
# (a tail the PowerPC compiler shared between functions, e.g. "return 1"), this inlines those loads
# before the return, so the function still returns what it should.
#   inline_error_tails.py <ppc dir> <image.bin> [base 0x82000000]
import re, struct, sys, glob

ppc, image = sys.argv[1], open(sys.argv[2], 'rb').read()
base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x82000000

def tail(addr):
    """C statements for a li*/blr tail at addr, or None."""
    out = []
    for i in range(8):
        o = addr - base + i * 4
        if o < 0 or o + 4 > len(image): return None
        w = struct.unpack('>I', image[o:o + 4])[0]
        if w == 0x4E800020: return out  # blr
        if w >> 26 == 14 and ( w >> 16 ) & 31 == 0:  # addi rD,0,simm = li
            rd, imm = ( w >> 21 ) & 31, w & 0xFFFF
            out.append('ADJ(ctx)->r%d.s64 = %d;' % (rd, imm - 0x10000 if imm & 0x8000 else imm))
            continue
        return None
    return None

ERR = re.compile(r'^(\s*)// ERROR:? (?:0x)?([0-9A-F]{8})\n\1PPC_RETURN\(\);', re.M)
total = 0
for path in glob.glob(ppc + '/ppc_recomp.*.cpp'):
    src = open(path).read()
    def fix(m):
        global total
        t = tail(int(m.group(2), 16))
        if t is None: return m.group(0)
        total += 1
        ind = m.group(1)
        return '%s// ERROR %s: a shared return tail, inlined (inline_error_tails.py)\n' % (ind, m.group(2)) + ''.join(ind + s + '\n' for s in t) + ind + 'PPC_RETURN();'
    new = ERR.sub(fix, src)
    if new != src: open(path, 'w').write(new)
print('inlined %d tails' % total)
