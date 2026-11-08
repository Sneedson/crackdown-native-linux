# Mip layout probe (per-level placement, uncompressed formats). Memory from CRACKDOWN_DUMP_MEM=0:20000000,
# texture constants from CRACKDOWN_FETCH_DUMP=all. Run with python3 -I.
# gen.py mem.bin base mip fmt W H: uncompressed formats (2 = 8 bit, 4 = 565, 6 = 8888). Locates each level
# by searching texel offsets inside 1 KB-aligned candidate starts, against the box-filtered base.
import sys, numpy as np
mem = np.fromfile(sys.argv[1], dtype=np.uint8)
base, mip, fmt, W, H = int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
lb = {2: 0, 4: 1, 6: 2}[fmt]

def tiled(x, y, width, lb):  # as runtime/prelude.glsl.h tiled_offset (vectorised)
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (lb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << lb
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
    return ((((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) + ((((((y & 8) >> 2) + (x >> 3)) & 3) << 6))) >> lb)

def decode(addr, w, h, pitch):
    ys, xs = np.mgrid[0:h, 0:w]
    a = np.clip(addr + (tiled(xs, ys, pitch, lb) << lb), 0, len(mem) - 4)
    return np.stack([mem[a + i] for i in range(1 << lb)], -1).astype(float)

ref = decode(base, W, H, (W + 31) & ~31)
for L in range(1, 8):
    lw, lh = W >> L, H >> L
    want = ref.reshape(lh, 1 << L, lw, 1 << L, -1).mean((1, 3))
    best = []
    pitch = max((lw + 31) & ~31, 32)
    for start in range(mip, mip + 0x60000, 0x400):
        reg = decode(start, min(pitch, 64), min(max(lh, 32), 64), pitch)
        for oy in range(0, reg.shape[0] - min(lh, 64) + 1, 4 if lh <= 16 else 64):
            for ox in range(0, reg.shape[1] - min(lw, 64) + 1, 4 if lw <= 16 else 64):
                e = np.abs(reg[oy:oy + min(lh, 64), ox:ox + min(lw, 64)] - want[:64, :64]).mean()
                best.append((e, start - mip, ox, oy))
    best.sort()
    print(f'level {L} {lw}x{lh}:', '; '.join(f'{e:.1f} @+{s:#x} ({x},{y})' for e, s, x, y in best[:3]))
