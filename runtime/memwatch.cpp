// Write tracking of guest physical memory (memwatch.h).
#include "memwatch.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <sys/mman.h>

namespace
{
constexpr uint32_t PAGE = 4096, PAGES = 0x20000000 / PAGE;
constexpr uint32_t VIEWS[3] = { 0xA0000000u, 0xC0000000u, 0xE0000000u };  // physical memory's views (main.cpp)

std::atomic<uint32_t> g_serial{ 1 };
std::atomic<uint32_t> g_faults;  // writes caught since the last memwatch_faults()
// The last writes noticed, by write number (slot = number % EVENTS): which pages. A check since a recent
// write number looks only at these; one too far back scans its pages.
constexpr uint32_t EVENTS = 8192;
struct Event { std::atomic<uint32_t> serial; uint32_t first, last; };
Event g_events[EVENTS];
void event( uint32_t serial, uint32_t first, uint32_t last )
{
    Event &e = g_events[serial % EVENTS];
    e.first = first, e.last = last;
    e.serial.store( serial, std::memory_order_release );
}
std::atomic<uint32_t> g_page_write[PAGES];  // write number of each page's last noticed write
std::atomic<uint8_t> g_protected[PAGES];
// Held while protecting and while the handler opens a page, so a page is never left open but marked
// protected (a spin lock: the handler takes it too).
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
struct Lock
{
    Lock() { while ( g_lock.test_and_set( std::memory_order_acquire ) ) {} }
    ~Lock() { g_lock.clear( std::memory_order_release ); }
};
struct sigaction g_previous;

void open_page( uint32_t page )
{
    for ( uint32_t view : VIEWS ) mprotect( g_base + view + page * PAGE, PAGE, PROT_READ | PROT_WRITE );
}

void on_segv( int sig, siginfo_t *si, void *uc )
{
    uintptr_t a = (uintptr_t)si->si_addr - (uintptr_t)g_base;
    if ( si->si_code == SEGV_ACCERR && a >= 0xA0000000u && a < 0x100000000ull )
    {
        uint32_t page = (uint32_t)( a & 0x1FFFFFFF ) / PAGE;
        Lock l;
        if ( g_protected[page].exchange( 0 ) )
        {
            g_faults.fetch_add( 1, std::memory_order_relaxed );
            uint32_t n = g_serial.fetch_add( 1 ) + 1;
            g_page_write[page].store( n );
            event( n, page, page );
            open_page( page );
            return;
        }
        return;  // opened by another thread just now: the write goes through when retried
    }
    // Not ours: as before.
    if ( g_previous.sa_flags & SA_SIGINFO ) return g_previous.sa_sigaction( sig, si, uc );
    if ( g_previous.sa_handler != SIG_IGN && g_previous.sa_handler != SIG_DFL ) return g_previous.sa_handler( sig );
    signal( SIGSEGV, SIG_DFL );
}
} // namespace

void memwatch_init()
{
    static bool done;
    if ( done ) return;
    done = true;
    struct sigaction sa = {};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset( &sa.sa_mask );
    sigaction( SIGSEGV, &sa, &g_previous );
}

uint32_t memwatch_protect( uint32_t phys, uint32_t size )
{
    phys &= 0x1FFFFFFF;
    static const bool off = getenv( "CRACKDOWN_MEMWATCH_OFF" );  // debug: nothing protected (CPU writes unseen)
    if ( !size || off ) return g_serial.load();
    uint32_t first = phys / PAGE, last = std::min( ( phys + size - 1 ) / PAGE, PAGES - 1 );
    Lock l;
    // Runs of pages not yet protected, protected together.
    for ( uint32_t p = first; p <= last; )
    {
        if ( g_protected[p].load() ) { ++p; continue; }
        uint32_t q = p;
        while ( q <= last && !g_protected[q].load() ) g_protected[q++].store( 1 );
        for ( uint32_t view : VIEWS ) mprotect( g_base + view + p * PAGE, ( q - p ) * PAGE, PROT_READ );
        p = q;
    }
    return g_serial.load();
}

bool memwatch_written( uint32_t phys, uint32_t size, uint32_t since )
{
    phys &= 0x1FFFFFFF;
    if ( !size ) return false;
    uint32_t first = phys / PAGE, last = std::min( ( phys + size - 1 ) / PAGE, PAGES - 1 );
    uint32_t now = g_serial.load();
    if ( now - since < EVENTS )
    {
        bool complete = true;
        for ( uint32_t n = since + 1; n != now + 1; ++n )
        {
            const Event &e = g_events[n % EVENTS];
            if ( e.serial.load( std::memory_order_acquire ) != n ) { complete = false; break; }  // not recorded yet: scan
            if ( e.first <= last && e.last >= first ) return true;
        }
        if ( complete ) return false;
    }
    for ( uint32_t p = first; p <= last; ++p )
        if ( g_page_write[p].load( std::memory_order_relaxed ) > since ) return true;
    return false;
}

void memwatch_write( uint32_t phys, uint32_t size )
{
    phys &= 0x1FFFFFFF;
    if ( !size ) return;
    uint32_t first = phys / PAGE, last = std::min( ( phys + size - 1 ) / PAGE, PAGES - 1 );
    uint32_t n = g_serial.fetch_add( 1 ) + 1;
    Lock l;
    // Runs of protected pages opened together (a full-screen resolve covers some 900 pages).
    uint32_t run = UINT32_MAX;
    auto open_run = [&]( uint32_t end ) {
        if ( run == UINT32_MAX ) return;
        for ( uint32_t view : VIEWS ) mprotect( g_base + view + run * PAGE, ( end - run ) * PAGE, PROT_READ | PROT_WRITE );
        run = UINT32_MAX;
    };
    for ( uint32_t p = first; p <= last; ++p )
    {
        if ( g_protected[p].exchange( 0 ) ) { if ( run == UINT32_MAX ) run = p; }
        else open_run( p );
        g_page_write[p].store( n );
    }
    open_run( last + 1 );
    event( n, first, last );
}

uint32_t memwatch_serial() { return g_serial.load(); }

uint32_t memwatch_faults() { return g_faults.exchange( 0 ); }
