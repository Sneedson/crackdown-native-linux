// Re-emit a XEX2 as encryption=NONE / compression=BASIC.
//
// Raw retail disc executables are encrypted + LZX-compressed (enc=1, comp=2).
// MS's BC packaging ships game executables as enc=1/comp=1, and every module
// the XeO3 emulator loads uses a combination it accepts. This tool produces the
// decoded form while leaving the header region byte-identical, so the
// headerDigest -- and therefore the xeo3_*.dll name -- does not change.
//
// Usage: XexRepack <in.xex> <out.xex>

#include <xex.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("usage: XexRepack <in.xex> <out.xex> [--encrypt]\n");
        printf("  --encrypt  re-encrypt under the original file key (enc=1/comp=1)\n");
        return 1;
    }

    bool encrypt = false;
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "--encrypt") == 0)
            encrypt = true;
    }

    FILE* f = fopen(argv[1], "rb");
    if (f == nullptr)
    {
        printf("error: cannot open %s\n", argv[1]);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> in;
    in.resize(static_cast<size_t>(size));
    if (fread(in.data(), 1, in.size(), f) != in.size())
    {
        printf("error: short read on %s\n", argv[1]);
        fclose(f);
        return 1;
    }
    fclose(f);

    printf("in : %s (%ld bytes)\n", argv[1], size);

    std::vector<uint8_t> out;
    if (!Xex2RepackBasic(in.data(), in.size(), out, encrypt))
    {
        printf("error: failed to decode XEX (bad magic, or LZX/AES decode failed)\n");
        return 1;
    }

    FILE* o = fopen(argv[2], "wb");
    if (o == nullptr)
    {
        printf("error: cannot write %s\n", argv[2]);
        return 1;
    }
    fwrite(out.data(), 1, out.size(), o);
    fclose(o);

    printf("out: %s (%zu bytes)\n", argv[2], out.size());
    printf("done: encryption=%d compression=1\n", encrypt ? 1 : 0);
    return 0;
}
