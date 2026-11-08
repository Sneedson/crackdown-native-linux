# XenonRecomp-XEO3

A proof-of-concept fork of XenonRecomp aimed at allowing arbitrary games to run on Microsoft's official XEO3 emulator.

Original README can be found [here](./README.orig.md).

> **In xeo3-recomp-kit** this is the recompiler, driven by the kit's scripts
> (`scripts\setup.ps1` builds it, `scripts\recompile.ps1` runs it). The text below is
> WitherOrNot's original write-up of the fork; the next section lists what has been added since.

## What this fork adds

WitherOrNot's proof of concept recompiled one visual novel. Taking full retail games
(Crackdown, Fable II, Need for Speed: Most Wanted), the Xbox 360 kernel and the dashboard
through it needed these changes, each found by a crash or a wrong result:

**Loading**
- recompile the 360 kernel (`xboxkrnlce.bin`), a raw big-endian PowerPC PE rather than a
  XEX, loaded as the flat memory image the emulator maps it as
- import records walked to the end of the blob (not `numImports` times), and each record's
  export map chosen by its own name index; the import dialect remapped to the emulator's
- `XexRepack`: re-emit a XEX as unencrypted/basic-compressed for the recompiler to read,
  leaving the header (and therefore the DLL name) byte-identical

**Code generation**
- `mftb` lowered to `zz_mftb()`, the 360's 50 MHz timebase, instead of the raw CPU counter
  (every timer ran 70-100x fast)
- `setjmp`/`longjmp` for the emulator's ABI: `PPC_SETJMP`/`PPC_LONGJMP` (host `setjmp` in the
  surviving frame + a snapshot of the guest's non-volatile registers), emitted at every call
  site when the config names the C runtime's two functions. Without it a guest `longjmp`
  just returned to its caller
- the next PC returned in `ECX`, traps and host imports serviced, local imports given the
  context, the idle-loop services implemented
- CTR-decrement branches that leave the function emitted as tail calls
- new or fixed instructions: `vpkd3d128`/`vupkd3d128` types 2 (10:10:10:2), 5 (float16_4) and
  SHORT (it zeroed every negative lane, which broke normals and reflections game-wide),
  `lhaux`, `lhbrx`, `vsubuwm`, `vcmpgtsb`; an unimplemented instruction now prints its
  opcode before trapping

**Jump tables**
- a live default arm: an index the table doesn't list falls back to an indirect call and is
  reported through `PPC_SWITCH_MISS`, instead of `__builtin_unreachable()` letting clang
  delete the bounds check
- XenonAnalyse validates the bounds-check register, finds checks further back, rejects
  implausible tables, and no longer loops forever on backward branches into a predecessor
- XenonAnalyse writes a flat image dump (third argument) for the analysis tools

The source comments at each change say what broke and how it was found.

## XEO3 Internals

This emulator mainly simulates the hypervisor and important hardware (Audio and GPU). Everything else from kernel downwards just runs within this environment, which is what makes it possible to have an [emulated dashboard](https://www.youtube.com/watch?v=PcD0knw-QN8) with the right setup, and it's also why a set of system binaries (devkit-signed, version 17000.3) are distributed with the emulator. 

Interestingly, XEO3 doesn't emulate PowerPC at all to achieve this functionality. Instead, DLLs are loaded which contain recompiled PPC code for each module loaded by the hypervisor (some sources claim that these DLLs are shaders, they are not). Despite not needing the corresponding XEX files for their code, the emulator still loads and maps them into memory to make their data sections accessible.

Each dll has the following minimum required exports:

- `InitPrecompiledDll` - Initialize mapping between PowerPC code addresses and native DLL adddresses, load emulator callbacks, return flags and version
- `CleanupPrecompiledDll` - Cleanup before DLL is unloaded
- `PrecompiledPointers` - Described below
- `PrecompiledImportTable` - Map between import thunks and corresponding function addresses, filled in by emulator on load

There is also functionality to load XEX data patches directly from the DLL.

XBEs also have corresponding DLLs containing recompiled x86 code. I have not looked into these as much, but most of the info about PPC's DLLs should apply here as well.

### PrecompiledPointers

This is a table of function pointers needed for interacting with the emulator.

```
0 - void __fastcall CallGuestFunc(uint32_t ppc_func_addr)             // Call arbitrary address in emulated system address space
1 - void __fastcall CallHostFunction(void* unknown, uint16_t ordinal) // Call various emulator-provided functions
2 - void __fastcall Syscall()                                         // Do syscall, with syscall number in r3
3 - uint32_t __fastcall GetCR(PPCContext* ctx)                        // Get value of full CR register
4 - uint64_t __fastcall GetFrequency()                                // Get emulator frequency
5 - void __fastcall RaiseException(uint64_t address)                  // Raise exception from guest code at specified address
6 - unused
7 - unused
8 - uint32_t __fastcall MMIORead(uint32_t address)                    // Read MMIO address (value is endian-swapped)
9 - void __fastcall MMIOWrite(uint32_t address, uint32_t value)       // Write MMIO address (value is endian-swapped)
```

Most of these are only used by the kernel or XeFu.

### Calling convention

The recompiled PowerPC code uses a highly non-standard calling convention that resembles MSVC's [`__preserve_none`](https://learn.microsoft.com/en-us/cpp/cpp/preserve-none?view=msvc-170) but is incompatible with it.

```
Arguments:
rbx - Address of PowerPC register context + 0x80 (address of r3 register)
r15 - Base address of virtual memory
r14 - Unknown, stack frame?
r13 - Unknown, treated as both argument/scratch register

All other registers clobbered, stack is callee-cleanup.
```

I had to create a [custom version](https://github.com/WitherOrNot/llvm-project/releases) of clang that replaces `preserve_none` with this calling convention for the generated code to work correctly.

## Compilation

1. Clone and build with Visual Studio CMake.
2. Run `XenonAnalyse.exe default.xex switch.toml` to collect switch statement and register save-restore info.
3. Create `config.toml` ([reference](./example/config.toml)) containing path to `switch.toml` along with function boundaries, MMIO instructions, and invalid instructions as needed. 
4. Run `XenonRecomp.exe config.toml ppc_context.h`, with `ppc_context.h` from `XenonUtils` folder.
5. Copy `ppc_precomp_init.cpp` from `XenonUtils` folder to recompiler output directory.
6. Compile the source files to DLL with custom clang, using `-mavx` (`/arch:AVX` for `clang-cl`).

## Current status

Chaos;Head Love Chu Chu was successfully recompiled and runs playably in XEO3 using this recompilation method.