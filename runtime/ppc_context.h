#ifndef PPC_CONTEXT_H_INCLUDED
#define PPC_CONTEXT_H_INCLUDED

#ifndef PPC_CONFIG_H_INCLUDED
#error "ppc_config.h must be included before ppc_context.h"
#endif

// Native build (crackdown-recomp): the recompiled code runs as ordinary functions on the host,
// with no emulator. This header is the kit's ppc_context.h with the XEO3-specific parts (its
// preserve_none ABI, next-PC-in-ECX returns, PrecompiledPointers callbacks) replaced by calls into
// our own runtime (runtime/*.cpp). The generated code itself is unchanged.

#define __declspec(x)
#define __fastcall

#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <x86intrin.h>
#include <xmmintrin.h>
#include <smmintrin.h>
#include <emmintrin.h>

extern "C" unsigned long long zz_mftb(void);

#define PPC_JOIN(x, y) x##y
#define PPC_XSTRINGIFY(x) #x
#define PPC_STRINGIFY(x) PPC_XSTRINGIFY(x)
#define PPC_FUNC(x) void x(uintptr_t ctx, uintptr_t base, uintptr_t frame)
#define PPC_FUNC_IMPL(x) extern "C" PPC_FUNC(x)
#define PPC_EXTERN_FUNC(x) extern PPC_FUNC(x)
#define PPC_WEAK_FUNC(x) __attribute__((weak,noinline)) void x(uintptr_t ctx, uintptr_t base, uintptr_t frame)
// ctx points at r3 (+0x80) as in the XEO3 ABI, so the generated ADJ(ctx) works unchanged.
#define ADJ(x) ((PPCContext*)(ctx - 0x80))

// A guest blr is a plain host return: guest calls are host calls.
#define PPC_RETURN_PC(v) return
#ifdef RT_CHECK_NONVOLATILE
// Debug: every game function must return with r14..r31 as it found them.
#define PPC_RETURN() do { if ( __func__[7] == 's' ) rt_check_nv( __func__, rt_nv_in, ADJ(ctx) ); return; } while (0)
#else
#define PPC_RETURN() return
#endif

// Trap instructions and the kernel-only import forms (the game uses none of the latter).
extern "C" void rt_trap(unsigned int addr, void* ppcCtx);
extern "C" void rt_unmapped_import(unsigned int addr, unsigned int ordinal);
extern "C" void rt_local_import(unsigned int addr, unsigned int ordinal, void* ppcCtx);
// Conditional traps are the game's asserts and divide-by-zero guards: log and carry on in place.
// (Returning here would skip the function epilogue and lose the caller's saved registers.)
#define PPC_TRAP(addr) rt_trap((uint32_t)(addr), ADJ(ctx))
#define PPC_IMPORT(addr, ordinal) do { rt_unmapped_import((uint32_t)(addr), (uint32_t)(ordinal)); return; } while (0)
#define PPC_IMPORT_UNMAPPED(addr, ordinal) rt_unmapped_import((uint32_t)(addr), (uint32_t)(ordinal))
#define PPC_IMPORT_LOCAL(addr, ordinal) rt_local_import((uint32_t)(addr), (uint32_t)(ordinal), ADJ(ctx))

// Hardware registers (GPU 0x7FC8xxxx, XMA 0x7FEAxxxx): our device models.
extern "C" uint32_t rt_mmio_read(uint32_t addr);
extern "C" void rt_mmio_write(uint32_t addr, uint32_t value);
#define PPC_MM_LOAD_U32(x) rt_mmio_read((uint32_t)(x))
#define PPC_MM_STORE_U32(x, y) rt_mmio_write((uint32_t)(x), (uint32_t)(y))

// Kernel/xam imports: every import thunk calls into the runtime by index and name.
extern "C" void rt_import(unsigned int index, const char* name, void* ppcCtx, uint8_t* base);
#define PPC_IMPORT_FUNC_IMPL(n,i) void n(uintptr_t ctx, uintptr_t base, uintptr_t frame) { \
    rt_import((i), #n, ADJ(ctx), (uint8_t*)base); \
}

// Call trace: every guest function entry lands in a per-thread ring (diagnostics for stalls).
struct RtTraceEntry { const char *func; uint32_t r23; };
extern thread_local RtTraceEntry rt_trace_ring[128];
extern thread_local unsigned rt_trace_pos;
#ifdef RT_CHECK_NONVOLATILE
struct RtNv { uint64_t r[18]; };
void rt_nv_save( RtNv &n, struct PPCContext *c );
void rt_check_nv( const char *func, const RtNv &n, struct PPCContext *c );
#define RT_NV_SAVE RtNv rt_nv_in; rt_nv_save( rt_nv_in, (PPCContext*)(ctx - 0x80) );
#else
#define RT_NV_SAVE
#endif
// Function profile (CRACKDOWN_PROFILE): while rt_profile_on, every entry is counted per function.
extern volatile bool rt_profile_on;
void rt_profile_hit( const char *func );
#define PPC_FUNC_PROLOGUE() RT_NV_SAVE do { __builtin_assume(((size_t)base & 0x1F) == 0); if ( __func__[7] == 's' ) rt_trace_ring[rt_trace_pos++ & 127] = { __func__, ((PPCContext*)(ctx - 0x80))->r23.u32 }; \
    if ( rt_profile_on ) rt_profile_hit( __func__ ); } while (0)

#ifndef PPC_LOAD_U8
#define PPC_LOAD_U8(x) *(volatile uint8_t*)(base + (x))
#endif

#ifndef PPC_LOAD_U16
#define PPC_LOAD_U16(x) __builtin_bswap16(*(volatile uint16_t*)(base + (x)))
#endif

#ifndef PPC_LOAD_U32
#define PPC_LOAD_U32(x) __builtin_bswap32(*(volatile uint32_t*)(base + (x)))
#endif

#ifndef PPC_LOAD_U64
#define PPC_LOAD_U64(x) __builtin_bswap64(*(volatile uint64_t*)(base + (x)))
#endif

// TODO: Implement.
// These are currently unused. However, MMIO loads could possibly be handled statically with some profiling and a fallback.
// The fallback would be a runtime exception handler which will intercept reads from MMIO regions 
// and log the PC for compiling to static code later.
#ifndef PPC_MM_LOAD_U8
#define PPC_MM_LOAD_U8(x)  PPC_LOAD_U8 (x)
#endif

#ifndef PPC_MM_LOAD_U16
#define PPC_MM_LOAD_U16(x) PPC_LOAD_U16(x)
#endif


#ifndef PPC_MM_LOAD_U64
#define PPC_MM_LOAD_U64(x) PPC_LOAD_U64(x)
#endif

#ifndef PPC_STORE_U8
#define PPC_STORE_U8(x, y) *(volatile uint8_t*)(base + (x)) = (y)
#endif

#ifndef PPC_STORE_U16
#define PPC_STORE_U16(x, y) *(volatile uint16_t*)(base + (x)) = __builtin_bswap16(y)
#endif

#ifndef PPC_STORE_U32
#define PPC_STORE_U32(x, y) *(volatile uint32_t*)(base + (x)) = __builtin_bswap32(y)
#endif

#ifndef PPC_STORE_U64
#define PPC_STORE_U64(x, y) *(volatile uint64_t*)(base + (x)) = __builtin_bswap64(y)
#endif

// MMIO Store handling is completely reliant on being preeceded by eieio.
// TODO: Verify if that's always the case.
#ifndef PPC_MM_STORE_U8
#define PPC_MM_STORE_U8(x, y)   PPC_STORE_U8 (x, y)
#endif

#ifndef PPC_MM_STORE_U16
#define PPC_MM_STORE_U16(x, y)  PPC_STORE_U16(x, y)
#endif


#ifndef PPC_MM_STORE_U64
#define PPC_MM_STORE_U64(x, y)  PPC_STORE_U64(x, y)
#endif

#ifndef PPC_CALL_FUNC
#define PPC_CALL_FUNC(x) x(ctx, base)
#endif

#define PPC_MEMORY_SIZE 0x100000000ull

struct PPCContext;
typedef void PPCFunc(uintptr_t ctx, uintptr_t base, uintptr_t frame);
extern "C" PPCFunc* rt_lookup(uint32_t guest_addr);
#define PPC_LOOKUP_FUNC(x, y) rt_lookup((uint32_t)(y))

#ifndef PPC_CALL_INDIRECT_FUNC
#define PPC_CALL_INDIRECT_FUNC(x) (PPC_LOOKUP_FUNC(base, x))(ctx, base, frame)
#endif

// s4: the `default:` arm of a recompiled jump table.
//
// It used to be `__builtin_unreachable()`. That is a promise to the optimiser
// that the index is always in range, and clang cashes it in by DELETING the
// bounds check -- at sub_822D8180 it emitted a bare `movsxd rax,[rcx+rax*4]`
// into its own jump table, so an index one past the end (the s3 crash: the guest
// table had five entries and switch.toml recorded four) read from hyperspace and
// died as an anonymous c0000005 at a host RVA.
//
// The arm is now live, which restores the bounds check, and it falls back to
// PPC_CALL_INDIRECT_FUNC(ctr). That is architecturally right: the recompiler
// translates the whole lis/addi/rlwinm/lwzx/mtctr chain faithfully and special
// cases only the `bctr`, so on a default hit ctr already holds the address real
// hardware would branch to. If Emu knows that address the guest carries on
// correctly; if it does not, Emu fails by NAME ("Unable to execute precompiled
// code for IAR 0x...") with a guest address in the log instead of a host RVA.
//
// The report hook defaults to nothing, so titles built against this header need
// no new symbol -- the indirect fallback alone is their safety net. A build that
// wants the log defines PPC_SWITCH_MISS ahead of this header and links its own
// zz_switch_miss (Fable II: runtime\zz_switch.cpp).
#ifndef PPC_SWITCH_MISS
#define PPC_SWITCH_MISS(addr, index, target) ((void)0)
#endif


struct PPCFuncMapping
{
    uint32_t guest_rva;
    uint32_t host_rva;
};

struct PPCImportMapping
{
    uint32_t imp_addr;
    uint32_t dest_addr;
};

extern "C" PPCFuncMapping PPCFuncMappings[];
__declspec(dllexport) extern "C" PPCImportMapping PrecompiledImportTable[];
__declspec(dllexport) extern "C" uintptr_t PrecompiledPointers[];

union PPCRegister
{
    int8_t s8;
    uint8_t u8;
    int16_t s16;
    uint16_t u16;
    int32_t s32;
    uint32_t u32;
    int64_t s64;
    uint64_t u64;
    float f32;
    double f64;
};

struct PPCXERRegister
{
    uint8_t so;
    uint8_t ov;
    uint8_t ca;
};

struct PPCCRRegister
{
    uint8_t lt;
    uint8_t gt;
    uint8_t eq;
    union
    {
        uint8_t so;
        uint8_t un;
    };

    template<typename T>
    inline void compare(T left, T right, const PPCXERRegister& xer) noexcept
    {
        lt = left < right;
        gt = left > right;
        eq = left == right;
        so = xer.so;
    }

    inline void compare(double left, double right) noexcept
    {
        un = __builtin_isnan(left) || __builtin_isnan(right);
        lt = !un && (left < right);
        gt = !un && (left > right);
        eq = !un && (left == right);
    }

    inline void setFromMask(__m128 mask, int imm) noexcept
    {
        int m = _mm_movemask_ps(mask);
        lt = m == imm; // all equal
        gt = 0;
        eq = m == 0; // none equal
        so = 0;
    }

    inline void setFromMask(__m128i mask, int imm) noexcept
    {
        int m = _mm_movemask_epi8(mask);
        lt = m == imm; // all equal
        gt = 0;
        eq = m == 0; // none equal
        so = 0;
    }
};

union alignas(0x10) PPCVRegister
{
    int8_t s8[16];
    uint8_t u8[16];
    int16_t s16[8];
    uint16_t u16[8];
    int32_t s32[4];
    uint32_t u32[4];
    int64_t s64[2];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
};

#define PPC_ROUND_NEAREST 0x00
#define PPC_ROUND_TOWARD_ZERO 0x01
#define PPC_ROUND_UP 0x02
#define PPC_ROUND_DOWN 0x03
#define PPC_ROUND_MASK 0x03

struct PPCFPSCRRegister
{
    uint32_t csr;

    static constexpr size_t GuestToHost[] = { _MM_ROUND_NEAREST, _MM_ROUND_TOWARD_ZERO, _MM_ROUND_UP, _MM_ROUND_DOWN };
    static constexpr size_t HostToGuest[] = { PPC_ROUND_NEAREST, PPC_ROUND_DOWN, PPC_ROUND_UP, PPC_ROUND_TOWARD_ZERO };

    inline uint32_t loadFromHost() noexcept
    {
        csr = _mm_getcsr();
        return HostToGuest[(csr & _MM_ROUND_MASK) >> 13];
    }
        
    inline void storeFromGuest(uint32_t value) noexcept
    {
        csr &= ~_MM_ROUND_MASK;
        csr |= GuestToHost[value & PPC_ROUND_MASK];
        _mm_setcsr(csr);
    }

    static constexpr size_t FlushMask = _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
    // Threads that keep denormals flushed for scalar floats too (the audio driver thread, system.cpp: the
    // game's filters decay into denormals, which x86 handles a hundred times slower; inaudible anyway).
    static inline thread_local bool keep_flush = false;

    inline void enableFlushModeUnconditional() noexcept
    {
        csr |= FlushMask;
        csr |= _MM_MASK_MASK;
        _mm_setcsr(csr);
    }

    inline void disableFlushModeUnconditional() noexcept
    {
        if ( keep_flush ) [[unlikely]] return;
        csr &= ~FlushMask;
        _mm_setcsr(csr);
    }

    inline void enableFlushMode() noexcept
    {
        if ((csr & FlushMask) != FlushMask) [[unlikely]]
        {
            csr |= FlushMask;
            csr |= _MM_MASK_MASK;
            _mm_setcsr(csr);
        }
    }

    inline void disableFlushMode() noexcept
    {
        if ((csr & FlushMask) != 0 && !keep_flush) [[unlikely]]
        {
            csr &= ~FlushMask;
            _mm_setcsr(csr);
        }
    }
};

struct alignas(0x10) PPCContext
{
  PPCVRegister v63;
  PPCVRegister v127;
  PPCRegister f0;
  PPCRegister f1;
  PPCRegister f13;
  PPCRegister f31;
  PPCCRRegister cr0;
  PPCCRRegister cr1;
  PPCCRRegister cr2;
  PPCCRRegister cr3;
  PPCCRRegister cr4;
  PPCCRRegister cr5;
  PPCCRRegister cr6;
  PPCCRRegister cr7;
  uint64_t lr;
  PPCRegister ctr;
  uintptr_t image_mem_base;
  PPCRegister iar;
  PPCRegister r3;
  PPCRegister r1;
  uint32_t host_mxcsr;
  PPCXERRegister xer;
  PPCRegister r4;
  PPCRegister r5;
  PPCRegister r6;
  PPCRegister r7;
  PPCRegister r8;
  PPCRegister r9;
  PPCRegister r10;
  PPCRegister r11;
  PPCRegister r12;
  PPCRegister r28;
  PPCRegister r29;
  PPCRegister r30;
  PPCRegister r31;
  PPCRegister r0;
  PPCRegister r2;
  PPCRegister r13;
  PPCRegister r14;
  PPCRegister r15;
  PPCRegister r16;
  PPCRegister r17;
  PPCRegister r18;
  PPCRegister r19;
  PPCRegister r20;
  PPCRegister r21;
  PPCRegister r22;
  PPCRegister r23;
  PPCRegister r24;
  PPCRegister r25;
  PPCRegister r26;
  PPCRegister r27;
  PPCRegister f2;
  PPCRegister f3;
  PPCRegister f4;
  PPCRegister f5;
  PPCRegister f6;
  PPCRegister f7;
  PPCRegister f8;
  PPCRegister f9;
  PPCRegister f10;
  PPCRegister f11;
  PPCRegister f12;
  PPCRegister f14;
  PPCRegister f15;
  PPCRegister f16;
  PPCRegister f17;
  PPCRegister f18;
  PPCRegister f19;
  PPCRegister f20;
  PPCRegister f21;
  PPCRegister f22;
  PPCRegister f23;
  PPCRegister f24;
  PPCRegister f25;
  PPCRegister f26;
  PPCRegister f27;
  PPCRegister f28;
  PPCRegister f29;
  PPCRegister f30;
  PPCFPSCRRegister fpscr;
  uint64_t msr;
  PPCRegister reserved;
  PPCVRegister v0;
  PPCVRegister v1;
  PPCVRegister v2;
  PPCVRegister v3;
  PPCVRegister v4;
  PPCVRegister v5;
  PPCVRegister v6;
  PPCVRegister v7;
  PPCVRegister v8;
  PPCVRegister v9;
  PPCVRegister v10;
  PPCVRegister v11;
  PPCVRegister v12;
  PPCVRegister v13;
  PPCVRegister v14;
  PPCVRegister v15;
  PPCVRegister v16;
  PPCVRegister v17;
  PPCVRegister v18;
  PPCVRegister v19;
  PPCVRegister v20;
  PPCVRegister v21;
  PPCVRegister v22;
  PPCVRegister v23;
  PPCVRegister v24;
  PPCVRegister v25;
  PPCVRegister v26;
  PPCVRegister v27;
  PPCVRegister v28;
  PPCVRegister v29;
  PPCVRegister v30;
  PPCVRegister v31;
  PPCVRegister v32;
  PPCVRegister v33;
  PPCVRegister v34;
  PPCVRegister v35;
  PPCVRegister v36;
  PPCVRegister v37;
  PPCVRegister v38;
  PPCVRegister v39;
  PPCVRegister v40;
  PPCVRegister v41;
  PPCVRegister v42;
  PPCVRegister v43;
  PPCVRegister v44;
  PPCVRegister v45;
  PPCVRegister v46;
  PPCVRegister v47;
  PPCVRegister v48;
  PPCVRegister v49;
  PPCVRegister v50;
  PPCVRegister v51;
  PPCVRegister v52;
  PPCVRegister v53;
  PPCVRegister v54;
  PPCVRegister v55;
  PPCVRegister v56;
  PPCVRegister v57;
  PPCVRegister v58;
  PPCVRegister v59;
  PPCVRegister v60;
  PPCVRegister v61;
  PPCVRegister v62;
  PPCVRegister v64;
  PPCVRegister v65;
  PPCVRegister v66;
  PPCVRegister v67;
  PPCVRegister v68;
  PPCVRegister v69;
  PPCVRegister v70;
  PPCVRegister v71;
  PPCVRegister v72;
  PPCVRegister v73;
  PPCVRegister v74;
  PPCVRegister v75;
  PPCVRegister v76;
  PPCVRegister v77;
  PPCVRegister v78;
  PPCVRegister v79;
  PPCVRegister v80;
  PPCVRegister v81;
  PPCVRegister v82;
  PPCVRegister v83;
  PPCVRegister v84;
  PPCVRegister v85;
  PPCVRegister v86;
  PPCVRegister v87;
  PPCVRegister v88;
  PPCVRegister v89;
  PPCVRegister v90;
  PPCVRegister v91;
  PPCVRegister v92;
  PPCVRegister v93;
  PPCVRegister v94;
  PPCVRegister v95;
  PPCVRegister v96;
  PPCVRegister v97;
  PPCVRegister v98;
  PPCVRegister v99;
  PPCVRegister v100;
  PPCVRegister v101;
  PPCVRegister v102;
  PPCVRegister v103;
  PPCVRegister v104;
  PPCVRegister v105;
  PPCVRegister v106;
  PPCVRegister v107;
  PPCVRegister v108;
  PPCVRegister v109;
  PPCVRegister v110;
  PPCVRegister v111;
  PPCVRegister v112;
  PPCVRegister v113;
  PPCVRegister v114;
  PPCVRegister v115;
  PPCVRegister v116;
  PPCVRegister v117;
  PPCVRegister v118;
  PPCVRegister v119;
  PPCVRegister v120;
  PPCVRegister v121;
  PPCVRegister v122;
  PPCVRegister v123;
  PPCVRegister v124;
  PPCVRegister v125;
  PPCVRegister v126;
};

// PPCFunc: defined above (native calling convention).

// ---------------------------------------------------------------------------
// s35: guest setjmp / longjmp.
//
// The recompiler turns the CRT's setjmp and longjmp into ordinary generated
// functions unless config.toml names them (setjmp_address / longjmp_address).
// As ordinary functions they are wrong in a way that is silent until the first
// real error: guest longjmp restores the guest registers, `mtlr` the saved LR
// and `blr` -- but a `blr` here is PPC_RETURN, i.e. a HOST return to longjmp's
// caller, not a jump to the setjmp site. The host stack is never unwound. Lua's
// luaD_throw is the textbook victim: `bl longjmp` and then, on the very next
// instruction, the panic()/exit(1) path that hardware never reaches. That is
// exactly how the dashboard died on an empty UI string in s33 (a Lua error the
// scripts catch with pcall on a console).
//
// So, when config names them, the recompiler emits PPC_SETJMP() in place of the
// `bl setjmp` (it MUST be inline in the caller: that frame is the one that has
// to survive) and PPC_LONGJMP() in place of `bl longjmp`.
//
//   * The HOST jmp_buf is written into the GUEST jmp_buf's memory (base + r3):
//     the guest buffer is 1344+ bytes (f14-31, r13-31, v64-127, LR, CR, r1),
//     the x64 one is 256, so it fits, and nothing else reads that memory once
//     the guest setjmp body is no longer run.
//   * The guest register file is snapshotted at setjmp and the non-volatile
//     part (r1, r13-r31, f14-f31, v64-v127, LR, CR -- precisely what the CRT
//     longjmp restores) is put back when a longjmp lands, because the frames
//     in between have advanced ctx->r1 and clobbered ctx->r13.. in memory.
//     Emulator-owned fields (image_mem_base, iar, msr, host_mxcsr, reserved)
//     are deliberately NOT restored.
//   * _JUMP_BUFFER::Frame is zeroed after a successful setjmp so the MSVC
//     longjmp does a plain register restore instead of an SEH unwind through
//     preserve_none frames (nothing in generated code has a destructor).
//   * `setjmp` is returns_twice: clang keeps ctx/base/the snapshot in memory
//     across it, which is what makes resuming in this frame sound.
// ---------------------------------------------------------------------------
inline void PPCLongjmpRestore(PPCContext* c, const PPCContext* e)
{
    c->r1 = e->r1;
    c->r13 = e->r13; c->r14 = e->r14; c->r15 = e->r15; c->r16 = e->r16;
    c->r17 = e->r17; c->r18 = e->r18; c->r19 = e->r19; c->r20 = e->r20;
    c->r21 = e->r21; c->r22 = e->r22; c->r23 = e->r23; c->r24 = e->r24;
    c->r25 = e->r25; c->r26 = e->r26; c->r27 = e->r27; c->r28 = e->r28;
    c->r29 = e->r29; c->r30 = e->r30; c->r31 = e->r31;
    c->f14 = e->f14; c->f15 = e->f15; c->f16 = e->f16; c->f17 = e->f17;
    c->f18 = e->f18; c->f19 = e->f19; c->f20 = e->f20; c->f21 = e->f21;
    c->f22 = e->f22; c->f23 = e->f23; c->f24 = e->f24; c->f25 = e->f25;
    c->f26 = e->f26; c->f27 = e->f27; c->f28 = e->f28; c->f29 = e->f29;
    c->f30 = e->f30; c->f31 = e->f31;
    c->v64 = e->v64; c->v65 = e->v65; c->v66 = e->v66; c->v67 = e->v67;
    c->v68 = e->v68; c->v69 = e->v69; c->v70 = e->v70; c->v71 = e->v71;
    c->v72 = e->v72; c->v73 = e->v73; c->v74 = e->v74; c->v75 = e->v75;
    c->v76 = e->v76; c->v77 = e->v77; c->v78 = e->v78; c->v79 = e->v79;
    c->v80 = e->v80; c->v81 = e->v81; c->v82 = e->v82; c->v83 = e->v83;
    c->v84 = e->v84; c->v85 = e->v85; c->v86 = e->v86; c->v87 = e->v87;
    c->v88 = e->v88; c->v89 = e->v89; c->v90 = e->v90; c->v91 = e->v91;
    c->v92 = e->v92; c->v93 = e->v93; c->v94 = e->v94; c->v95 = e->v95;
    c->v96 = e->v96; c->v97 = e->v97; c->v98 = e->v98; c->v99 = e->v99;
    c->v100 = e->v100; c->v101 = e->v101; c->v102 = e->v102; c->v103 = e->v103;
    c->v104 = e->v104; c->v105 = e->v105; c->v106 = e->v106; c->v107 = e->v107;
    c->v108 = e->v108; c->v109 = e->v109; c->v110 = e->v110; c->v111 = e->v111;
    c->v112 = e->v112; c->v113 = e->v113; c->v114 = e->v114; c->v115 = e->v115;
    c->v116 = e->v116; c->v117 = e->v117; c->v118 = e->v118; c->v119 = e->v119;
    c->v120 = e->v120; c->v121 = e->v121; c->v122 = e->v122; c->v123 = e->v123;
    c->v124 = e->v124; c->v125 = e->v125; c->v126 = e->v126; c->v127 = e->v127;
    c->lr = e->lr;
    c->cr0 = e->cr0; c->cr1 = e->cr1; c->cr2 = e->cr2; c->cr3 = e->cr3;
    c->cr4 = e->cr4; c->cr5 = e->cr5; c->cr6 = e->cr6; c->cr7 = e->cr7;
}

// Optional hook for a build's diagnostics: called with the value being delivered
// (0 = first return, >0 = a longjmp landed here, -1 = a longjmp is being thrown).
// Every build that uses PPC_SETJMP must define it (an empty body is fine).
extern "C" void zz_setjmp_note(uintptr_t ctx, uintptr_t guest_jmpbuf, int value);

#ifdef _WIN32
#define PPC_JMPBUF_NO_UNWIND(jb) (reinterpret_cast<_JUMP_BUFFER*>(jb)->Frame = 0)
#else
#define PPC_JMPBUF_NO_UNWIND(jb) ((void)0)
#endif

#define PPC_SETJMP() do { \
    PPCContext zz_jmp_env; \
    memcpy(&zz_jmp_env, ADJ(ctx), sizeof(PPCContext)); \
    uint32_t zz_jmp_guest = ADJ(ctx)->r3.u32; \
    jmp_buf* zz_jb = reinterpret_cast<jmp_buf*>(base + zz_jmp_guest); \
    int zz_jr = setjmp(*zz_jb); \
    if (zz_jr == 0) PPC_JMPBUF_NO_UNWIND(zz_jb); \
    else PPCLongjmpRestore(ADJ(ctx), &zz_jmp_env); \
    ADJ(ctx)->r3.s64 = zz_jr; \
    zz_setjmp_note(ctx, zz_jmp_guest, zz_jr); \
} while (0)

// The CRT's longjmp delivers 1 for a value of 0.
#define PPC_LONGJMP() do { \
    zz_setjmp_note(ctx, ADJ(ctx)->r3.u32, -1); \
    longjmp(*reinterpret_cast<jmp_buf*>(base + ADJ(ctx)->r3.u32), ADJ(ctx)->r4.s32 ? ADJ(ctx)->r4.s32 : 1); \
} while (0)

extern PPCFunc* HostFuncs[];
extern int HostFuncsCount;

inline uint8_t VectorMaskL[] =
{
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};

inline uint8_t VectorMaskR[] =
{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

inline uint8_t VectorShiftTableL[] =
{
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
};

inline uint8_t VectorShiftTableR[] =
{
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

inline __m128i _mm_adds_epu32(__m128i a, __m128i b) 
{
    return _mm_add_epi32(a, _mm_min_epu32(_mm_xor_si128(a, _mm_cmpeq_epi32(a, a)), b));
}

inline __m128i _mm_avg_epi8(__m128i a, __m128i b)
{
    __m128i c = _mm_set1_epi8(char(128));
    return _mm_xor_si128(c, _mm_avg_epu8(_mm_xor_si128(c, a), _mm_xor_si128(c, b)));
}

inline __m128i _mm_avg_epi16(__m128i a, __m128i b)
{
    __m128i c = _mm_set1_epi16(short(32768));
    return _mm_xor_si128(c, _mm_avg_epu16(_mm_xor_si128(c, a), _mm_xor_si128(c, b)));
}

inline __m128 _mm_cvtepu32_ps_(__m128i src1)
{
    __m128i xmm1 = _mm_add_epi32(src1, _mm_set1_epi32(127));
    __m128i xmm0 = _mm_slli_epi32(src1, 31 - 8);
    xmm0 = _mm_srli_epi32(xmm0, 31);
    xmm0 = _mm_add_epi32(xmm0, xmm1);
    xmm0 = _mm_srai_epi32(xmm0, 8);
    xmm0 = _mm_add_epi32(xmm0, _mm_set1_epi32(0x4F800000));
    __m128 xmm2 = _mm_cvtepi32_ps(src1);
    return _mm_blendv_ps(xmm2, _mm_castsi128_ps(xmm0), _mm_castsi128_ps(src1));
}

inline __m128i _mm_perm_epi8_(__m128i a, __m128i b, __m128i c)
{
    __m128i d = _mm_set1_epi8(0xF);
    __m128i e = _mm_sub_epi8(d, _mm_and_si128(c, d));
    return _mm_blendv_epi8(_mm_shuffle_epi8(a, e), _mm_shuffle_epi8(b, e), _mm_slli_epi32(c, 3));
}

inline __m128i _mm_cmpgt_epu8(__m128i a, __m128i b)
{
    __m128i c = _mm_set1_epi8(char(128));
    return _mm_cmpgt_epi8(_mm_xor_si128(a, c), _mm_xor_si128(b, c));
}

inline __m128i _mm_cmpgt_epu16(__m128i a, __m128i b)
{
    __m128i c = _mm_set1_epi16(short(32768));
    return _mm_cmpgt_epi16(_mm_xor_si128(a, c), _mm_xor_si128(b, c));
}

inline __m128i _mm_vctsxs(__m128 src1)
{
    __m128 xmm2 = _mm_cmpunord_ps(src1, src1);
    __m128i xmm0 = _mm_cvttps_epi32(src1);
    __m128i xmm1 = _mm_cmpeq_epi32(xmm0, _mm_set1_epi32(INT_MIN));
    xmm1 = _mm_andnot_si128(_mm_castps_si128(src1), xmm1);
    __m128 dest = _mm_blendv_ps(_mm_castsi128_ps(xmm0), _mm_castsi128_ps(_mm_set1_epi32(INT_MAX)), _mm_castsi128_ps(xmm1));
    return _mm_andnot_si128(_mm_castps_si128(xmm2), _mm_castps_si128(dest));
}

inline __m128i _mm_vsr(__m128i a, __m128i b)
{
    b = _mm_srli_epi64(_mm_slli_epi64(b, 61), 61);
    return _mm_castps_si128(_mm_insert_ps(_mm_castsi128_ps(_mm_srl_epi64(a, b)), _mm_castsi128_ps(_mm_srl_epi64(_mm_srli_si128(a, 4), b)), 0x10));
}

inline __m128 _mm_vcmpbfp(__m128 a, __m128 b) 
{
    __m128 xmm0 = _mm_and_ps(_mm_cmpgt_ps(a, b), _mm_castsi128_ps(_mm_set1_epi32(0x80000000)));
    __m128 xmm1 = _mm_and_ps(_mm_cmplt_ps(a, _mm_sub_ps(_mm_setzero_ps(), b)), _mm_castsi128_ps(_mm_set1_epi32(0x40000000)));
    return _mm_or_ps(xmm0, xmm1);
}

inline uint64_t __mulhu(uint64_t a, uint64_t b) {
    // Get high/low 32-bit parts 
    uint32_t a_lo = (uint32_t)a;
    uint32_t a_hi = (uint32_t)(a >> 32);
    uint32_t b_lo = (uint32_t)b;
    uint32_t b_hi = (uint32_t)(b >> 32);

    // Compute partial products
    uint64_t lo_lo = (uint64_t)a_lo * b_lo;
    uint64_t hi_lo = (uint64_t)a_hi * b_lo;
    uint64_t lo_hi = (uint64_t)a_lo * b_hi;
    uint64_t hi_hi = (uint64_t)a_hi * b_hi;

    // Compute high 64 bits of result
    uint64_t cross = (lo_lo >> 32) + (uint32_t)hi_lo + (uint32_t)lo_hi;
    return hi_hi + (hi_lo >> 32) + (lo_hi >> 32) + (cross >> 32);
}

inline __m128i _mm_vctuxs(__m128 src1)
{
    // Clamp negative to 0
    __m128 clamped = _mm_max_ps(src1, _mm_setzero_ps());

    // For values in [2^31, 2^32), subtract 2^31, convert, add 2^31 back
    __m128i big_result = _mm_add_epi32(
        _mm_cvttps_epi32(
            _mm_sub_ps(clamped, _mm_castsi128_ps(_mm_set1_epi32(0x4F000000)))
        ), 
        _mm_set1_epi32(0x80000000)
    ); 
    
    // Select based on range
    __m128i result = _mm_blendv_epi8(
        _mm_cvttps_epi32(clamped), 
        big_result, 
        _mm_castps_si128(
            _mm_cmpge_ps(clamped, _mm_castsi128_ps(_mm_set1_epi32(0x4F000000)))
        )
    );
    
    // Saturate overflow and NaN to UINT_MAX
    __m128 saturate_mask = _mm_or_ps(
        _mm_cmpge_ps(
            clamped, _mm_castsi128_ps(_mm_set1_epi32(0x4F800000))
        ),
        _mm_cmpunord_ps(src1, src1)
    );
    return _mm_blendv_epi8(result, _mm_set1_epi32(-1), _mm_castps_si128(saturate_mask));
}

inline __m128i _mm_vsl(__m128i a, __m128i b)
{
    // Extract shift count from last byte of b (accounting for endianness)
    uint32_t shift = _mm_extract_epi8(b, 15) & 0x7;
    
    if (shift == 0) return a;
    
    // Shift left by bits
    __m128i shifted = _mm_or_si128(
        _mm_slli_epi64(a, shift),
        _mm_srli_epi64(_mm_slli_si128(a, 8), 64 - shift)
    );
    
    return shifted;
}

inline __m128i _mm_adds_epi32(__m128i a, __m128i b)
{
    __m128i int_min = _mm_set1_epi32(0x80000000);
    __m128i int_max = _mm_set1_epi32(0x7FFFFFFF);

    const __m128i res = _mm_add_epi32(a, b);
    const __m128i sign_and = _mm_and_si128(a, b);
    const __m128i sign_or = _mm_or_si128(a, b);

    const __m128i min_sat_mask = _mm_andnot_si128(res, sign_and);
    const __m128i max_sat_mask = _mm_andnot_si128(sign_or, res);
    const __m128 res_temp =
    _mm_blendv_ps(_mm_castsi128_ps(res), _mm_castsi128_ps(int_min), _mm_castsi128_ps(min_sat_mask));
    return _mm_castps_si128(_mm_blendv_ps(res_temp, _mm_castsi128_ps(int_max), _mm_castsi128_ps(max_sat_mask)));
}

#endif
