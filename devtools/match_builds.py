#!/usr/bin/env python3
# Maps game addresses from one recompiled build to another (the disc version to a title update):
# functions are paired by their code with addresses masked (branch targets, lis-formed constants),
# then through the calls of paired functions; data addresses through the instructions of paired
# functions that form them (lis + displacement).
#   match_builds.py <old ppc dir> <new ppc dir> <address list file>   (one hex address per line)
# Prints "old new how" per address.
import re, sys, glob, hashlib, collections

INSN = re.compile(r'^\s*// ([0-9A-F]{8}) (\S+)\s*(.*)$')
IMPL = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)')

def load(d):
    funcs = collections.OrderedDict()
    cur = None
    for path in sorted(glob.glob(d + '/ppc_recomp.*.cpp'), key=lambda p: int(p.split('.')[-2])):
        for line in open(path, errors='ignore'):
            m = IMPL.match(line)
            if m:
                cur = int(m.group(1), 16); funcs[cur] = []; continue
            m = INSN.match(line)
            if m and cur is not None:
                funcs[cur].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return funcs

def norm(fa, insns):
    """Normalized instruction strings, callees in order, and (index, absolute value) constants."""
    out, calls, consts = [], [], []
    end = insns[-1][0] + 4 if insns else fa
    lis = {}  # reg -> (hi value, index)
    for i, (a, mn, ops) in enumerate(insns):
        t = re.search(r'0x([0-9a-f]{8})\b', ops)
        if t and ( mn.startswith('b') ):
            tgt = int(t.group(1), 16)
            if fa <= tgt < end: out.append('%s @%d' % (mn, tgt - fa))
            else: out.append(mn + ' ?'); calls.append(tgt)
            continue
        if mn == 'lis':
            r, v = ops.split(',')
            lis[r] = ((int(v) & 0xFFFF) << 16, i)
            out.append('lis %s,?' % r); continue
        m = re.match(r'(r\d+),(r\d+),(-?\d+)$', ops)            # addi rD,rA,imm
        m2 = re.match(r'([rfv]\d+),(-?\d+)\((r\d+)\)$', ops)      # lwz rD,d(rA)
        base_reg, disp = (m.group(2), int(m.group(3))) if m and mn in ('addi', 'addic', 'ori') else \
                         (m2.group(3), int(m2.group(2))) if m2 else (None, None)
        if base_reg in lis:
            hi, _ = lis[base_reg]
            val = (hi + (disp if mn != 'ori' else disp & 0xFFFF)) & 0xFFFFFFFF if mn != 'ori' else (hi | (disp & 0xFFFF))
            consts.append((i, val))
            out.append(mn + ' ' + re.sub(r'-?\d+(?=\(|$)', '?', ops)); continue
        out.append(mn + ' ' + ops)
        # a register written by something else no longer holds the lis value
        d = ops.split(',')[0]
        if d in lis and mn not in ('stw', 'stb', 'sth', 'std', 'stfs', 'stfd', 'cmpw', 'cmplw', 'cmpwi', 'cmplwi'): lis.pop(d, None)
    return out, calls, consts

def main():
    old, new = load(sys.argv[1]), load(sys.argv[2])
    want = [int(l.strip(), 16) for l in open(sys.argv[3]) if l.strip()]
    no = {f: norm(f, i) for f, i in old.items()}
    nn = {f: norm(f, i) for f, i in new.items()}
    h = lambda n: hashlib.sha1('\n'.join(n[0]).encode()).hexdigest()
    ho, hn = collections.defaultdict(list), collections.defaultdict(list)
    for f, n in no.items(): ho[h(n)].append(f)
    for f, n in nn.items(): hn[h(n)].append(f)
    pair, fuzzy = {}, {}
    for k, fs in ho.items():
        if len(fs) == 1 and len(hn.get(k, [])) == 1: pair[fs[0]] = hn[k][0]
    exact = len(pair)
    # propagate through calls: paired functions with the same number of calls pair their callees
    changed = True
    while changed:
        changed = False
        for f, g in list(pair.items()):
            co, cn = no[f][1], nn[g][1]
            if len(co) != len(cn): continue
            for a, b in zip(co, cn):
                if a in old and b in new and a not in pair and b not in pair.values():
                    pair[a] = b; changed = True
    print('# %d functions: %d paired by code, %d in all' % (len(old), exact, len(pair)), file=sys.stderr)
    # Wanted functions still unpaired: the most similar code between the paired neighbours around them
    # (functions keep their order in a rebuild).
    import bisect, difflib
    po = sorted(pair); pn = [pair[f] for f in po]
    newfs = sorted(new)
    for a in want:
        if a not in old or a in pair: continue
        i = bisect.bisect_left(po, a)
        lo = pn[i - 1] if i > 0 else 0
        hi = pn[i] if i < len(po) else 0xFFFFFFFF
        if hi < lo: hi = lo + 0x40000
        cands = newfs[bisect.bisect_right(newfs, lo):bisect.bisect_left(newfs, hi)][:400]
        best, score = None, 0.0
        for g in cands:
            if g in pair.values(): continue
            r = difflib.SequenceMatcher(None, no[a][0], nn[g][0], autojunk=False)
            if r.real_quick_ratio() < score: continue
            q = r.ratio()
            if q > score: best, score = g, q
        if best is not None and score > 0.6:
            pair[a] = best; fuzzy[a] = score
    # data: (old value) -> votes for new value, from paired functions' constants at the same index
    data = collections.defaultdict(collections.Counter)
    for f, g in pair.items():
        co, cn = no[f][2], nn[g][2]
        if len(co) != len(cn) or len(no[f][0]) != len(nn[g][0]): continue
        for (i, a), (j, b) in zip(co, cn):
            if i == j: data[a][b] += 1
    for a in want:
        if a in pair: print('%08X %08X function%s' % (a, pair[a], ' (similar %.2f)' % fuzzy[a] if a in fuzzy else '')); continue
        if a in old: print('%08X ? function unpaired' % a); continue
        if a in data:
            b, n = data[a].most_common(1)[0]
            print('%08X %08X data (%d refs)' % (a, b, n)); continue
        # nearest referenced base below, same offset (globals inside a struct / table)
        near = [k for k in data if k <= a and a - k < 0x400]
        if near:
            k = max(near); b, n = data[k].most_common(1)[0]
            print('%08X %08X data via %08X+%X (%d refs)' % (a, b + (a - k), k, a - k, n)); continue
        # inside a function body
        fo = max((f for f in old if f <= a), default=None)
        if fo is not None and fo in pair and a < (old[fo][-1][0] + 4 if old[fo] else fo):
            print('%08X %08X inside %08X+%X' % (a, pair[fo] + (a - fo), fo, a - fo)); continue
        print('%08X ? unknown' % a)

main()
