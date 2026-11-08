// Kernel and xam imports, threads and the small hardware surface the game touches.
// Stage 1: every import is logged and returns 0; real implementations replace the stub one by
// one as the log asks for them.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string>

namespace
{

std::mutex g_log_lock;

// Guest memory layout for runtime-owned structures (well clear of the game image at 0x82000000).
constexpr uint32_t STACK_AREA = 0x70000000;   // guest thread stacks, 1 MB each
constexpr uint32_t STACK_SIZE = 0x00100000;
constexpr uint32_t PCR_AREA   = 0x7FF00000;   // per-thread processor control blocks, 4 KB each
uint32_t g_next_thread;


} // namespace

void rt_log( const char *fmt, ... )
{
    std::lock_guard<std::mutex> guard( g_log_lock );
    static const bool stamp = getenv( "CRACKDOWN_LOG_TIME" );  // wall clock prefix, to line up with screen recordings
    if ( stamp )
    {
        timespec ts;
        clock_gettime( CLOCK_REALTIME, &ts );
        tm t;
        localtime_r( &ts.tv_sec, &t );
        fprintf( stderr, "%02d:%02d:%02d.%03ld ", t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000 );
    }
    va_list ap;
    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );
}

void rt_fatal( const char *fmt, ... )
{
    {
        std::lock_guard<std::mutex> guard( g_log_lock );
        fprintf( stderr, "FATAL: " );
        va_list ap;
        va_start( ap, fmt );
        vfprintf( stderr, fmt, ap );
        va_end( ap );
    }
    abort();
}

RtStats g_stats;

void rt_profile_tick( double seconds );

void rt_kernel_init()
{
    if ( getenv( "CRACKDOWN_PROFILE" ) )
        std::thread( [] {
            auto start = std::chrono::steady_clock::now();
            for ( ;; )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
                rt_profile_tick( std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() );
            }
        } ).detach();
    rt_log( "kernel: stage 1 (imports are logged stubs)\n" );
    std::thread( [] {
        for ( int t = 5;; t += 5 )
        {
            std::this_thread::sleep_for( std::chrono::seconds( 5 ) );
            uint32_t virt, phys;
            rt_memory_used( virt, phys );
            rt_log( "status %3ds: reads %llu (%.1f MB), frames %llu, gpu submits %llu, imports %llu, memory %u + %u MB\n", t,
                    (unsigned long long)g_stats.reads, g_stats.read_bytes / 1048576.0, (unsigned long long)g_stats.swaps,
                    (unsigned long long)g_stats.wptr_writes, (unsigned long long)g_stats.imports,
                    virt >> 20, phys >> 20 );
            if ( getenv( "CRACKDOWN_IMPORT_STATS" ) ) rt_print_import_stats();
        }
    } ).detach();
}

void rt_run_thread( uint32_t entry, uint32_t arg, const char *name ) { rt_run_thread2( entry, arg, 0, name ); }

namespace
{
std::mutex g_kthread_lock;
std::unordered_map<uint32_t, uint32_t> g_kthread_pcr;  // KTHREAD stand-in -> processor block
} // namespace

bool rt_kthread_set_processor( uint32_t kthread, int processor, int *previous )
{
    std::lock_guard<std::mutex> g( g_kthread_lock );
    auto it = g_kthread_pcr.find( kthread );
    if ( it == g_kthread_pcr.end() ) return false;
    if ( previous ) *previous = g_base[it->second + 0x10C];
    guest_write8( it->second + 0x10C, (uint8_t)processor );
    return true;
}

GuestThread *rt_guest_thread_create( const char *name, uint32_t kthread, int processor )
{
    uint32_t index = __atomic_fetch_add( &g_next_thread, 1, __ATOMIC_SEQ_CST );
    auto *t = new GuestThread;
    t->stack_top = STACK_AREA + ( index + 1 ) * STACK_SIZE - 0x100;
    t->pcr = PCR_AREA + index * 0x1000;
    memset( g_base + t->pcr, 0, 0x1000 );
    guest_write32( t->pcr + 0x000, rt_new_tls_block() );    // static TLS block (r13+0)
    // Processor number (r13+268): the hardware thread the game asked for, if it did. Code that
    // splits work across hardware threads (the audio mixer's barrier) keys per-thread state on it.
    guest_write8( t->pcr + 0x10C, (uint8_t)( processor >= 0 ? processor % 6 : index % 6 ) );
    // Current thread (r13+256): a KTHREAD stand-in. Code compares it with 0 to mean "no thread"
    // (the audio engine's worker quits when the render callback's owner reads as 0).
    if ( !kthread )
    {
        kthread = rt_alloc_virtual( 0x200, 0x10 );
        memset( g_base + kthread, 0, 0x200 );
        g_base[kthread] = 6;  // dispatcher type: thread
    }
    // Its thread id (KTHREAD+0x14C), which code reads through r13+256 as the current thread's id:
    // D3D lets only its owner thread write commands (others record work for it), so with every
    // thread reading id 0 any thread wrote into the render thread's command buffer at the same time.
    // Game threads have the id ExCreateThread returned; the runtime's own get ids of their own.
    if ( !guest_read32( kthread + 0x14C ) ) guest_write32( kthread + 0x14C, 0x10000 + index );
    guest_write32( t->pcr + 0x100, kthread );
    {
        std::lock_guard<std::mutex> g( g_kthread_lock );
        g_kthread_pcr[kthread] = t->pcr;
    }
    t->ctx = (PPCContext *)aligned_alloc( 0x40, ( sizeof(PPCContext) + 0x3F ) & ~0x3F );
    rt_log( "thread %u '%s': stack %08X, pcr %08X, processor %u\n", index, name, t->stack_top, t->pcr, g_base[t->pcr + 0x10C] );
    return t;
}

void rt_guest_call( GuestThread *t, uint32_t entry, uint32_t a1, uint32_t a2 )
{
    PPCContext *ctx = t->ctx;
    memset( ctx, 0, sizeof(*ctx) );
    ctx->r1.u64 = t->stack_top;
    ctx->r13.u64 = t->pcr;
    ctx->r3.u64 = a1;
    ctx->r4.u64 = a2;
    ctx->image_mem_base = (uintptr_t)g_base;
    PPCFunc *f = rt_lookup( entry );
    f( (uintptr_t)ctx + 0x80, (uintptr_t)g_base, 0 );
}

void rt_run_thread2( uint32_t entry, uint32_t arg, uint32_t arg2, const char *name )
{
    GuestThread *t = rt_guest_thread_create( name );
    rt_log( "  '%s' entry %08X\n", name, entry );
    try { rt_guest_call( t, entry, arg, arg2 ); }
    catch ( const GuestThreadExit & ) {}
}

extern "C" void rt_trap( unsigned int addr, void *pctx )
{
    auto *c = (PPCContext *)pctx;
    rt_log( "trap at %08X (ignored) r3=%08X r10=%08X r11=%08X r23=%08X r31=%08X\n", addr, c->r3.u32, c->r10.u32, c->r11.u32, c->r23.u32, c->r31.u32 );
    if ( addr == 0x8272E9CC ) rt_dump_trace( "trap" );
}

extern "C" void rt_unmapped_import( unsigned int addr, unsigned int ordinal )
{
    rt_fatal( "kernel-only import %04X at %08X in game code\n", ordinal, addr );
}

extern "C" void rt_local_import( unsigned int addr, unsigned int ordinal, void * )
{
    rt_fatal( "kernel-only local import %04X at %08X in game code\n", ordinal, addr );
}

extern "C" unsigned long long zz_mftb( void );

// GPU registers the CPU polls. Display status reports "in vertical blank" so waits finish.
bool xma_mmio_read( uint32_t addr, uint32_t &value );
bool xma_mmio_write( uint32_t addr, uint32_t value );

extern "C" uint32_t rt_mmio_read( uint32_t addr )
{
    if ( uint32_t v; xma_mmio_read( addr, v ) ) return v;
    switch ( addr )
    {
    case 0x7FC86544: return 1;  // display interrupt / vblank status (register 0x1951)
    case 0x7FC86530:            // display vertical line counter (register 0x194C)
        return (uint32_t)( zz_mftb() / 1157 % 750 );  // 750 lines per 60 Hz frame
    }
    static std::mutex m;
    static std::unordered_map<uint32_t, int> seen;
    std::lock_guard<std::mutex> g( m );
    if ( !seen[addr]++ ) rt_log( "mmio read %08X -> 0 (first)\n", addr );
    return 0;
}

void gpu_write_wptr( uint32_t wptr );
void gpu_mmio_write( uint32_t reg, uint32_t value );

extern "C" void rt_mmio_write( uint32_t addr, uint32_t value )
{
    if ( addr == 0x7FC80714 ) { gpu_write_wptr( value ); return; }  // CP_RB_WPTR
    if ( addr >= 0x7FC80000 && addr < 0x7FCA0000 ) { gpu_mmio_write( ( addr - 0x7FC80000 ) / 4, value ); return; }
    if ( xma_mmio_write( addr, value ) ) return;
    rt_log( "mmio write %08X = %08X\n", addr, value );
}

extern "C" unsigned long long zz_mftb( void );

// The 360 timebase runs at 50 MHz.
extern "C" unsigned long long zz_mftb( void )
{
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (unsigned long long)ts.tv_sec * 50000000ull + (unsigned long long)ts.tv_nsec / 20;
}

extern "C" void zz_setjmp_note( uintptr_t, uintptr_t, int ) {}

void rt_guest_backtrace( char *out, size_t size )
{
    size_t n = snprintf( out, size, "%08X", rt_import_lr );
    uint32_t sp = rt_import_sp;
    for ( int i = 0; i < 10 && n < size; ++i )
    {
        if ( sp < 0x70000000 || sp >= 0x80000000 ) break;
        sp = guest_read32( sp );  // back chain
        if ( sp < 0x70000000 || sp >= 0x80000000 ) break;
        uint32_t lr = guest_read32( sp - 8 );
        if ( lr < PPC_CODE_BASE || lr >= PPC_CODE_BASE + PPC_CODE_SIZE ) continue;
        n += snprintf( out + n, size - n, " < %08X", lr );
    }
}

thread_local RtTraceEntry rt_trace_ring[128];
thread_local unsigned rt_trace_pos;

// CRACKDOWN_PROFILE="name:start-end,...": count guest function entries between those seconds after
// start-up and log each window's counts (functions called in some windows but not others show what an
// action runs, e.g. menu navigation against an idle menu).
volatile bool rt_profile_on;
namespace
{
struct ProfileSlot { std::atomic<const char *> func; std::atomic<uint32_t> count; };
ProfileSlot g_profile[1 << 16];
}
void rt_profile_hit( const char *func )
{
    for ( size_t h = ( (uintptr_t)func >> 3 ) * 0x9E3779B97F4A7C15ull >> 48, i = 0; i < 64; ++i, h = ( h + 1 ) & 0xFFFF )
    {
        const char *cur = g_profile[h].func.load( std::memory_order_relaxed );
        if ( !cur && g_profile[h].func.compare_exchange_strong( cur, func ) ) cur = func;
        if ( cur == func ) { g_profile[h].count.fetch_add( 1, std::memory_order_relaxed ); return; }
    }
}
void rt_profile_tick( double seconds )  // called by the status thread
{
    static const char *spec = getenv( "CRACKDOWN_PROFILE" );
    if ( !spec ) return;
    static std::string active;
    bool want = false;
    std::string name;
    for ( const char *c = spec; c && *c; )
    {
        char nm[32]; double a, b; int used = 0;
        if ( sscanf( c, "%31[^:]:%lf-%lf%n", nm, &a, &b, &used ) != 3 ) break;
        if ( seconds >= a && seconds < b ) { want = true; name = nm; }
        c = strchr( c + used, ',' ); if ( c ) ++c;
    }
    if ( want && !rt_profile_on ) { active = name; rt_profile_on = true; }
    if ( !want && rt_profile_on )
    {
        rt_profile_on = false;
        std::string out = "profile " + active + ":";
        for ( auto &slot : g_profile )
            if ( const char *f = slot.func.exchange( nullptr ) )
            {
                char b[64];
                snprintf( b, sizeof( b ), " %s=%u", strncmp( f, "__imp__sub_", 11 ) ? f : f + 11, slot.count.exchange( 0 ) );
                out += b;
            }
        rt_log( "%s\n", out.c_str() );
    }
}

void rt_dump_trace( const char *why )
{
    char buf[4096];
    size_t n = snprintf( buf, sizeof(buf), "trace (%s), oldest first:", why );
    for ( unsigned i = 0; i < 128 && n < sizeof(buf); ++i )
        if ( const char *f = rt_trace_ring[( rt_trace_pos + i ) & 127].func )
            n += snprintf( buf + n, sizeof(buf) - n, " %s[%X]", strncmp( f, "__imp__sub_", 11 ) ? f : f + 11, rt_trace_ring[( rt_trace_pos + i ) & 127].r23 );
    rt_log( "%s\n", buf );
}

#ifdef RT_CHECK_NONVOLATILE
static const PPCRegister &nv( PPCContext *c, int i )  // r14..r27 are contiguous, r28..r31 elsewhere
{
    return i < 14 ? ( &c->r14 )[i] : ( &c->r28 )[i - 14];
}
void rt_nv_save( RtNv &n, PPCContext *c )
{
    for ( int i = 0; i < 18; ++i ) n.r[i] = nv( c, i ).u64;
}
void rt_check_nv( const char *func, const RtNv &n, PPCContext *c )
{
    for ( int i = 0; i < 18; ++i )
        if ( nv( c, i ).u64 != n.r[i] )
        {
            static std::atomic<int> reports;
            if ( reports++ < 20 )
                rt_log( "nonvolatile: %s returned with r%d %llX -> %llX\n", func, 14 + i, (unsigned long long)n.r[i], (unsigned long long)nv( c, i ).u64 );
            return;
        }
}
#endif
