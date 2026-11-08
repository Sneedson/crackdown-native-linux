#include "ppc_recomp_shared.h"
#include <windows.h>

// s10: every module in the modern XeO3 dump reports 34 (0x22) -- that is the
// literal `mov eax,0x22` at the end of the dump kernel's InitPrecompiledDll,
// and "version 34" is what Emu prints for all of them. We reported 46, which
// made our module the only one announcing a different protocol version.
#define VERSION 0x22

// Guest module name reported in the registration descriptor. Override per
// module (e.g. in ppc_config.h) — informational, but mirrors the official DLLs.
#ifndef PPC_MODULE_NAME
#define PPC_MODULE_NAME "module"
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

__declspec(dllexport) extern "C" uintptr_t PrecompiledPointers[10] = {0};

// ---------------------------------------------------------------------------
// Module registration handshake, reverse-engineered from the official
// xeo3_5fb3687c_001748c4.dll (the shipped native-mode fusion-kernel DLL,
// InitPrecompiledDll at RVA 0x116d20). Without this registration Emu's
// guest-VA resolver (region table) has no entry for the module, and the
// first execution continuation after an hvdata callback dies with
// "Unable to execute precompiled code for IAR <garbage>".
//
// Contract:
//   setupObj->vtbl[+0x28](setupObj, &outA, &outB, &outC)  // host glue ptrs
//   registrar = setupObj->vtbl[+0xA8](setupObj)
//   registrar->vtbl[0](registrar, &block)                 // register module
//
// s10 — MEASURED, and it contradicts the paragraph above: on the Emu shipped
// with the modern XeO3 dumps this registration NEVER HAPPENS, for any module,
// and everything boots anyway. Do not chase it again.
//   - The dump's own kernel (xeo3_5d12c3b3_00175344.dll, InitPrecompiledDll at
//     RVA 0x114E50) fetches its registrar with `call qword ptr [rax+0xA8]`
//     = vtbl[21] -- and in Emu.exe vtbl[21] is rva 0x2B530, which is literally
//     `xor eax,eax; ret`. Slots 28/29/30 share that same null stub.
//   - vtbl[33] (+0x108, rva 0x64680) IS a real getter (`mov rax,[rip+..]; ret`)
//     but the global it returns is null at InitPrecompiledDll time.
// So `registrar` is NULL either way; the stock kernel takes its own error path
// and runs fine. vtbl[33] is kept because it is at least the genuine getter.
// The garbage-IAR failure described above therefore has a DIFFERENT cause on
// this Emu -- registration is not it.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct PrecompDescriptor
{
    uint8_t  hasImageData;   // 0: no embedded guest-image blob
    uint8_t  unk1;           // 0x4D in the official DLL
    uint16_t pad2;
    uint32_t unk4;           // 0xD
    uint32_t unk8;           // 0xD
    uint32_t nameOffset;     // 0x18 (offset of name[] below)
    uint32_t guestBase;      // guest VA the module is loaded at
    uint32_t guestSize;      // guest image size
    char     name[16];       // guest module file name
    uint8_t  zeros[8];
    uint8_t  tailFF[20];     // 20-byte digest field; all-0xFF in the official DLL
    uint8_t  tailZero[12];
};
#pragma pack(pop)

struct PrecompRegBlock
{
    const void* descriptor;   // +0x00
    const void* moduleBase;   // +0x08 host DLL image base
    uint32_t    sizeOfImage;  // +0x10 host DLL SizeOfImage
    uint32_t    pad14;
    const char* moduleName;   // +0x18 host DLL file path
    const void* imageData;    // +0x20 embedded guest image blob (unused: 0)
    uint32_t    imageSize;    // +0x28
    uint32_t    pad2C;
    uint8_t     registered;   // +0x30
    uint8_t     pad31[7];
};

static PrecompDescriptor g_descriptor = {
    0, 0x4D, 0, 0xD, 0xD, 0x18,
    (uint32_t)PPC_IMAGE_BASE, (uint32_t)PPC_IMAGE_SIZE,
    PPC_MODULE_NAME, {0},
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
     0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    {0}
};

static void* g_glue[3];
static void* g_registrar;
static char g_modulePath[0x104];
static PrecompRegBlock g_regBlock;

typedef void  (*PrecompGetGlueFn)(void* self, void** a, void** b, void** c);
typedef void* (*PrecompGetRegistrarFn)(void* self);
typedef void* (*PrecompRegisterFn)(void* self, PrecompRegBlock* block);

#ifdef PPC_INIT_TRACE
// Provided by the module's own trace unit (work_kernel/ppc/zz_trace.cpp).
extern void zz_log_init(unsigned long long** EmuSetupCallbacks, unsigned int* Info);
extern void zz_log_initf(const char* fmt, ...);
#endif

__declspec(dllexport) extern "C" int InitPrecompiledDll(PPCFuncMapping** CodeMap, int64_t* ElemCount, uintptr_t** EmuSetupCallbacks, uint32_t* Info) {
    uintptr_t module_base = reinterpret_cast<uintptr_t>(&__ImageBase);
    uint32_t emuVersion = Info[1];
#ifdef PPC_INIT_TRACE
    zz_log_init(reinterpret_cast<unsigned long long**>(EmuSetupCallbacks), Info);
#endif

    for (int i = 0; i < HostFuncsCount; i++) {
        uintptr_t rva = reinterpret_cast<uintptr_t>(HostFuncs[i]) - module_base;
        PPCFuncMappings[i].host_rva = static_cast<uint32_t>(rva);
    }

    *CodeMap = PPCFuncMappings;
    *ElemCount = HostFuncsCount;
    Info[0] = 1;
    Info[1] = VERSION;

    void* setupObj = reinterpret_cast<void*>(EmuSetupCallbacks);
    if (setupObj) {
        void** vtbl = *reinterpret_cast<void***>(setupObj);
        reinterpret_cast<PrecompGetGlueFn>(vtbl[5])(setupObj, &g_glue[0], &g_glue[1], &g_glue[2]);
        if (emuVersion >= 0x29)
            g_registrar = reinterpret_cast<PrecompGetRegistrarFn>(vtbl[33])(setupObj);
        if (g_registrar) {
            GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), g_modulePath, sizeof(g_modulePath));
            const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                reinterpret_cast<const uint8_t*>(&__ImageBase) + __ImageBase.e_lfanew);
            g_regBlock.descriptor = &g_descriptor;
            g_regBlock.moduleBase = &__ImageBase;
            g_regBlock.sizeOfImage = nt->OptionalHeader.SizeOfImage;
            g_regBlock.moduleName = g_modulePath;
            g_regBlock.imageData = nullptr;
            g_regBlock.imageSize = 0;
            void** rvtbl = *reinterpret_cast<void***>(g_registrar);
            reinterpret_cast<PrecompRegisterFn>(rvtbl[0])(g_registrar, &g_regBlock);
            g_regBlock.registered = 1;
        }
#ifdef PPC_INIT_TRACE
        zz_log_initf("  registrar=%p glue=%p/%p/%p codeMapCount=%d\n",
                     g_registrar, g_glue[0], g_glue[1], g_glue[2], HostFuncsCount);
#endif
    }

    // Version negotiation as in the official DLL: 0x21..0x2D -> echo Emu's
    // version, anything else (incl. 0x2E+) -> our VERSION.
    if (emuVersion - 0x21u <= 0xCu)
        return static_cast<int>(emuVersion);
    return VERSION;
}

__declspec(dllexport) extern "C" void CleanupPrecompiledDll() {

}

// Default sink for PPC_IMPORT_UNMAPPED (see ppc_context.h): a host-import
// stub whose ce->Emu ordinal translation is unverified is a silent no-op.
// zz_trace.cpp provides a strong override that logs each one.
extern "C" __attribute__((weak)) void zz_unmapped_import(unsigned int, unsigned int)
{
}

// PPC_IMPORT_LOCAL implementation (see ppc_context.h). 0x70A3/0x70A4 are the
// kernel's global-spinlock acquire/release; the donor Emu implemented them as
// Enter/LeaveCriticalSection on a single host global, replicated here. A
// CRITICAL_SECTION (recursive, like the donor's) rather than an SRW lock.
// 0x7013 (IRQL-change notify) is a deliberate no-op: the fusion kernel's
// homologous code paths omit the call entirely on this Emu.
// The lock lives HERE and only here. zz_trace.cpp's strong override of
// zz_local_import adds logging and then delegates to this, so there is exactly
// one host lock object per DLL whichever definition the linker keeps.
static CRITICAL_SECTION g_zzGlobalLock;
static INIT_ONCE g_zzGlobalLockOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK zz_init_global_lock(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_zzGlobalLock);
    return TRUE;
}

// s9: the idle-loop services (ce worker pump sub_80063300 + DPC poll
// sub_80062478). Donor Emu semantics, read from its disassembly:
//   0x7052 = WaitForSingleObject(perProcWorkEvent[proc], proc==0 ? 1 : INF)
//   0x7014 = dequeue up to r5 host work items for proc r3 into r4; r3 = count
//   0x709F = "software interrupt / work pending?" poll; r3 branched on
// The target Emu's dialect has none of these (cf's homolog of the DPC
// function -- cf 0x80062A48, body byte-identical to ce 0x80062478 -- omits
// 0x709F/0x709E/0x7012 entirely, and cf has no worker pump at all). Locally:
// a real 1ms event wait for 0x7052 (donor's proc-0 behavior; INFINITE would
// hang because nothing signals the event yet -- when host-side work delivery
// is wired up, signal g_zzWorkEvent), and defined r3 = 0 for 0x7014/0x709F
// ("queue empty" / "nothing pending"). 0x709E/0x7012 (DPC lock/barrier) stay
// no-ops: single guest thread, and cf runs that path with native flag writes.
static HANDLE g_zzWorkEvent;
static INIT_ONCE g_zzWorkEventOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK zz_init_work_event(PINIT_ONCE, PVOID, PVOID*)
{
    g_zzWorkEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    return TRUE;
}
__declspec(dllexport) extern "C" void zz_signal_work(void)
{
    InitOnceExecuteOnce(&g_zzWorkEventOnce, zz_init_work_event, nullptr, nullptr);
    SetEvent(g_zzWorkEvent);
}

extern "C" void zz_local_import_impl(unsigned int, unsigned int ordinal, void* ppcCtx)
{
    PPCContext* c = (PPCContext*)ppcCtx;
    switch (ordinal) {
    case 0x70A3:
        InitOnceExecuteOnce(&g_zzGlobalLockOnce, zz_init_global_lock, nullptr, nullptr);
        EnterCriticalSection(&g_zzGlobalLock);
        break;
    case 0x70A4:
        InitOnceExecuteOnce(&g_zzGlobalLockOnce, zz_init_global_lock, nullptr, nullptr);
        LeaveCriticalSection(&g_zzGlobalLock);
        break;
    case 0x7052: // wait for work, 1ms tick (donor: WaitForSingleObject)
        InitOnceExecuteOnce(&g_zzWorkEventOnce, zz_init_work_event, nullptr, nullptr);
        WaitForSingleObject(g_zzWorkEvent, 1);
        break;
    case 0x7014: // dequeue host work items: queue is empty, r3 = 0
    case 0x709F: // work-pending poll: nothing pending, r3 = 0
        c->r3.u64 = 0;
        break;
    default: // 0x7013 IRQL notify, 0x709E/0x7012 DPC lock/barrier: no-ops
        break;
    }
}

// Weak entry point so a module without a trace unit still links; zz_trace.cpp
// overrides this with a logging version that delegates to the impl above.
extern "C" __attribute__((weak)) void zz_local_import(unsigned int addr, unsigned int ordinal, void* ppcCtx)
{
    zz_local_import_impl(addr, ordinal, ppcCtx);
}
