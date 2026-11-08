# Finds code that forms an address with lis + addi/ori (and lis + load/store displacement):
#   python3 -I devtools/xrefs.py <image.bin> <load base hex> <address hex> [...]
# Prints the instruction address of each use (the addi/ori/load/store completing the pair).
import struct, sys

data = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16)
targets = {int(a, 16) for a in sys.argv[3:]}
words = struct.unpack('>%dI' % (len(data) // 4), data[: len(data) // 4 * 4])
hi = {}  # register -> (value << 16, index of the lis)
for i, w in enumerate(words):
    op = w >> 26
    rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op == 15 and ra == 0:          # lis rd, imm
        hi[rd] = ((imm << 16) & 0xFFFFFFFF, i)
        continue
    if ra in hi and i - hi[ra][1] < 64:
        v = None
        if op == 14: v = (hi[ra][0] + simm) & 0xFFFFFFFF            # addi
        elif op == 24: v = hi[ra][0] | imm                           # ori
        elif op in (32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 50, 52, 54):  # loads/stores
            v = (hi[ra][0] + simm) & 0xFFFFFFFF
        if v in targets:
            print(f'{base + i * 4:08X}: target {v:08X}')
    if op in (14, 15) and rd in hi and rd != ra: hi.pop(rd, None)
