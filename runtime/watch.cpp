// Debug: which game functions read or write a range of guest memory. The range's page is protected;
// each access faults, is attributed to the recompiled function containing the faulting instruction
// (nearest host function at or below it), then the page is opened, the access single-stepped (x86 trap
// flag) and the page protected again. watch_arm(lo, hi, seconds) starts it; after that many seconds the
// counts are logged and the page left open.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <signal.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <sys/mman.h>
#include <ucontext.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

extern PPCFuncMapping PPCFuncMappings[];

void watch_arm( uint32_t lo, uint32_t hi, double seconds );
namespace
{
std::vector<std::pair<uintptr_t, uint32_t>> g_by_host;  // host entry -> guest address, sorted
uint32_t g_lo, g_hi, g_page;
uint32_t g_views[4];  // watch_arm_views: the same physical page in each view
int g_nviews;
std::map<std::tuple<uint32_t, uint32_t, bool>, uint32_t> g_view_hits;  // (guest function, offset, write) -> count
std::atomic<bool> g_on;
// CRACKDOWN_WATCH_WRITES=1: only writes fault (the page read-only), for memory read all the time.
const bool g_writes_only = getenv( "CRACKDOWN_WATCH_WRITES" ) != nullptr;
std::mutex g_lock;
std::map<std::pair<uint32_t, bool>, uint32_t> g_hits;  // (guest function, in range) -> count

uint32_t guest_function( uintptr_t rip )
{
    auto it = std::upper_bound( g_by_host.begin(), g_by_host.end(), std::make_pair( rip, ~0u ) );
    return it == g_by_host.begin() ? 0 : std::prev( it )->second;
}

void on_segv( int sig, siginfo_t *si, void *uc_ )
{
    auto *uc = (ucontext_t *)uc_;
    uint8_t *a = (uint8_t *)si->si_addr;
    uint32_t off = (uint32_t)( a - g_base );
    if ( g_nviews )
    {
        int v = 0;
        while ( v < g_nviews && ( off & ~0xFFFu ) != g_views[v] ) ++v;
        if ( !g_on || v == g_nviews ) { signal( SIGSEGV, SIG_DFL ); return; }
        bool write = uc->uc_mcontext.gregs[REG_ERR] & 2;
        {
            std::lock_guard<std::mutex> g( g_lock );
            ++g_view_hits[{ guest_function( uc->uc_mcontext.gregs[REG_RIP] ), off & 0xFFF, write }];
        }
        for ( int i = 0; i < g_nviews; ++i ) mprotect( g_base + g_views[i], 4096, PROT_READ | PROT_WRITE );
        uc->uc_mcontext.gregs[REG_EFL] |= 0x100;
        return;
    }
    if ( !g_on || ( off & ~0xFFFu ) != g_page )
    {
        signal( SIGSEGV, SIG_DFL );  // not ours: crash as usual
        return;
    }
    {
        std::lock_guard<std::mutex> g( g_lock );
        ++g_hits[{ guest_function( uc->uc_mcontext.gregs[REG_RIP] ), off >= g_lo && off < g_hi }];
    }
    mprotect( g_base + g_page, 4096, PROT_READ | PROT_WRITE );
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;  // trap after this instruction
    (void)sig;
}

void on_trap( int, siginfo_t *, void *uc_ )
{
    auto *uc = (ucontext_t *)uc_;
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    if ( g_on && g_nviews )
        for ( int i = 0; i < g_nviews; ++i ) mprotect( g_base + g_views[i], 4096, PROT_NONE );
    else if ( g_on ) mprotect( g_base + g_page, 4096, g_writes_only ? PROT_READ : PROT_NONE );
}
} // namespace

static void watch_init()
{
    if ( g_by_host.empty() )
    {
        for ( int i = 0; i < HostFuncsCount; ++i )
            g_by_host.push_back( { (uintptr_t)HostFuncs[i], (uint32_t)( PPC_IMAGE_BASE + PPCFuncMappings[i].guest_rva ) } );
        std::sort( g_by_host.begin(), g_by_host.end() );
    }
    struct sigaction sa = {};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = on_segv;
    sigaction( SIGSEGV, &sa, nullptr );
    sa.sa_sigaction = on_trap;
    sigaction( SIGTRAP, &sa, nullptr );
}

void watch_arm( uint32_t lo, uint32_t hi, double seconds )
{
    watch_init();
    g_lo = lo;
    g_hi = hi;
    g_page = lo & ~0xFFFu;
    g_on = true;
    mprotect( g_base + g_page, 4096, g_writes_only ? PROT_READ : PROT_NONE );
    rt_log( "watch: %08X-%08X for %.1fs%s\n", lo, hi, seconds, g_writes_only ? " (writes)" : "" );
    std::thread( [seconds] {
        std::this_thread::sleep_for( std::chrono::duration<double>( seconds ) );
        g_on = false;
        mprotect( g_base + g_page, 4096, PROT_READ | PROT_WRITE );
        std::lock_guard<std::mutex> g( g_lock );
        for ( auto &[k, n] : g_hits )
            if ( k.second ) rt_log( "watch: %08X touched the range %u times\n", k.first, n );
    } ).detach();
}

// watch_arm_views(physical page, seconds): the same, for a physical page in all its views (A0, C0, E0
// and the plain physical address), counting (function, offset in page, read / write).
void watch_arm_views( uint32_t phys_page, double seconds )
{
    watch_init();
    g_nviews = 0;
    for ( uint32_t v : { 0u, 0xA0000000u, 0xC0000000u, 0xE0000000u } ) g_views[g_nviews++] = v + ( phys_page & 0x1FFFF000u );
    g_on = true;
    for ( int i = 0; i < g_nviews; ++i ) mprotect( g_base + g_views[i], 4096, PROT_NONE );
    rt_log( "watch: physical page %08X in all views for %.1fs\n", phys_page, seconds );
    std::thread( [seconds] {
        std::this_thread::sleep_for( std::chrono::duration<double>( seconds ) );
        g_on = false;
        for ( int i = 0; i < g_nviews; ++i ) mprotect( g_base + g_views[i], 4096, PROT_READ | PROT_WRITE );
        std::lock_guard<std::mutex> g( g_lock );
        for ( auto &[k, n] : g_view_hits )
            rt_log( "watch: %08X %s +%03X %u times\n", std::get<0>( k ), std::get<2>( k ) ? "writes" : "reads", std::get<1>( k ), n );
    } ).detach();
}

// Debug: the callers (guest return addresses) of the pad's virtual axis / action getters asking about
// objects in [lo, hi), counted with the object's offset from lo, for a while.
PPCFunc *rt_hook_indirect( uint32_t guest, PPCFunc *f );
namespace
{
constexpr uint32_t GETTERS[] = { 0x8245B348, 0x8245B420, 0x8245B540, 0x8245B600, 0x82459EE8, 0x82459F80, 0x82459AE0 };
constexpr int NG = sizeof( GETTERS ) / 4;
PPCFunc *g_orig[NG];
uint32_t g_axis_lo, g_axis_hi;
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> g_axis_callers;  // (getter, caller, offset)
template <int I> void getter( uintptr_t ctx, uintptr_t base, uintptr_t frame )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    if ( c->r3.u32 >= g_axis_lo && c->r3.u32 < g_axis_hi )
    {
        std::lock_guard<std::mutex> g( g_lock );
        ++g_axis_callers[{ GETTERS[I], (uint32_t)c->lr, c->r3.u32 - g_axis_lo }];
    }
    g_orig[I]( ctx, base, frame );
}
template <int... I> void hook_all( std::integer_sequence<int, I...> ) { ( ( g_orig[I] = rt_hook_indirect( GETTERS[I], getter<I> ) ), ... ); }
} // namespace

void watch_axis_callers( uint32_t lo, uint32_t hi, double seconds )
{
    g_axis_lo = lo;
    g_axis_hi = hi;
    hook_all( std::make_integer_sequence<int, NG>() );
    rt_log( "watch: getter callers for %08X-%08X, %.1fs\n", lo, hi, seconds );
    std::thread( [seconds] {
        std::this_thread::sleep_for( std::chrono::duration<double>( seconds ) );
        for ( int i = 0; i < NG; ++i ) rt_hook_indirect( GETTERS[i], g_orig[i] );
        std::lock_guard<std::mutex> g( g_lock );
        for ( auto &[k, n] : g_axis_callers )
            rt_log( "watch: getter %08X object +%u read from %08X (%u times)\n", std::get<0>( k ), std::get<2>( k ), std::get<1>( k ), n );
    } ).detach();
}

// Debug: CRACKDOWN_CALLERS="t:secs:f1,f2,...": the callers (guest return addresses) of up to 8 functions
// reached through the function table (virtual methods), counted for secs from t.
namespace
{
uint32_t g_cf[8];
PPCFunc *g_corig[8];
std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_callers;
template <int I> void counted( uintptr_t ctx, uintptr_t base, uintptr_t frame )
{
    {
        std::lock_guard<std::mutex> g( g_lock );
        auto *c = (PPCContext *)( ctx - 0x80 );
        if ( !g_callers.count( { g_cf[I], (uint32_t)c->lr } ) )
        {
            rt_log( "watch: %08X first called from %08X with r3 %08X r4 %08X\n", g_cf[I], (uint32_t)c->lr, c->r3.u32, c->r4.u32 );
            static bool armed;  // CRACKDOWN_WATCH_R3=len:secs: watch [r3, r3+len) of the first function's first call
            if ( const char *w = getenv( "CRACKDOWN_WATCH_R3" ); w && I == 0 && !armed )
            {
                armed = true;
                unsigned len = 0; double secs = 0;
                if ( sscanf( w, "%u:%lf", &len, &secs ) == 2 ) { g_lock.unlock(); watch_arm( c->r3.u32, c->r3.u32 + len, secs ); g_lock.lock(); }
            }
        }
        ++g_callers[{ g_cf[I], (uint32_t)c->lr }];
        static std::map<std::pair<uint32_t, uint32_t>, double> seen;  // non-zero float argument per (function, object)
        if ( c->f1.f64 != 0.0 && std::fabs( c->f1.f64 ) < 1e6 && !seen.count( { g_cf[I], c->r3.u32 } ) )
        {
            seen[{ g_cf[I], c->r3.u32 }] = c->f1.f64;
            rt_log( "watch: %08X got f1 %g for r3 %08X r4 %08X (from %08X)\n", g_cf[I], c->f1.f64, c->r3.u32, c->r4.u32, (uint32_t)c->lr );
        }
    }
    uint32_t obj = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    g_corig[I]( ctx, base, frame );
    // CRACKDOWN_CALLERS_RESULT=1: log each object the first time the function returns a non-zero float for it
    static const bool result = getenv( "CRACKDOWN_CALLERS_RESULT" );
    auto *c = (PPCContext *)( ctx - 0x80 );
    static std::map<std::pair<uint32_t, uint32_t>, int> got;
    if ( result && c->f1.f64 != 0.0 && std::fabs( c->f1.f64 ) < 1e6 )
    {
        std::lock_guard<std::mutex> g( g_lock );
        if ( got[{ g_cf[I], obj }]++ == 0 ) rt_log( "watch: %08X returned %g for object %08X\n", g_cf[I], c->f1.f64, obj );
    }
}
PPCFunc *const g_counted[8] = { counted<0>, counted<1>, counted<2>, counted<3>, counted<4>, counted<5>, counted<6>, counted<7> };
} // namespace

void watch_callers( const char *list, double seconds )
{
    int n = 0;
    for ( const char *c = list; c && *c && n < 8; c = strchr( c, ',' ) ? strchr( c, ',' ) + 1 : nullptr )
    {
        g_cf[n] = (uint32_t)strtoul( c, nullptr, 16 );
        g_corig[n] = rt_hook_indirect( g_cf[n], g_counted[n] );
        ++n;
    }
    rt_log( "watch: callers of %d functions for %.1fs\n", n, seconds );
    std::thread( [n, seconds] {
        std::this_thread::sleep_for( std::chrono::duration<double>( seconds ) );
        for ( int i = 0; i < n; ++i ) rt_hook_indirect( g_cf[i], g_corig[i] );
        std::lock_guard<std::mutex> g( g_lock );
        for ( auto &[k, c] : g_callers ) rt_log( "watch: %08X called from %08X (%u times)\n", k.first, k.second, c );
    } ).detach();
}
