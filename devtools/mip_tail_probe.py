# Mip layout probe (packed tail, DXT formats). Memory comes from CRACKDOWN_DUMP_MEM=0:20000000,
# texture constants from CRACKDOWN_FETCH_DUMP=all. Run with python3 -I.
# tail2.py mem.bin mem_base base mip fmt W H: decodes the packed-mip tail tile texel by texel and finds
# each small level's position (block-aligned) by comparison with the box-filtered base level.
import sys, numpy as np
mem = np.fromfile(sys.argv[1], dtype=np.uint8)
mbase = int(sys.argv[2], 16)
base, mip, fmt, W, H = int(sys.argv[3], 16), int(sys.argv[4], 16), int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7])
bpb = 8 if fmt == 18 else 16
lb = 3 if bpb == 8 else 4

def tiled(x, y, width, lb):  # as runtime/prelude.glsl.h tiled_offset
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (lb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << lb
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
    return ((((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) + ((((((y & 8) >> 2) + (x >> 3)) & 3) << 6))) >> lb)

def decode(addr, bw, bh, pitch):
    """Full colour decode (DXT1, or the colour half of DXT2-5) of bw x bh tiled blocks -> (4bh, 4bw, 3)."""
    out = np.zeros((bh * 4, bw * 4, 3))
    for by in range(bh):
        for bx in range(bw):
            a = addr - mbase + (tiled(bx, by, pitch, lb) << lb) + (0 if bpb == 8 else 8)
            w = lambda i: int(mem[a + i + 1]) | (int(mem[a + i]) << 8)
            c0, c1, idx = w(0), w(2), w(4) | (w(6) << 16)
            rgb = lambda c: np.array([(c >> 11) & 31, (c >> 5) & 63, c & 31]) * [8.2, 4.05, 8.2]
            if c0 > c1 or bpb == 16: p = [rgb(c0), rgb(c1), (2 * rgb(c0) + rgb(c1)) / 3, (rgb(c0) + 2 * rgb(c1)) / 3]
            else: p = [rgb(c0), rgb(c1), (rgb(c0) + rgb(c1)) / 2, np.zeros(3)]
            for i in range(16): out[by * 4 + i // 4, bx * 4 + i % 4] = p[(idx >> (2 * i)) & 3]
    return out

ref = decode(base, W // 4, H // 4, (W // 4 + 31) & ~31)
# Levels whose shorter side is above 16 texels take whole tiles of their own (pitch and rows rounded to 32 blocks).
addr, L = mip, 1
while min(W >> L, H >> L) > 16:
    bw, bh = (W >> L) // 4, (H >> L) // 4
    addr += ((bw + 31) & ~31) * ((bh + 31) & ~31) * bpb
    L += 1
tile = decode(addr, 32, 32, 32)
print(f'{W}x{H} fmt {fmt}: tail tile at mip+{addr - mip:#x} starts with level {L}')
while True:
    lw, lh = max(W >> L, 1), max(H >> L, 1)
    want = ref.reshape(lh, H // lh, lw, W // lw, 3).mean((1, 3))
    c = []
    for y in range(0, 128 - lh + 1, 4):
        for x in range(0, 128 - lw + 1, 4):
            c.append((np.abs(tile[y:y + lh, x:x + lw] - want).mean(), x, y))
    c.sort()
    print(f'  level {L} {lw}x{lh}: median {c[len(c) // 2][0]:.1f};', '; '.join(f'{e:.1f} at texel ({x},{y})' for e, x, y in c[:3]))
    if lw == 1 and lh == 1: break
    L += 1
