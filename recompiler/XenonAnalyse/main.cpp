#include <cassert>
#include <iterator>
#include <file.h>
#include <disasm.h>
#include <image.h>
#include <xbox.h>
#include <fmt/core.h>
#include "function.h"

//Added Search for and print out register save/load locations
//modificated from this fork: https://github.com/hedge-dev/XenonRecomp/pull/108
#include "fmt/xchar.h"
#include "function.h"
#include <algorithm>
#include <file.h>
#include <cstring>
#include <fstream>
#include <vector>

#define SWITCH_ABSOLUTE 0
#define SWITCH_COMPUTED 1
#define SWITCH_BYTEOFFSET 2
#define SWITCH_SHORTOFFSET 3

struct SwitchTable
{
    std::vector<size_t> labels{};
    size_t base{};
    size_t defaultLabel{};
    uint32_t r{};
    uint32_t type{};
};

static const uint8_t RESTGPRLR_14[] = { 0xe9, 0xc1, 0xff, 0x68 };
static const uint8_t SAVEGPRLR_14[] = { 0xf9, 0xc1, 0xff, 0x68 };
static const uint8_t RESTFPR_14[] = { 0xc9, 0xcc, 0xff, 0x70 };
static const uint8_t SAVEFPR_14[] = { 0xd9, 0xcc, 0xff, 0x70 };
static const uint8_t RESTVMX_14[] = { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x60, 0xce };
static const uint8_t SAVEVMX_14[] = { 0x39, 0x60, 0xfe, 0xe0, 0x7d, 0xcb, 0x61, 0xce };
static const uint8_t RESTVMX_64[] = { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x60, 0xcb };
static const uint8_t SAVEVMX_64[] = { 0x39, 0x60, 0xfc, 0x00, 0x10, 0x0b, 0x61, 0xcb };

uint32_t BytePatternSearch(uint8_t* data, const uint32_t dataSize, const uint32_t baseAddress, const uint8_t pattern[], const size_t patternSize)
{
    auto result = std::search(data, data + dataSize, pattern, pattern + patternSize);
    if (result != data + dataSize) {
        return baseAddress + std::distance(data, result);
    }

    return UINT32_MAX;
}

// s4: dump the decrypted, decompressed image FLAT -- file offset == VA - base.
//
// Written for tools\audit_switch_tables.py, which has to read GUEST BYTES
// rather than the recompiler's disassembly comments. Two reasons the comments do
// not suffice: a jump table's words can lie outside any emitted function (a
// truncated function boundary is part of the very bug being hunted, so trusting
// the emitted range would hide it), and the computed/offset table types keep
// their offsets out-of-line in data, which is never disassembled at all.
//
// This xex is enc=1/comp=1 and XenonUtils\xex.cpp already decrypts+expands it, so
// the dump reuses that path instead of reimplementing AES+BASIC in Python.
// Purely additive: no analysis behaviour changes, and the argument is optional.
static void DumpImage(const Image& image, const char* path)
{
    // image.data IS the flat decrypted/decompressed image already: xex.cpp
    // allocates security->imageSize and maps every section into it at its own
    // RVA, so file offset == VA - image.base with no reassembly needed.
    //
    // Do NOT rebuild this by walking image.sections and memcpy'ing section.data
    // for section.size. Sections are Mapped with Misc.VirtualSize, which for the
    // trailing section can run PAST the end of the imageSize allocation those
    // pointers point into -- copying section.size bytes segfaults. (It did.)
    if (image.data == nullptr || image.size == 0)
    {
        fmt::println("DumpImage: image has no data, nothing written");
        return;
    }

    // C stdio, and the byte count is CHECKED. An ofstream here wrote a 0-byte
    // file while reporting nothing wrong, and a silently-empty dump would have
    // made every audit verdict downstream meaningless.
    FILE* f = nullptr;
    f = fopen(path, "wb");
    if (f == nullptr)
    {
        fmt::println("DumpImage: cannot open {} for writing", path);
        return;
    }

    // Chunked deliberately: a single 23 MB fwrite returned 0 with errno=EINVAL on
    // this UCRT, while a 454 KB ofstream write in the same process succeeded.
    // 1 MB chunks write the whole image reliably, and a short count still names
    // the exact offset that failed instead of just "0 of 23199744".
    const uint8_t* p = image.data.get();
    const size_t kChunk = 1u << 20;
    size_t written = 0;

    while (written < image.size)
    {
        size_t want = image.size - written;
        if (want > kChunk)
            want = kChunk;

        const size_t got = fwrite(p + written, 1, want, f);
        written += got;

        if (got != want)
        {
            fmt::println("DumpImage: write failed at offset 0x{:X} ({} of {}, ferror={} errno={})",
                written, got, want, ferror(f), errno);
            break;
        }
    }

    fclose(f);

    if (written != image.size)
        fmt::println("DumpImage: SHORT WRITE {} of {} bytes -> {}", written, image.size, path);
    else
        fmt::println("image dump: base 0x{:X} size 0x{:X} -> {}", image.base, image.size, path);
}

void RegisterFunctionsSearch(Image& image)
{
    uint32_t baseAddress = UINT32_MAX;

    for (const auto& section : image.sections) {
        if (section.name == ".text") {
            baseAddress = section.base;

            if (baseAddress == UINT32_MAX) {
                fmt::println("Could not find \".text\" section.");
                return;
            }

            uint32_t restgprlr_14 = BytePatternSearch(section.data, section.size, baseAddress, RESTGPRLR_14, sizeof(RESTGPRLR_14));
            uint32_t savegprlr_14 = BytePatternSearch(section.data, section.size, baseAddress, SAVEGPRLR_14, sizeof(SAVEGPRLR_14));
            uint32_t restfpr_14 = BytePatternSearch(section.data, section.size, baseAddress, RESTFPR_14, sizeof(RESTFPR_14));
            uint32_t savefpr_14 = BytePatternSearch(section.data, section.size, baseAddress, SAVEFPR_14, sizeof(SAVEFPR_14));
            uint32_t restvmx_14 = BytePatternSearch(section.data, section.size, baseAddress, RESTVMX_14, sizeof(RESTVMX_14));
            uint32_t savevmx_14 = BytePatternSearch(section.data, section.size, baseAddress, SAVEVMX_14, sizeof(SAVEVMX_14));
            uint32_t restvmx_64 = BytePatternSearch(section.data, section.size, baseAddress, RESTVMX_64, sizeof(RESTVMX_64));
            uint32_t savevmx_64 = BytePatternSearch(section.data, section.size, baseAddress, SAVEVMX_64, sizeof(SAVEVMX_64));

            fmt::println("restgprlr_14_address = 0x{:X}", restgprlr_14);
            fmt::println("savegprlr_14_address = 0x{:X}", savegprlr_14);
            fmt::println("restfpr_14_address = 0x{:X}", restfpr_14);
            fmt::println("savefpr_14_address = 0x{:X}", savefpr_14);
            fmt::println("restvmx_14_address = 0x{:X}", restvmx_14);
            fmt::println("savevmx_14_address = 0x{:X}", savevmx_14);
            fmt::println("restvmx_64_address = 0x{:X}", restvmx_64);
            fmt::println("savevmx_64_address = 0x{:X}", savevmx_64);
        }
    }
}

int SearchInstr(int start, uint32_t* code, uint64_t base, int id, ppc_insn& insn) {
    for (int k = start; k < 64; k++) {
        ppc::Disassemble(code + k, base + 4*k, insn);

        if (insn.opcode->id == id) {
            return k;
        }
    }
}

void ReadTable(Image& image, SwitchTable& table)
{
    uint32_t pOffset;
    ppc_insn insn;
    int reg1;
    
    auto* code = (uint32_t*)image.Find(table.base);
    ppc::Disassemble(code, table.base, insn);
    pOffset = insn.operands[1] << 16;

    SearchInstr(1, code, table.base, PPC_INST_ADDI, insn);
    pOffset += insn.operands[2];

    if (table.type == SWITCH_ABSOLUTE)
    {
        const auto* offsets = (be<uint32_t>*)image.Find(pOffset);
        for (size_t i = 0; i < table.labels.size(); i++)
        {
            table.labels[i] = offsets[i];
        }
    }
    else if (table.type == SWITCH_COMPUTED)
    {
        uint32_t base;
        uint32_t shift;
        const auto* offsets = (uint8_t*)image.Find(pOffset);

        int sw_start = SearchInstr(1, code, table.base, PPC_INST_RLWINM, insn);
        shift = insn.operands[2];

        sw_start = SearchInstr(sw_start, code, table.base, PPC_INST_LIS, insn);
        base = insn.operands[1] << 16;

        sw_start = SearchInstr(sw_start, code, table.base, PPC_INST_ADDI, insn);
        base += insn.operands[2];

        for (size_t i = 0; i < table.labels.size(); i++)
        {
            table.labels[i] = base + (offsets[i] << shift);
        }
    }
    else if (table.type == SWITCH_BYTEOFFSET || table.type == SWITCH_SHORTOFFSET)
    {
        if (table.type == SWITCH_BYTEOFFSET)
        {
            const auto* offsets = (uint8_t*)image.Find(pOffset);
            uint32_t base;

            int sw_start = SearchInstr(2, code, table.base, PPC_INST_LIS, insn);
            base = insn.operands[1] << 16;

            sw_start = SearchInstr(sw_start, code, table.base, PPC_INST_ADDI, insn);
            base += insn.operands[2];

            for (size_t i = 0; i < table.labels.size(); i++)
            {
                table.labels[i] = base + offsets[i];
            }
        }
        else if (table.type == SWITCH_SHORTOFFSET)
        {
            const auto* offsets = (be<uint16_t>*)image.Find(pOffset);
            uint32_t base;

            int sw_start = SearchInstr(3, code, table.base, PPC_INST_LIS, insn);
            base = insn.operands[1] << 16;

            sw_start = SearchInstr(sw_start, code, table.base, PPC_INST_ADDI, insn);
            base += insn.operands[2];

            for (size_t i = 0; i < table.labels.size(); i++)
            {
                table.labels[i] = base + offsets[i];
            }
        }
    }
    else
    {
        assert(false);
    }
}

// maxBack = how many instructions to walk backwards looking for the bounds check
// (`bgt`/`ble` on a `cmplwi` of the index register) that gives the table its default
// label and its length.
//
// s33: this was a hard-coded 32 and that is not always enough. The dash's zlib
// `inflate()` has its state switch at 0x92230900 with the guarded compare at
// 0x922307E4 -- 71 instructions earlier, because the compiler hoisted the whole
// prologue reload block between them. The table was therefore never emitted, the
// recompiler lowered the `bctr` to a plain indirect call, and the guest branched to
// 0x92230928 (case 0), an address in the middle of a function with no entry in Emu's
// guest->host table: "No image data found for guest VA" and a fatal.
//
// Widening is strictly ADDITIVE: the loop breaks at the first bgt/ble + cmplwi pair it
// finds, so iterations 0..31 are identical to before and every table that was found
// before is found identically now. Only matches that previously produced nothing can
// change, and those produced no [[switch]] entry at all.
// s33: a jump table whose labels are not all 4-byte-aligned instructions inside a code
// section of this image cannot be real -- PowerPC branch targets are always word
// aligned, and a label outside the image is not code at all. Such a table means
// ScanTable latched onto an unrelated compare (wrong length, so ReadTable walked off the
// end of the real table into whatever data follows it), and emitting it is worse than
// emitting nothing: the recompiler lowers the `bctr` to a switch over addresses that are
// not branch targets. Widening the backward window makes that mis-latch more likely, so
// the check comes in with it. Two such tables already existed at the old window in the
// NFS image (0x825725B0 n=110, 0x82576FD0 n=1804) and none in the dash.
static bool TableLabelsPlausible(const Image& image, const SwitchTable& table)
{
    if (table.labels.empty())
        return false;

    auto isCode = [&](size_t va)
        {
            for (const auto& section : image.sections)
            {
                if (!(section.flags & SectionFlags_Code))
                    continue;
                if (va >= section.base && va < section.base + section.size)
                    return true;
            }
            return false;
        };

    for (const auto& label : table.labels)
    {
        if ((label & 3) != 0 || !isCode(label))
            return false;
    }

    return true;
}

// s4: the register a dispatch ACTUALLY indexes with, read off the matched
// pattern. Ground truth by construction -- it is what the hardware indexes with.
//
// Raw instruction words are decoded here rather than going through ppc_insn
// operands, because the operand ORDER is a property of the binutils tables and
// this must not silently follow a change there.
//
// Returns UINT32_MAX when it cannot tell, which disables validation for that
// site and leaves the old behaviour exactly as it was.
static uint32_t DispatchIndexRegister(const uint32_t* code, int span = 16)
{
    uint32_t baseRegs = 0;   // registers holding the TABLE BASE, not the index

    for (int k = 0; k < span; k++)
    {
        const uint32_t w = ByteSwap(code[k]);
        const uint32_t op = (w >> 26) & 0x3F;
        const uint32_t rD = (w >> 21) & 0x1F;
        const uint32_t rA = (w >> 16) & 0x1F;
        const uint32_t rB = (w >> 11) & 0x1F;

        if (op == 15)                       // addis: starts a table-base chain
        {
            baseRegs |= (1u << rD);
            continue;
        }

        if (op == 14)                       // addi: continues one, or breaks it
        {
            if (baseRegs & (1u << rA))
                baseRegs |= (1u << rD);
            else
                baseRegs &= ~(1u << rD);
            continue;
        }

        if (op == 31)
        {
            const uint32_t xo = (w >> 1) & 0x3FF;
            if (xo == 23 || xo == 87 || xo == 279)   // lwzx / lbzx / lhzx
            {
                // EA = rA + rB; whichever operand is not the table base is the
                // (possibly scaled) index.
                uint32_t idx = (baseRegs & (1u << rA)) ? rB : rA;

                // Unscale, but only through the NEAREST definition of idx. A
                // loose search for "any rlwinm writing idx" is wrong when the
                // register is rebuilt afterwards: at 0x831141E8 the guest forms
                // rlwinm r11,r6,1 / rlwinm r9,r5,0,24,31 / or r11,r11,r9, so the
                // index is r11 and unscaling to r6 would condemn a correct entry.
                for (int j = k - 1; j >= 0; j--)
                {
                    const uint32_t p = ByteSwap(code[j]);
                    const uint32_t pop = (p >> 26) & 0x3F;
                    const uint32_t pD = (p >> 21) & 0x1F;
                    const uint32_t pA = (p >> 16) & 0x1F;

                    if (pop == 21 && pA == idx)          // rlwinm rA,rS,SH,MB,ME
                    {
                        const uint32_t sh = (p >> 11) & 0x1F;
                        if (sh >= 1 && sh <= 3)
                            idx = pD;                    // the rS field
                        break;
                    }
                    if ((pop == 14 || pop == 15) && pD == idx)
                        break;                           // addi/addis wrote it
                    if (pop == 31 && ((p >> 1) & 0x3FF) == 444 && pA == idx)
                        break;                           // or/mr wrote it
                }
                return idx;
            }
        }
    }

    return UINT32_MAX;
}

// s4: GPRs this instruction writes, decoded from the raw word. `copySrc` is set
// when the write is a plain register-to-register copy (mr, or a zero-extending
// rlwinm), so the caller can follow the value backwards through it.
//
// Mirrors defines() in tools\audit_switch_tables.py deliberately, including
// its treatment of unmodelled instructions as writing nothing. The audit is the
// gate on this analyser, so the two must agree: if this were stricter it would
// drop tables the audit calls fine, and if looser it would keep tables the audit
// condemns.
static uint32_t GprDefs(uint32_t w, uint32_t& copySrc)
{
    copySrc = UINT32_MAX;
    const uint32_t op = (w >> 26) & 0x3F;
    const uint32_t rD = (w >> 21) & 0x1F;
    const uint32_t rA = (w >> 16) & 0x1F;
    const uint32_t rB = (w >> 11) & 0x1F;

    switch (op)
    {
    case 7: case 12: case 13:                       // mulli, addic, addic.
    case 14: case 15:                               // addi, addis
        return 1u << rD;

    case 21:                                        // rlwinm rA,rS,SH,MB,ME
        if (((w >> 11) & 0x1F) == 0)                // SH == 0: a zero-extend copy
            copySrc = rD;                           // the rS field
        return 1u << rA;

    case 24: case 25: case 26: case 27: case 28: case 29:   // ori .. andis.
        return 1u << rA;

    case 32: case 33: case 34: case 35: case 40: case 41:
    case 42: case 43: case 46: case 48: case 49: case 50:
    case 51: case 58:                               // D-form loads
        return 1u << rD;

    case 31:
    {
        const uint32_t xo = (w >> 1) & 0x3FF;
        if (xo == 444)                              // or rA,rS,rB (mr when rS==rB)
        {
            if (rD == rB)
                copySrc = rD;
            return 1u << rA;
        }
        if (xo == 23 || xo == 87 || xo == 279 || xo == 266)  // lwzx/lbzx/lhzx/add
            return 1u << rD;
        return 0;
    }

    default:
        return 0;                                   // stores, branches, compares, unmodelled
    }
}

// indexReg: the dispatch's real index register, or UINT32_MAX to skip validation.
void ScanTable(const uint32_t* code, size_t base, SwitchTable& table, int maxBack = 128,
    uint32_t indexReg = UINT32_MAX)
{
    ppc_insn insn;
    uint32_t cr{ (uint32_t)-1 };

    // s4: reverse dataflow on the index value, WITH REDEFINITION TRACKING.
    //
    // `live` is the set of registers still carrying the index at the point the
    // walk has reached; `aliases` accumulates everything that was ever live, and
    // is what a compare is judged against, so a bounds check written on a
    // pre-copy register still validates. Once a live register is written by
    // anything that is not a copy it leaves the set -- nothing earlier that
    // mentions it refers to the index. When `live` empties there is no valid
    // compare to find and the walk is finished.
    //
    // ⚠ AN EARLIER VERSION ONLY EVER ADDED, NEVER REMOVED, and precomputed the
    // set in one sweep. It was wrong, and it cost a launch. At 0x82B8E638 the
    // guest does `lwz r10,308(r1)` in the instruction immediately before the
    // dispatch, so nothing earlier can alias r10 -- yet the additive set pulled
    // in {5, 8, 27} from copies further back and blessed a table recorded as
    // r=5. This validation had ALREADY rejected that entry correctly; the loose
    // audit rule declared the rejection a false loss and it was "fixed" back in.
    // The build then died at exactly that dispatch with index=65535.
    // A widening-only alias rule cannot reject anything; it can only excuse.
    // PASS 1 -- build the alias set, in its own sweep, WITH redefinition tracking.
    // Both halves of that are load-bearing, and each was learned the hard way:
    //
    //  * IT MUST BE A SEPARATE PASS. A compare's `mr` can sit at a LOWER address
    //    than the compare itself, so a single backward walk reaches the compare
    //    before it has learned the alias. The dash does this twice
    //    (0x921B635C: `mr r31,r4` then `cmplwi r4,18`, dispatch on r31;
    //    0x922AF8B0: `mr r25,r4` then `cmplwi r4,22`, dispatch on r25) -- an
    //    incremental walk rejects both, and they are perfectly valid.
    //
    //  * IT MUST TRACK REDEFINITION. `live` holds the registers still carrying
    //    the index; once one is written by anything that is not a copy it leaves,
    //    and when `live` empties nothing further can alias. `aliases` accumulates
    //    everything that was ever live and is what a compare is judged against.
    //    An earlier version only ever ADDED, and it cost a launch: at 0x82B8E638
    //    the guest does `lwz r10,308(r1)` in the instruction immediately before
    //    the dispatch, so nothing earlier can alias r10 -- but the additive set
    //    pulled in {5, 8, 27} from copies further back and blessed a table
    //    recorded as r=5. The s4 build died at that dispatch with index=65535.
    //    A widening-only alias rule cannot reject anything; it can only excuse.
    //
    // Note the walk deliberately does NOT stop at the first kill: it stops when
    // `live` is empty. A register that is rematerialised (0x823C53BC recomputes
    // an identical `addi r11,r28,-1` after the branch) or that has no local
    // definition at all (0x8259FEA8 is preceded by another function's `b` and
    // padding) still ends up in `aliases` under its own name, so its compare
    // validates.
    uint32_t live = (indexReg == UINT32_MAX) ? 0u : (1u << indexReg);
    uint32_t aliases = (indexReg == UINT32_MAX) ? 0xFFFFFFFFu : (1u << indexReg);

    for (int i = 1; i < maxBack && live != 0; i++)
    {
        uint32_t copySrc = UINT32_MAX;
        const uint32_t defs = GprDefs(ByteSwap(code[-i]), copySrc);
        const uint32_t hit = live & defs;
        if (hit != 0)
        {
            // Clear BEFORE adding the copy source, or a SELF-copy annihilates
            // itself: `rlwinm r11,r11,0,0,31` is a plain zero-extend and a real
            // idiom (0x8223FB90, 0x82CAE6AC both sit directly behind one).
            live &= ~hit;
            if (copySrc != UINT32_MAX)
            {
                live |= (1u << copySrc);
                aliases |= (1u << copySrc);
            }
        }
    }

    // PASS 2 -- the latch walk, unchanged except that a compare must name a
    // register in `aliases`.
    for (int i = 0; i < maxBack; i++)
    {

        ppc::Disassemble(&code[-i], base - (4 * i), insn);
        if (insn.opcode == nullptr)
        {
            continue;
        }

        if (cr == -1 &&(insn.opcode->id == PPC_INST_BGT || insn.opcode->id == PPC_INST_BGTLR || insn.opcode->id == PPC_INST_BLE || insn.opcode->id == PPC_INST_BLELR))
        {
            cr = insn.operands[0];
            if (insn.opcode->operands[1] != 0)
            {
                table.defaultLabel = insn.operands[1];
            }
        }
        else if (cr != -1)
        {
            if (insn.opcode->id == PPC_INST_CMPLWI && insn.operands[0] == cr)
            {
                // s4: REJECT a bounds check on a register the dispatch does not
                // index with, and keep walking.
                //
                // This walk has no stop at a basic-block boundary, so it can
                // reach back into an ENTIRELY DIFFERENT function and adopt that
                // function's bounds check whole -- register, default label and
                // length. That is what killed Fable II in s3: sub_822D8180
                // indexes with r5, the previous function ends in `blr` at
                // 0x822D8178 and contains its own switch on r9 with four labels,
                // and this loop crossed the blr and recorded r=9/4 labels for a
                // dispatch whose guest table has five entries. The recompiler
                // then emitted `switch (r9)` with four cases and a `default:
                // __builtin_unreachable()`, which let clang drop the bounds check
                // entirely -- an unguarded scaled load, fatal on index 4.
                //
                // TableLabelsPlausible cannot see this: the stolen labels are
                // word-aligned addresses in a code section, merely the wrong ones.
                //
                // A BLOCK-TERMINATOR STOP WAS MEASURED AND REJECTED. Auditing all
                // 876 tables against the guest image showed 17 whose accepted
                // compare lies beyond a `blr`/`b`/`bctr`, and FIVE of those are
                // correct -- the compiler emits `cmplwi/bgt` then an
                // unconditional `b` into the dispatch, so a terminator sits
                // between them in address order while the guard is perfectly
                // real. Three were confirmed against the table's own extent
                // (0x8233887C -- whose r30 is a verified `mr r7,r30` /
                // `mr r30,r3` copy chain -- plus 0x8237DE7C and 0x8259FEA8), and
                // two more are out-of-line tables (0x8304D2A0, 0x831A4B18) that
                // scan_jumptables_f2.py could not have recovered, since it only
                // handles inline ones. Stopping at terminators would have deleted
                // five working tables and permanently lost two.
                //
                // (A sixth cross-terminator entry, 0x82B8E638, LOOKED correct
                // under the original additive alias rule and was the entry that
                // then crashed the s4 launch. The register check below, with
                // redefinition tracking, rejects it. Its length was right and its
                // register was not -- which is why length agreement alone must
                // never be taken as a clean bill of health.)
                //
                // Register validation costs nothing on a correct entry and cannot
                // drop a table whose compare genuinely belongs to it.
                if (!(aliases & (1u << insn.operands[1])))
                {
                    cr = (uint32_t)-1;
                    continue;
                }

                table.r = insn.operands[1];
                table.labels.resize(insn.operands[2] + 1);
                table.base = base;
                break;
            }
        }
    }
}

void MakeMask(const uint32_t* instructions, size_t count)
{
    ppc_insn insn;
    for (size_t i = 0; i < count; i++)
    {
        ppc::Disassemble(&instructions[i], 0, insn);
        fmt::println("0x{:X}, // {}", ByteSwap(insn.opcode->opcode | (insn.instruction & insn.opcode->mask)), insn.opcode->name);
    }
}

void* SearchMask(const void* source, const uint32_t* compare, size_t compareCount, size_t size)
{
    assert(size % 4 == 0);
    uint32_t* src = (uint32_t*)source;
    size_t count = size / 4;
    ppc_insn insn;

    for (size_t i = 0; i < count; i++)
    {
        size_t c = 0;
        size_t off = 0;

        for (c = 0; c < compareCount; c++)
        {
            ppc::Disassemble(&src[i + c + off], 0, insn);
            if (insn.opcode == nullptr)
            {
                break;
            }

            if (insn.opcode->id == PPC_INST_NOP) {
                off++;
                c--;
                continue;
            }

            if (insn.opcode->id != compare[c]) {
                break;
            }
        }

        if (c == compareCount)
        {
            return &src[i];
        }
    }

    return nullptr;
}

static std::string out;

template<class... Args>
static void println(fmt::format_string<Args...> fmt, Args&&... args)
{
    fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    out += '\n';
};

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("Usage: XenonAnalyse [input XEX file path] [output jump table TOML file path] [optional flat image dump path]");
        return EXIT_SUCCESS;
    }

    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

       RegisterFunctionsSearch(image);

    if (argc >= 4)
    {
        DumpImage(image, argv[3]);
    }

    auto printTable = [&](const SwitchTable& table)
        {
            println("[[switch]]");
            println("base = 0x{:X}", table.base);
            println("r = {}", table.r);
            println("default = 0x{:X}", table.defaultLabel);
            println("labels = [");
            for (const auto& label : table.labels)
            {
                println("    0x{:X},", label);
            }

            println("]");
            println("");
        };

    std::vector<SwitchTable> switches{};

    println("# Generated by XenonAnalyse");

    auto scanPattern = [&](uint32_t* pattern, size_t count, size_t type)
        {
            for (const auto& section : image.sections)
            {
                if (!(section.flags & SectionFlags_Code))
                {
                    continue;
                }

                size_t base = section.base;
                uint8_t* data = section.data;
                uint8_t* dataStart = section.data;
                uint8_t* dataEnd = section.data + section.size;
                while (data < dataEnd && data != nullptr)
                {
                    data = (uint8_t*)SearchMask(data, pattern, count, dataEnd - data);

                    if (data != nullptr)
                    {
                        SwitchTable table{};
                        table.type = type;
                        // never walk backwards out of the section
                        int maxBack = (int)((data - dataStart) / 4);
                        if (maxBack > 128) maxBack = 128;
                        ScanTable((uint32_t*)data, base + (data - dataStart), table, maxBack,
                            DispatchIndexRegister((uint32_t*)data));

                        // fmt::println("{:X} ; jmptable - {}", base + (data - dataStart), table.labels.size());
                        if (table.base != 0)
                        {
                            ReadTable(image, table);
                            if (TableLabelsPlausible(image, table))
                            {
                                printTable(table);
                                switches.emplace_back(std::move(table));
                            }
                            else
                            {
                                println("# rejected implausible table at 0x{:X} ({} labels)", table.base, table.labels.size());
                                println("");
                            }
                        }

                        data += 4;
                    }
                    continue;
                }
            }
        };

    uint32_t absoluteSwitch[] =
    {
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_RLWINM,
        PPC_INST_LWZX,
        PPC_INST_MTCTR,
        PPC_INST_BCTR,
    };

    uint32_t absoluteSwitch2[] =
    {
        PPC_INST_LIS,
        PPC_INST_RLWINM,
        PPC_INST_ADDI,
        PPC_INST_LWZX,
        PPC_INST_MTCTR,
        PPC_INST_BCTR,
    };

    uint32_t computedSwitch[] =
    {
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_LBZX,
        PPC_INST_RLWINM,
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_ADD,
        PPC_INST_MTCTR,
    };

    uint32_t offsetSwitch[] =
    {
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_LBZX,
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_ADD,
        PPC_INST_MTCTR,
    };

    uint32_t wordOffsetSwitch[] =
    {
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_RLWINM,
        PPC_INST_LHZX,
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_ADD,
        PPC_INST_MTCTR,
    };

    uint32_t wordOffsetSwitch2[] =
    {
        PPC_INST_LIS,
        PPC_INST_RLWINM,
        PPC_INST_ADDI,
        PPC_INST_LHZX,
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_ADD,
        PPC_INST_MTCTR,
    };

    println("# ---- ABSOLUTE JUMPTABLE ----");
    scanPattern(absoluteSwitch, std::size(absoluteSwitch), SWITCH_ABSOLUTE);
    scanPattern(absoluteSwitch2, std::size(absoluteSwitch2), SWITCH_ABSOLUTE);

    println("# ---- COMPUTED JUMPTABLE ----");
    scanPattern(computedSwitch, std::size(computedSwitch), SWITCH_COMPUTED);

    println("# ---- OFFSETED JUMPTABLE ----");
    scanPattern(offsetSwitch, std::size(offsetSwitch), SWITCH_BYTEOFFSET);
    scanPattern(wordOffsetSwitch, std::size(wordOffsetSwitch), SWITCH_SHORTOFFSET);
    scanPattern(wordOffsetSwitch2, std::size(wordOffsetSwitch2), SWITCH_SHORTOFFSET);

    std::ofstream f(argv[2]);
    f.write(out.data(), out.size());

    return EXIT_SUCCESS;
}
