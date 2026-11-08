// Shared declarations of the crackdown-recomp runtime.
#pragma once
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <string>

struct PPCContext;

extern uint8_t *g_base;  // host address of guest address 0

// getenv( name ) looked up once per call site: debug switches tested on hot paths (every packet, every draw).
#define RT_ENV( name ) ( [] { static const char *v = getenv( name ); return v; }() )

void rt_log( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
[[noreturn]] void rt_fatal( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));

void rt_kernel_init();
// Points the XEX's data-import slots at runtime-owned kernel variables (variables.cpp).
void rt_bind_variable_imports( const uint8_t *xex );
// A fresh copy of the game's static TLS block for a new guest thread.
uint32_t rt_new_tls_block();

// Runs guest function `entry` on the calling host thread with a fresh guest context
// (own guest stack and processor block); r3 = `arg`. Returns when the guest function returns.
void rt_run_thread( uint32_t entry, uint32_t arg, const char *name );
// Same, with r3 = a1 and r4 = a2.
void rt_run_thread2( uint32_t entry, uint32_t a1, uint32_t a2, const char *name );

// A reusable guest execution context (stack, processor block, registers) for host threads that
// call into guest code repeatedly, e.g. interrupt callbacks. One call at a time per GuestThread.
struct GuestThread { PPCContext *ctx; uint32_t stack_top, pcr; };
// Thrown by ExTerminateThread; unwinds guest code back to the thread's start.
struct GuestThreadExit { uint32_t status; };
// `kthread`: the guest KTHREAD stand-in for r13+256 (0: a new one); `processor`: hardware thread
// 0-5 reported at r13+268 (-1: spread round-robin).
GuestThread *rt_guest_thread_create( const char *name, uint32_t kthread = 0, int processor = -1 );
// Moves a started guest thread to `processor`, found by its KTHREAD; false if no thread has it.
bool rt_kthread_set_processor( uint32_t kthread, int processor, int *previous );
void rt_guest_call( GuestThread *t, uint32_t entry, uint32_t a1, uint32_t a2 );

// Guest memory helpers (big-endian).
inline uint32_t guest_read32( uint32_t a ) { return __builtin_bswap32( *(uint32_t *)( g_base + a ) ); }
inline void guest_write32( uint32_t a, uint32_t v ) { *(uint32_t *)( g_base + a ) = __builtin_bswap32( v ); }
inline void guest_write8( uint32_t a, uint8_t v ) { g_base[a] = v; }

// Import handlers register themselves:  RT_IMPORT(KeQueryPerformanceFrequency) { ... c.r3.u64 = ...; }
// Guest return address of the import call this host thread is in (diagnostics).
extern thread_local uint32_t rt_import_lr, rt_import_sp;
// Guest call chain from the current import (saved LRs along the stack back chain).
void rt_guest_backtrace( char *out, size_t size );
// Logs this thread's last 128 guest function entries.
void rt_dump_trace( const char *why );
void rt_memory_used( uint32_t &virt, uint32_t &phys );  // imports.cpp: guest bytes allocated (virtual, physical)
void audio_submit_frame( const uint8_t *samples );  // XAudioSubmitRenderDriverFrame (audio.cpp)
// video.cpp: texel index in a Xenos 2D tiled surface; write 0x00BBGGRR pixels as a binary PPM.
uint32_t tiled_offset( uint32_t x, uint32_t y, uint32_t width, uint32_t log_bpp );
void write_ppm( const char *path, const std::vector<uint32_t> &px, uint32_t w, uint32_t h );

using ImportHandler = void (*)( PPCContext &c );
void rt_register_import( const char *name, ImportHandler h );
struct ImportRegistrar { ImportRegistrar( const char *n, ImportHandler h ) { rt_register_import( n, h ); } };
#define RT_IMPORT(name) \
    static void rt_imp_##name( PPCContext &c ); \
    static ImportRegistrar rt_reg_##name( #name, rt_imp_##name ); \
    static void rt_imp_##name( PPCContext &c )

// Argument i (0 = r3) and the return value of an import.
#define RT_ARG(i) ( (i) == 0 ? c.r3.u32 : (i) == 1 ? c.r4.u32 : (i) == 2 ? c.r5.u32 : (i) == 3 ? c.r6.u32 : \
                    (i) == 4 ? c.r7.u32 : (i) == 5 ? c.r8.u32 : (i) == 6 ? c.r9.u32 : c.r10.u32 )
#define RT_RETURN(v) do { c.r3.u64 = (uint64_t)(v); return; } while (0)

inline uint16_t guest_read16( uint32_t a ) { return __builtin_bswap16( *(uint16_t *)( g_base + a ) ); }
inline void guest_write16( uint32_t a, uint16_t v ) { *(uint16_t *)( g_base + a ) = __builtin_bswap16( v ); }
inline uint64_t guest_read64( uint32_t a ) { return __builtin_bswap64( *(uint64_t *)( g_base + a ) ); }
inline void guest_write64( uint32_t a, uint64_t v ) { *(uint64_t *)( g_base + a ) = __builtin_bswap64( v ); }

// Guest allocations (imports.cpp): virtual (NtAllocateVirtualMemory) and physical (GPU-visible).
uint32_t rt_alloc_virtual( uint32_t size, uint32_t align );
uint32_t rt_alloc_physical( uint32_t size, uint32_t align );

// Activity counters, printed every 5 s by the status thread (kernel.cpp).
#include <atomic>
struct RtStats { std::atomic<uint64_t> reads, read_bytes, swaps, wptr_writes, imports; };
extern RtStats g_stats;
void rt_count_import( const char *name );
// Content roots ("save:") mounted onto writable host folders (files.cpp).
void files_mount( const std::string &root, const std::string &host );
bool files_unmount( const std::string &root );
void rt_print_import_stats();
