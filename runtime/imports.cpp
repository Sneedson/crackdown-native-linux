// Kernel (xboxkrnl) and xam imports, implemented on the host. Each handler reads its arguments
// from the guest registers (r3, r4, ...) and returns its result in r3, as the 360 ABI does.
// Imports without a handler are logged and return 0 (see rt_import).
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"

#include <cstdio>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <map>
#include <cstring>

// Implemented in sync.cpp / files.cpp.
void imp_NtCreateEvent( PPCContext &c );
void imp_NtSetEvent( PPCContext &c );
void imp_NtClearEvent( PPCContext &c );
void imp_NtPulseEvent( PPCContext &c );
void imp_NtCreateMutant( PPCContext &c );
void imp_NtReleaseMutant( PPCContext &c );
void imp_NtCreateSemaphore( PPCContext &c );
void imp_NtReleaseSemaphore( PPCContext &c );
void imp_NtWaitForSingleObjectEx( PPCContext &c );
void imp_NtWaitForMultipleObjectsEx( PPCContext &c );
void imp_ExCreateThread( PPCContext &c );
void imp_NtResumeThread( PPCContext &c );
void imp_NtClose( PPCContext &c );
void imp_NtDuplicateObject( PPCContext &c );
void imp_NtCreateFile( PPCContext &c );
void imp_NtOpenFile( PPCContext &c );
void imp_NtReadFile( PPCContext &c );
void imp_NtQueryInformationFile( PPCContext &c );
void imp_NtSetInformationFile( PPCContext &c );
void imp_NtQueryFullAttributesFile( PPCContext &c );

namespace
{

using Handler = void (*)( PPCContext &c );

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;

uint32_t arg( PPCContext &c, int i )
{
    switch ( i )
    {
    case 0: return c.r3.u32; case 1: return c.r4.u32; case 2: return c.r5.u32; case 3: return c.r6.u32;
    case 4: return c.r7.u32; case 5: return c.r8.u32; case 6: return c.r9.u32; default: return c.r10.u32;
    }
}
void ret( PPCContext &c, uint64_t v ) { c.r3.u64 = v; }

// ------------------------------------------------------------------ memory
// Guest allocations come from two bump regions: "virtual" (NtAllocateVirtualMemory) and
// "physical" (MmAllocatePhysicalMemory*, which the GPU also reads; below). Virtual frees are a no-op.
std::mutex g_mem_lock;
uint32_t g_virtual_next = 0x40000000, g_virtual_end = 0x70000000;
uint32_t g_physical_next = 0xA0000000;  // physical high-water mark

uint32_t bump( uint32_t &next, uint32_t end, uint32_t size, uint32_t align )
{
    std::lock_guard<std::mutex> guard( g_mem_lock );
    uint32_t a = ( next + align - 1 ) & ~( align - 1 );
    if ( a + size > end || a + size < a ) return 0;
    next = a + size;
    return a;
}

// NtAllocateVirtualMemory(PVOID *base, PSIZE_T size, ULONG type, ULONG protect, ...)
void NtAllocateVirtualMemory( PPCContext &c )
{
    uint32_t pbase = arg( c, 0 ), psize = arg( c, 1 );
    uint32_t want = guest_read32( pbase ), size = ( guest_read32( psize ) + 0xFFFF ) & ~0xFFFFu;
    if ( want ) { ret( c, STATUS_SUCCESS ); return; }  // committing inside an earlier reservation
    uint32_t a = bump( g_virtual_next, g_virtual_end, size, 0x10000 );
    if ( !a ) { ret( c, STATUS_NO_MEMORY ); return; }
    guest_write32( pbase, a );
    guest_write32( psize, size );
    ret( c, STATUS_SUCCESS );
}

void NtFreeVirtualMemory( PPCContext &c ) { ret( c, STATUS_SUCCESS ); }

// Physical memory (512 MB at 0xA0000000) is handed out first fit from a list of free ranges and taken
// back on MmFreePhysicalMemory: the game frees and reallocates as it streams the city (about 0.5 MB/s
// driving), and with frees ignored the 512 MB ran out after five minutes or so. A reused range is
// zeroed, as fresh pages are. CRACKDOWN_PHYS_LOG=1 logs every allocation and free.
std::map<uint32_t, uint32_t> g_phys_free = { { 0xA0000000u, 0x20000000u } };  // start -> size
std::unordered_map<uint32_t, uint32_t> g_phys_used;                            // start -> size
uint32_t g_phys_in_use;

uint32_t phys_alloc( uint32_t size, uint32_t align )
{
    std::unique_lock<std::mutex> guard( g_mem_lock );
    for ( auto it = g_phys_free.begin(); it != g_phys_free.end(); ++it )
    {
        const uint32_t start = it->first, end = start + it->second, a = ( start + align - 1 ) & ~( align - 1 );
        if ( a < start || a + size > end || a + size < a ) continue;
        g_phys_free.erase( it );
        if ( a > start ) g_phys_free[start] = a - start;
        if ( a + size < end ) g_phys_free[a + size] = end - ( a + size );
        g_phys_used[a] = size;
        g_phys_in_use += size;
        const uint32_t mark = g_physical_next;  // below it, ranges have been used before
        if ( a + size > mark ) g_physical_next = a + size;
        guard.unlock();
        if ( a < mark ) memset( g_base + a, 0, std::min( a + size, mark ) - a );
        return a;
    }
    return 0;
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, min, max, alignment) -> address or 0
void MmAllocatePhysicalMemoryEx( PPCContext &c )
{
    uint32_t size = ( arg( c, 1 ) + 0xFFF ) & ~0xFFFu, align = arg( c, 5 );
    if ( align < 0x1000 ) align = 0x1000;
    uint32_t a = phys_alloc( size, align );
    static const bool log = getenv( "CRACKDOWN_PHYS_LOG" );
    if ( log ) rt_log( "phys: alloc %X align %X -> %08X\n", size, align, a );
    if ( !a )
    {
        static int n;
        if ( n++ < 5 ) rt_log( "phys: out of physical memory: %u KB wanted, %u MB in use\n", size >> 10, g_phys_in_use >> 20 );
    }
    ret( c, a );
}

// MmFreePhysicalMemory(type, address)
void MmFreePhysicalMemory( PPCContext &c )
{
    const uint32_t a = 0xA0000000u | ( arg( c, 1 ) & 0x1FFFFFFFu );  // any of the three views
    std::lock_guard<std::mutex> guard( g_mem_lock );
    auto used = g_phys_used.find( a );
    static const bool log = getenv( "CRACKDOWN_PHYS_LOG" );
    if ( used == g_phys_used.end() )
    {
        static int n;
        if ( n++ < 5 ) rt_log( "phys: free of %08X, which was not allocated\n", arg( c, 1 ) );
        ret( c, 0 );
        return;
    }
    uint32_t start = a, size = used->second;
    if ( log ) rt_log( "phys: free %08X (%X)\n", a, size );
    g_phys_used.erase( used );
    g_phys_in_use -= size;
    auto next = g_phys_free.lower_bound( start );
    if ( next != g_phys_free.end() && next->first == start + size ) size += next->second, next = g_phys_free.erase( next );
    if ( next != g_phys_free.begin() )
        if ( auto prev = std::prev( next ); prev->first + prev->second == start ) start = prev->first, size += prev->second, g_phys_free.erase( prev );
    g_phys_free[start] = size;
    ret( c, 0 );
}

// ------------------------------------------------------------------ critical sections
// A guest RTL_CRITICAL_SECTION is backed by a host-side recursive lock keyed by its guest address.
// It is first-come-first-served (a ticket lock): a thread that releases and immediately re-enters
// queues behind the threads already waiting, as game loops that hold a lock almost continuously
// otherwise starve their workers. It remembers its owner so a long wait can say who holds it.
struct CriticalSection
{
    std::mutex m;
    std::condition_variable cv;
    std::thread::id owner;
    unsigned depth = 0;
    uint64_t next_ticket = 0, serving = 0;
    uint32_t where_lr = 0, where_sp = 0;  // the holder's import return address and stack, for "stuck" reports
};
std::mutex g_cs_lock;
std::unordered_map<uint32_t, CriticalSection *> g_cs;

CriticalSection &cs_for( uint32_t addr )
{
    // Each thread remembers the ones it has used (they are never freed): the game enters and leaves
    // critical sections half a million times in its busiest frames, every thread taking the one lock
    // below for each.
    struct Slot { uint32_t addr; CriticalSection *cs; };
    thread_local Slot mine[512];
    Slot &slot = mine[( addr >> 2 ) & 511];
    if ( slot.cs && slot.addr == addr ) return *slot.cs;
    std::lock_guard<std::mutex> guard( g_cs_lock );
    auto &m = g_cs[addr];
    if ( !m ) m = new CriticalSection;
    slot = { addr, m };
    return *m;
}

bool cs_enter( uint32_t addr, bool wait )
{
    CriticalSection &cs = cs_for( addr );
    std::unique_lock<std::mutex> lock( cs.m );
    auto me = std::this_thread::get_id();
    if ( cs.depth && cs.owner == me ) { ++cs.depth; return true; }
    if ( !wait && ( cs.depth || cs.serving != cs.next_ticket ) ) return false;
    uint64_t ticket = cs.next_ticket++;
    std::chrono::steady_clock::time_point since;  // read once waiting (every entry reading the clock showed up)
    bool reported = false;
    if ( cs.depth || cs.serving != ticket ) since = std::chrono::steady_clock::now();
    while ( cs.depth || cs.serving != ticket )
    {
        cs.cv.wait_for( lock, std::chrono::seconds( 1 ) );
        if ( !reported && std::chrono::steady_clock::now() - since > std::chrono::seconds( 5 ) )
        {
            reported = true;
            char bt[256];
            rt_guest_backtrace( bt, sizeof(bt) );
            rt_log( "stuck: critical section %08X, waiter %s; held (depth %u) by lr %08X sp %08X\n", addr, bt, cs.depth, cs.where_lr, cs.where_sp );
        }
    }
    cs.owner = me;
    cs.depth = 1;
    cs.where_lr = rt_import_lr, cs.where_sp = rt_import_sp;  // formatting a backtrace on every entry cost more than the lock
    return true;
}

void cs_leave( uint32_t addr )
{
    CriticalSection &cs = cs_for( addr );
    std::lock_guard<std::mutex> lock( cs.m );
    if ( cs.depth && cs.owner == std::this_thread::get_id() && !--cs.depth )
    {
        ++cs.serving;
        cs.cv.notify_all();
    }
}

void RtlInitializeCriticalSection( PPCContext &c ) { cs_for( arg( c, 0 ) ); ret( c, 0 ); }
void RtlInitializeCriticalSectionAndSpinCount( PPCContext &c ) { cs_for( arg( c, 0 ) ); ret( c, STATUS_SUCCESS ); }
void RtlEnterCriticalSection( PPCContext &c ) { cs_enter( arg( c, 0 ), true ); ret( c, 0 ); }
void RtlLeaveCriticalSection( PPCContext &c ) { cs_leave( arg( c, 0 ) ); ret( c, 0 ); }
void RtlTryEnterCriticalSection( PPCContext &c ) { ret( c, cs_enter( arg( c, 0 ), false ) ? 1 : 0 ); }

// ------------------------------------------------------------------ thread-local storage
thread_local uint32_t t_tls[64];
uint64_t g_tls_used;  // bitmap

void KeTlsAlloc( PPCContext &c )
{
    std::lock_guard<std::mutex> guard( g_mem_lock );
    for ( int i = 0; i < 64; ++i )
        if ( !( g_tls_used & ( 1ull << i ) ) ) { g_tls_used |= 1ull << i; ret( c, i ); return; }
    ret( c, 0xFFFFFFFF );
}
void KeTlsFree( PPCContext &c ) { std::lock_guard<std::mutex> g( g_mem_lock ); g_tls_used &= ~( 1ull << ( arg( c, 0 ) & 63 ) ); ret( c, 1 ); }
void KeTlsGetValue( PPCContext &c ) { ret( c, t_tls[arg( c, 0 ) & 63] ); }
void KeTlsSetValue( PPCContext &c ) { t_tls[arg( c, 0 ) & 63] = arg( c, 1 ); ret( c, 1 ); }

// ------------------------------------------------------------------ time
// FILETIME: 100 ns units since 1601-01-01.
void KeQuerySystemTime( PPCContext &c )
{
    timespec ts;
    clock_gettime( CLOCK_REALTIME, &ts );
    uint64_t ft = ( (uint64_t)ts.tv_sec + 11644473600ull ) * 10000000ull + ts.tv_nsec / 100;
    uint32_t p = arg( c, 0 );
    guest_write32( p, (uint32_t)( ft >> 32 ) );
    guest_write32( p + 4, (uint32_t)ft );
    ret( c, 0 );
}

// ------------------------------------------------------------------ strings
// ANSI_STRING { u16 Length; u16 MaximumLength; u32 Buffer; }
void RtlInitAnsiString( PPCContext &c )
{
    uint32_t dst = arg( c, 0 ), src = arg( c, 1 );
    uint16_t len = src ? (uint16_t)strlen( (const char *)g_base + src ) : 0;
    *(uint16_t *)( g_base + dst ) = __builtin_bswap16( len );
    *(uint16_t *)( g_base + dst + 2 ) = __builtin_bswap16( src ? len + 1 : 0 );
    guest_write32( dst + 4, src );
    ret( c, 0 );
}

// ------------------------------------------------------------------ process / system
void KeGetCurrentProcessType( PPCContext &c ) { ret( c, 1 ); }       // title process
void ObReferenceObjectByHandle( PPCContext &c )  // (handle, type, PVOID *object): a stand-in guest object
{
    static uint32_t fake = 0;
    if ( !fake ) fake = bump( g_virtual_next, g_virtual_end, 0x10000, 0x10000 );
    if ( arg( c, 2 ) ) guest_write32( arg( c, 2 ), fake );
    ret( c, STATUS_SUCCESS );
}
void ObDereferenceObject( PPCContext &c ) { ret( c, 0 ); }
void MmQueryStatistics( PPCContext &c )  // MM_STATISTICS: Length then page counts; report 512 MB free
{
    uint32_t p = arg( c, 0 );
    uint32_t len = guest_read32( p );
    memset( g_base + p + 4, 0, len > 4 ? len - 4 : 0 );
    guest_write32( p + 4, 0x20000 );   // TotalPhysicalPages (512 MB)
    guest_write32( p + 8, 0x1000 );    // KernelPages
    guest_write32( p + 12, 0x18000 );  // TitleAvailablePages
    ret( c, STATUS_SUCCESS );
}
void XexCheckExecutablePrivilege( PPCContext &c ) { ret( c, 0 ); }   // no special privileges
void KeBugCheck( PPCContext &c ) { rt_fatal( "guest KeBugCheck(%08X) lr=%08llX\n", arg( c, 0 ), (unsigned long long)c.lr ); }
void KeBugCheckEx( PPCContext &c ) { rt_fatal( "guest KeBugCheckEx(%08X, %08X) lr=%08llX\n", arg( c, 0 ), arg( c, 1 ), (unsigned long long)c.lr ); }
void HalReturnToFirmware( PPCContext &c ) { rt_log( "guest HalReturnToFirmware(%u): exiting\n", arg( c, 0 ) ); exit( 0 ); }

const std::unordered_map<std::string, Handler> g_handlers = {
#define H(n) { #n, n }
    H(NtAllocateVirtualMemory), H(NtFreeVirtualMemory), H(MmAllocatePhysicalMemoryEx), H(MmFreePhysicalMemory),
    H(RtlInitializeCriticalSection), H(RtlInitializeCriticalSectionAndSpinCount), H(RtlEnterCriticalSection),
    H(RtlLeaveCriticalSection), H(RtlTryEnterCriticalSection),
    H(KeTlsAlloc), H(KeTlsFree), H(KeTlsGetValue), H(KeTlsSetValue),
    H(KeQuerySystemTime), H(RtlInitAnsiString),
    { "NtCreateEvent", imp_NtCreateEvent }, { "NtSetEvent", imp_NtSetEvent }, { "NtClearEvent", imp_NtClearEvent }, { "NtPulseEvent", imp_NtPulseEvent }, { "NtCreateMutant", imp_NtCreateMutant }, { "NtReleaseMutant", imp_NtReleaseMutant }, { "NtCreateSemaphore", imp_NtCreateSemaphore }, { "NtReleaseSemaphore", imp_NtReleaseSemaphore }, { "NtWaitForSingleObjectEx", imp_NtWaitForSingleObjectEx }, { "NtWaitForMultipleObjectsEx", imp_NtWaitForMultipleObjectsEx }, { "ExCreateThread", imp_ExCreateThread }, { "NtResumeThread", imp_NtResumeThread }, { "NtClose", imp_NtClose }, { "NtDuplicateObject", imp_NtDuplicateObject }, { "NtCreateFile", imp_NtCreateFile }, { "NtOpenFile", imp_NtOpenFile }, { "NtReadFile", imp_NtReadFile }, { "NtQueryInformationFile", imp_NtQueryInformationFile }, { "NtSetInformationFile", imp_NtSetInformationFile }, { "NtQueryFullAttributesFile", imp_NtQueryFullAttributesFile },
    H(KeGetCurrentProcessType), H(MmQueryStatistics),
    H(XexCheckExecutablePrivilege), H(KeBugCheck), H(KeBugCheckEx), H(HalReturnToFirmware),
#undef H
};

std::unordered_map<std::string, ImportHandler> &registry()
{
    static std::unordered_map<std::string, ImportHandler> r;
    return r;
}

std::mutex g_count_lock;
std::unordered_map<std::string, unsigned> g_unimplemented;

} // namespace

// Bytes the guest has allocated so far (virtual, physical); frees do not return any.
void rt_memory_used( uint32_t &virt, uint32_t &phys )
{
    std::lock_guard<std::mutex> guard( g_mem_lock );
    virt = g_virtual_next - 0x40000000, phys = g_phys_in_use;
}

double input_script_clock();  // xam.cpp
thread_local uint32_t rt_import_lr, rt_import_sp;
thread_local bool rt_trace_imports;  // set around a guest function being studied: its imports are logged with a guest call chain (testing)

extern "C" void rt_import( unsigned int, const char *name, void *ppcCtx, uint8_t * )
{
    auto &c = *(PPCContext *)ppcCtx;
    rt_import_lr = (uint32_t)c.lr;
    thread_local uint32_t uncounted;  // added to the shared count in batches (every thread bumping it was contended)
    if ( ++uncounted == 256 ) g_stats.imports += uncounted, uncounted = 0;
    rt_import_sp = c.r1.u32;
    // Each import's name is one string per call site: its handler is cached by the pointer (looking it
    // up by name built and hashed a std::string on every call).
    struct Entry { const char *name; Handler h; const char *shortname; };
    thread_local Entry cache[1024];
    Entry &slot = cache[( (uintptr_t)name >> 3 ) & 1023];
    if ( slot.name != name )
    {
        const char *sn = strncmp( name, "__import__", 10 ) ? name : name + 10;
        slot = { name, nullptr, sn };
        if ( auto it = g_handlers.find( sn ); it != g_handlers.end() ) slot.h = it->second;
        else if ( auto r = registry().find( sn ); r != registry().end() ) slot.h = r->second;
    }
    const char *shortname = slot.shortname;
    static const bool count_imports = getenv( "CRACKDOWN_IMPORT_STATS" );
    if ( count_imports ) rt_count_import( shortname );
    // CRACKDOWN_TRACE_WINDOW=from,to (input script seconds): every thread's imports in that window too,
    // leaving out the constant ones (critical sections, TLS, timers, waits).
    static const char *window = getenv( "CRACKDOWN_TRACE_WINDOW" );
    bool in_window = false;
    if ( window )
    {
        double from = atof( window ), to = strchr( window, ',' ) ? atof( strchr( window, ',' ) + 1 ) : 1e9, t = input_script_clock();
        in_window = t >= from && t < to && !strstr( shortname, "CriticalSection" ) && !strstr( shortname, "Tls" ) &&
                    !strstr( shortname, "QueryPerformance" ) && !strstr( shortname, "Wait" ) && !strstr( shortname, "printf" ) &&
                    strcmp( shortname, "KeDelayExecutionThread" ) && strcmp( shortname, "NtYieldExecution" );
    }
    if ( rt_trace_imports || in_window )
    {
        std::string chain;  // guest return addresses: each frame saves LR 8 bytes below its caller's stack pointer
        uint32_t sp = c.r1.u32;
        for ( int i = 0; i < 12 && sp; ++i )
        {
            uint32_t back = guest_read32( sp );
            if ( !back || back <= sp ) break;
            char b[12];
            snprintf( b, sizeof( b ), " %08X", guest_read32( back - 8 ) );
            chain += b;
            sp = back;
        }
        rt_log( "trace: %s lr %08X <%s\n", shortname, (uint32_t)c.lr, chain.c_str() );
    }
    if ( slot.h ) { slot.h( c ); return; }

    unsigned n;
    {
        std::lock_guard<std::mutex> guard( g_count_lock );
        n = ++g_unimplemented[shortname];
    }
    if ( n <= 3 )
        rt_log( "unimplemented #%u %s(%08X, %08X, %08X, %08X) lr=%08llX -> 0\n", n, shortname,
                arg( c, 0 ), arg( c, 1 ), arg( c, 2 ), arg( c, 3 ), (unsigned long long)c.lr );
    ret( c, 0 );
}

void rt_register_import( const char *name, ImportHandler h ) { registry()[name] = h; }
uint32_t rt_alloc_virtual( uint32_t size, uint32_t align ) { return bump( g_virtual_next, g_virtual_end, size, align ); }
uint32_t rt_alloc_physical( uint32_t size, uint32_t align ) { return phys_alloc( size, align ); }

// CRACKDOWN_IMPORT_STATS: per-interval call counts, printed by the status thread.
namespace
{
std::mutex g_stats_lock;
std::unordered_map<std::string, uint64_t> g_import_counts;
}
void rt_count_import( const char *name )
{
    std::lock_guard<std::mutex> g( g_stats_lock );
    ++g_import_counts[name];
}
void rt_print_import_stats()
{
    std::vector<std::pair<uint64_t, std::string>> v;
    {
        std::lock_guard<std::mutex> g( g_stats_lock );
        for ( auto &[n, c] : g_import_counts ) v.push_back( { c, n } );
        g_import_counts.clear();
    }
    std::sort( v.rbegin(), v.rend() );
    std::string line = "  imports:";
    for ( size_t i = 0; i < v.size() && i < 14; ++i ) line += " " + v[i].second + "=" + std::to_string( v[i].first );
    std::string rare;
    for ( size_t i = 14; i < v.size(); ++i )
        if ( v[i].first <= 4 ) rare += " " + v[i].second + "=" + std::to_string( v[i].first );
    rt_log( "%s\n", line.c_str() );
    if ( !rare.empty() ) rt_log( "  rare:%s\n", rare.c_str() );
}
