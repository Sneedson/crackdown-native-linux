#!/usr/bin/env python3
# Features of the D3D block's functions (0x82730000-0x8275FFFF) in a recompiled build:
#   d3d_catalog.py <ppc dir> <entry list: "count addr" lines>
# Per entry: external call count, size, device fields read / written (offsets >= 1000 off any register),
# whether it streams words (stwu ,4), its callees inside the block, and constants (lis) it forms.
import re, sys, glob, collections

INSN = re.compile(r'^\s*// ([0-9A-F]{8}) (\S+)\s*(.*)$')
IMPL = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)')
funcs = collections.OrderedDict()
cur = None
for path in glob.glob(sys.argv[1] + '/ppc_recomp.*.cpp'):
    for line in open(path, errors='ignore'):
        m = IMPL.match(line)
        if m:
            cur = m.group(1); funcs[cur] = []; continue
        m = INSN.match(line)
        if m and cur:
            funcs[cur].append((m.group(2), m.group(3)))

MEM = re.compile(r'^(r\d+),(-?\d+)\((r\d+)\)$')
def features(f, depth=0, seen=None):
    seen = seen if seen is not None else set()
    rd, wr, calls, stream = set(), set(), [], 0
    for op, args in funcs.get(f, []):
        m = MEM.match(args)
        if m and m.group(3) != 'r1':
            off = int(m.group(2))
            if off >= 1000:
                (wr if op.startswith('st') else rd).add(off)
            if op == 'stwu' and off == 4: stream += 1
        if op == 'bl':
            t = args.replace('0x', '').upper()
            if '82730000' <= t < '82760000': calls.append(t)
    return rd, wr, calls, stream

for line in open(sys.argv[2]):
    n, f = line.split()
    rd, wr, calls, stream = features(f)
    # one level into callees inside the block: does the call tree stream commands?
    sub_stream = sum(features(c)[3] for c in set(calls))
    print(f"{f} ext={n} size={len(funcs.get(f, []))} stream={stream}+{sub_stream} "
          f"rd={','.join(list(map(str, sorted(rd)))[:8])} wr={','.join(list(map(str, sorted(wr)))[:8])} calls={' '.join(sorted(set(calls))[:6])}")
