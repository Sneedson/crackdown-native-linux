#!/usr/bin/env python3
# Indexes the game's XACT wave banks (Xbox 360, big-endian; signature "DNBW"): every sound's codec, channels,
# rate, length, loop and place in the file, and whether its bank is loaded whole or streamed from
# disk as it plays. devtools/xwb_index.py <Audio dir> [more dirs or .xwb files] > index.md
# Run with python3 -I (the banks are data). Layout: signature, version, then five segments (offset,
# length): bank data, entry metadata, seek tables, entry names, wave data. Bank data: flags (bit 0:
# streaming), entry count, name[64], metadata element size, name size, alignment, compact format,
# build time. An entry (24 bytes): flags and duration (duration in samples, bits 4-31), format,
# play region (offset in the wave data, length), loop region (start, length in samples).
# Format bits: tag 0-1 (0 PCM, 1 XMA, 2 ADPCM, 3 WMA), channels 2-4, rate 5-22, block align 23-30,
# bits per sample 31.
import os, struct, sys

TAGS = {0: "PCM", 1: "XMA", 2: "ADPCM", 3: "WMA"}


def bank(path):
    with open(path, "rb") as f:
        head = f.read(8 + 5 * 8)
        if head[:4] not in ( b"WBND", b"DNBW" ):  # the 360 tools write it byte-swapped
            return None
        seg = [struct.unpack(">II", head[8 + i * 8:16 + i * 8]) for i in range(5)]
        f.seek(seg[0][0])
        flags, count = struct.unpack(">II", f.read(8))
        name = f.read(64).split(b"\0")[0].decode("ascii", "replace")
        meta_size, = struct.unpack(">I", f.read(4))
        f.seek(seg[1][0])
        meta = f.read(seg[1][1])
        names = []
        if seg[3][1]:
            f.seek(seg[3][0])
            raw = f.read(seg[3][1])
            names = [raw[i * 64:(i + 1) * 64].split(b"\0")[0].decode("ascii", "replace") for i in range(count)]
    entries = []
    for i in range(count):
        e = meta[i * meta_size:(i + 1) * meta_size]
        if len(e) < 24:
            break
        fd, fmt, off, length, lstart, llen = struct.unpack(">IIIIII", e[:24])
        tag, ch, rate = fmt & 3, (fmt >> 2) & 7, (fmt >> 5) & 0x3FFFF
        samples = fd >> 4
        secs = samples / rate if rate else 0
        entries.append(dict(i=i, codec=TAGS[tag], ch=ch, rate=rate, secs=secs, off=seg[4][0] + off, length=length,
                            loop=llen > 0, name=names[i] if i < len(names) else ""))
    return dict(path=path, name=name, streaming=bool(flags & 1), entries=entries)


def main():
    paths = []
    for a in sys.argv[1:]:
        if os.path.isdir(a):
            paths += sorted(os.path.join(a, x) for x in os.listdir(a) if x.lower().endswith(".xwb"))
        else:
            paths.append(a)
    banks = [b for b in map(bank, paths) if b]
    print("# Crackdown sound banks\n")
    print("| File | Bank | Kind | Sounds | Total length | Longest | Looping |")
    print("|---|---|---|---|---|---|---|")
    for b in banks:
        es = b["entries"]
        total = sum(e["secs"] for e in es)
        print(f"| {os.path.relpath(b['path'], os.path.dirname(os.path.dirname(b['path'])))} | {b['name']} | "
              f"{'streamed from disk' if b['streaming'] else 'loaded into memory'} | {len(es)} | {total / 60:.1f} min | "
              f"{max((e['secs'] for e in es), default=0):.1f} s | {sum(e['loop'] for e in es)} |")
    for b in banks:
        print(f"\n## {b['name']} ({os.path.basename(b['path'])}, {'streamed' if b['streaming'] else 'in memory'})\n")
        print("| # | Codec | Ch | Rate | Length | Loop | File offset | Bytes |")
        print("|---|---|---|---|---|---|---|---|")
        for e in b["entries"]:
            print(f"| {e['i']} | {e['codec']} | {e['ch']} | {e['rate']} | {e['secs']:.2f} s | {'yes' if e['loop'] else ''} | "
                  f"0x{e['off']:X} | {e['length']} |")


main()
