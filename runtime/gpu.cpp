// Stage-1 GPU: no rendering yet. A command-processor thread walks the PM4 packets D3D writes into
// the ring buffer, asynchronously like the real GPU, and performs only the side effects the CPU
// waits on (memory writes, fences, scratch write-backs, waits, indirect buffers, interrupts, swaps).
// A 60 Hz thread raises the vertical-blank interrupt through the game's callback.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "gpu_ops.h"
#include "xxhash.h"
#include <unordered_map>

#include <array>
#include <cstring>
#include <atomic>
#include <functional>
#include <vector>
#include <string>
#include <csignal>
#include <execinfo.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <condition_variable>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <thread>
#include <deque>
#include <algorithm>

void renderer_defer( std::function<void()> f );
bool renderer_poll( bool kick );

namespace
{
thread_local bool t_cp;  // on the command-processor thread: guest-visible effects wait for the GPU work before them

std::mutex g_cp_lock;      // ring configuration and the command processor's walk
std::mutex g_wptr_lock;    // the CPU's latest write pointer
std::condition_variable g_wptr_cv;
uint32_t g_wptr;           // latest CP_RB_WPTR the CPU wrote
uint64_t g_wptr_seq;       // bumps on every write
uint32_t g_ring_pos;       // ring index of the packet being executed (diagnostics)
uint32_t g_pkt_addr[5];    // packet being executed at each indirect-buffer depth (diagnostics)
uint32_t g_ib_base[5];     // start of the indirect buffer run at each depth (diagnostics)
std::vector<uint32_t> g_ib_snapshot;  // CRACKDOWN_IB_SNAPSHOT: the depth-1 buffer as it started
int g_pkt_depth = -1;
uint64_t g_bin_mask = ~0ull, g_bin_select = ~0ull;  // predicated tiling state
uint32_t g_rb_base, g_rb_size_dwords, g_rb_rptr, g_rptr_writeback;
uint32_t g_int_callback, g_int_context;
std::atomic<uint64_t> g_swaps{ 0 };
int g_log_budget = 120;
uint32_t g_last_event_addr, g_last_event_value;
// CRACKDOWN_GPU_TAIL=seconds: keep the last packets and print them after that long (stall forensics).
constexpr unsigned TAIL = 120;
char g_tail[TAIL][120];
unsigned g_tail_pos;
bool g_tail_on = [] {
    const char *t = RT_ENV( "CRACKDOWN_GPU_TAIL" );
    if ( !t ) return false;
    int secs = atoi( t );
    std::thread( [secs] {
        std::this_thread::sleep_for( std::chrono::seconds( secs ) );  // racy snapshot (the CP may be mid-wait)
        rt_log( "gpu: last %u packets (wptr writes %llu, rptr %X, last event write %08X = %08X):\n", TAIL,
                (unsigned long long)g_stats.wptr_writes, g_rb_rptr, g_last_event_addr, g_last_event_value );
        for ( unsigned i = 0; i < TAIL; ++i ) if ( g_tail[( g_tail_pos + i ) % TAIL][0] ) rt_log( "  %s\n", g_tail[( g_tail_pos + i ) % TAIL] );
    } ).detach();
    return true;
}();  // CRACKDOWN_GPU_LOG lines left (refilled at each ring setup)

// Physical (GPU) addresses -> guest virtual (our physical allocations live at 0xA0000000 + P).
uint32_t phys( uint32_t a ) { return a < 0x20000000 ? 0xA0000000 | a : a; }

// Guest contexts that run interrupt callbacks raised by INTERRUPT packets, one per hardware thread:
// the handler acknowledges by clearing the bit of the processor it runs on (PCR+0x10C).
GuestThread *g_irq_thread[6];

// GPU register file (dword index). Writes to the CP scratch registers are mirrored to memory at
// SCRATCH_ADDR when their SCRATCH_UMSK bit is set -- one way D3D learns the GPU got somewhere.
constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC, REG_SCRATCH_ADDR = 0x01DD, REG_SCRATCH0 = 0x0578;
uint32_t g_regs[0x10000];
struct ScratchWrite { uint32_t reg, value, tid, ms, slot; } g_scratch_log[16];

void write_reg( uint32_t r, uint32_t v )
{
    r &= 0xFFFF;
    // SCRATCH_UMSK / SCRATCH_ADDR are set by D3D during start-up (the address once more early on). A
    // later change means the command processor ran something that isn't a command: a command buffer it
    // reached before the game finished writing it (the hang entering the garage vehicle pop-up, pausing
    // in a car, or the dossier video after the tunnel: the scratch fence D3D waits on then never landed).
    // The write is reported and dropped, so such data costs a glitch at most.
    static int changes[2];
    bool stray = ( r == REG_SCRATCH_UMSK || r == REG_SCRATCH_ADDR ) && g_regs[r] && g_regs[r] != v && ++changes[r - REG_SCRATCH_UMSK] > 1;
    if ( stray )
    {
        static int k;
        if ( k++ < 4 )
        {
            rt_log( "gpu: register %04X changed %08X -> %08X (thread %d, ring index %X, wptr %X)\n", r, g_regs[r], v, (int)gettid(), g_ring_pos, g_wptr );
            for ( int d = 0; d <= g_pkt_depth && d < 5; ++d )
            {
                uint32_t q = g_pkt_addr[d];
                rt_log( "gpu:   depth %d packet at %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", d, q, guest_read32( q ), guest_read32( q + 4 ),
                        guest_read32( q + 8 ), guest_read32( q + 12 ), guest_read32( q + 16 ), guest_read32( q + 20 ), guest_read32( q + 24 ), guest_read32( q + 28 ) );
            }
            // The indirect buffer being run, packet by packet from its start up to here: the last packets
            // before the stray one show which length was misread.
            if ( g_pkt_depth >= 1 && g_ib_base[g_pkt_depth] )
            {
                uint32_t a = g_ib_base[g_pkt_depth], end = g_pkt_addr[g_pkt_depth];
                std::vector<std::string> lines;
                for ( int guard = 0; a <= end && guard < 100000; ++guard )
                {
                    uint32_t h = guest_read32( a ), len = ( h >> 30 ) == 0 ? ( ( h >> 16 ) & 0x3FFF ) + 2 : ( h >> 30 ) == 1 ? 3 : ( h >> 30 ) == 2 ? 1 : ( ( h >> 16 ) & 0x3FFF ) + 2;
                    char b[160];
                    snprintf( b, sizeof( b ), "gpu:     %08X: %08X (%u dwords) %08X %08X %08X", a, h, len, guest_read32( a + 4 ), guest_read32( a + 8 ), guest_read32( a + 12 ) );
                    lines.push_back( b );
                    a += len * 4;
                }
                rt_log( "gpu:   indirect buffer %08X, last packets before %08X:\n", g_ib_base[g_pkt_depth], end );
                if ( g_pkt_depth == 1 && !g_ib_snapshot.empty() )
                {
                    uint32_t changed = 0, first = ~0u;
                    for ( uint32_t i = 0; i < g_ib_snapshot.size(); ++i )
                        if ( guest_read32( g_ib_base[1] + i * 4 ) != g_ib_snapshot[i] ) { ++changed; if ( first == ~0u ) first = i; }
                    rt_log( "gpu:   since it started, %u of its %zu dwords changed (first at %08X)\n", changed, g_ib_snapshot.size(),
                            first == ~0u ? 0 : g_ib_base[1] + first * 4 );
                    uint32_t at = ( end - g_ib_base[1] ) / 4;
                    for ( uint32_t i = at >= 8 ? at - 8 : 0; i < at + 8 && i < g_ib_snapshot.size(); ++i )
                        rt_log( "gpu:     %08X: was %08X now %08X\n", g_ib_base[1] + i * 4, g_ib_snapshot[i], guest_read32( g_ib_base[1] + i * 4 ) );
                }
                for ( size_t i = lines.size() > 24 ? lines.size() - 24 : 0; i < lines.size(); ++i ) rt_log( "%s\n", lines[i].c_str() );
            }
        }
    }
    if ( stray ) return;
    g_regs[r] = v;
    if ( ( r == REG_SCRATCH_UMSK || r == REG_SCRATCH_ADDR || ( r >= REG_SCRATCH0 && r < REG_SCRATCH0 + 8 ) ) && RT_ENV( "CRACKDOWN_GPU_LOG" ) )
    {
        static int n;
        if ( n++ < 400 && ( r != REG_SCRATCH0 + 4 && r != REG_SCRATCH0 + 5 ) ) rt_log( "gpu: reg %04X = %08X\n", r, v );
    }
    if ( r == REG_SCRATCH0 || r == REG_SCRATCH_UMSK )  // recent writers, for a stuck wait's report
    {
        static std::atomic<uint32_t> k;
        uint32_t j = k++ % 16;
        g_scratch_log[j] = { r, v, (uint32_t)gettid(), (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count(), j };
    }
    if ( r >= REG_SCRATCH0 && r < REG_SCRATCH0 + 8 )
    {
        uint32_t i = r - REG_SCRATCH0;
        if ( ( g_regs[REG_SCRATCH_UMSK] >> i ) & 1 )
        {
            uint32_t addr = phys( g_regs[REG_SCRATCH_ADDR] ) + i * 4;
            guest_write32( addr, v );
        }
    }
}

// A run of registers from `first`: copied straight in, unless it touches the few whose writes do more
// (the scratch registers) -- shader constants come hundreds at a time.
void write_regs( uint32_t first, const uint32_t *v, uint32_t n )
{
    first &= 0xFFFF;
    const uint32_t last = first + n;
    if ( !n ) return;
    if ( last > 0x10000 || ( first <= REG_SCRATCH_ADDR && last > REG_SCRATCH_UMSK ) || ( first < REG_SCRATCH0 + 8 && last > REG_SCRATCH0 ) )
    {
        for ( uint32_t i = 0; i < n; ++i ) write_reg( first + i, v[i] );
        return;
    }
    memcpy( &g_regs[first], v, n * 4 );
}

void raise_interrupt( GuestThread *t, uint32_t source )
{
    if ( g_int_callback ) rt_guest_call( t, g_int_callback, source, g_int_context );
}

void execute( uint32_t addr, uint32_t count, int depth );
}
// A guest-visible effect of a packet. These happen when the command processor reaches the packet, not
// when the GPU work before it completes: deferring them all made every wait mid-frame a round trip
// through the GPU. Results of GPU work (the presented frame, occlusion counts) and D3D's event-write
// fences wait for it (renderer_defer).
template <class F> void effect( F f ) { f(); }
struct HiFrame { const void *pixels; uint32_t w, h; bool rgba; };
void video_present( const uint32_t fetch[6], HiFrame hi );  // video.cpp
int renderer_hires_show( uint32_t base, uint32_t &w, uint32_t &h, const void *&pixels, bool &rgba );  // renderer.cpp
// xenos.cpp: the drawing side (shaders, draw calls).
void xenos_shader_load( uint32_t type, uint32_t addr, uint32_t dwords );
const void *xenos_shader_find( uint32_t type, uint32_t addr, uint32_t dwords );
void xenos_shader_bind( const void *shader );
bool native_d3d();
void xenos_draw( uint32_t initiator, uint32_t index_base, uint32_t index_size, const uint32_t *regs );
bool renderer_tracing();  // renderer.cpp
void renderer_zpd( uint32_t addr );
void renderer_clear( const uint32_t *regs, const uint32_t *op );

// CRACKDOWN_WATCH_EXTENTS=1: in the traced frame, page-protect the target of the first 5A packet in all
// three physical views and report (once) which code touches it.
static uint32_t g_watch_page;
static void watch_handler( int, siginfo_t *si, void *ctx )
{
    uint8_t *a = (uint8_t *)si->si_addr;
    uint32_t off = (uint32_t)( a - g_base );
    for ( uint32_t view : { 0xA0000000u, 0xC0000000u, 0xE0000000u } )
        mprotect( g_base + view + g_watch_page, 4096, PROT_READ | PROT_WRITE );
    void *frames[24];
    int n = backtrace( frames, 24 );
    char buf[160];
    int len = snprintf( buf, sizeof( buf ), "watch: access to guest %08X from rip %p (thread %ld); backtrace:\n", off,
                        (void *)( (ucontext_t *)ctx )->uc_mcontext.gregs[REG_RIP], (long)gettid() );
    write( 2, buf, len );
    backtrace_symbols_fd( frames, n, 2 );
}
static void watch_arm( uint32_t phys_addr )
{
    g_watch_page = phys_addr & 0x1FFFF000;
    struct sigaction sa = {};
    sa.sa_sigaction = watch_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction( SIGSEGV, &sa, nullptr );
    for ( uint32_t view : { 0xA0000000u, 0xC0000000u, 0xE0000000u } )
        mprotect( g_base + view + g_watch_page, 4096, PROT_NONE );
    rt_log( "watch: armed on physical page %08X\n", g_watch_page );
}
void xenos_frame_end();
namespace
{

// WAIT_REG_MEM / COND_WRITE comparison functions.
bool compare( uint32_t fn, uint32_t cur, uint32_t ref )
{
    switch ( fn )
    {
    case 0: return false;
    case 1: return cur < ref;
    case 2: return cur <= ref;
    case 3: return cur == ref;
    case 4: return cur != ref;
    case 5: return cur >= ref;
    case 6: return cur > ref;
    default: return true;
    }
}

// D3D's fence: value to address (physical) when the GPU work before it is done.
void event_write( uint32_t addr, uint32_t value )
{
    // D3D's fences (a counter and its ring position): the value lands when the GPU work before it is
    // done, as the game frees or rewrites what that work reads once it sees the fence pass (a skipped
    // Bink movie's planes were cleared before the GPU drew its last frame: a green flash).
    // CRACKDOWN_IMMEDIATE_EVENTS=1: at command-processor time instead.
    static const bool defer = !RT_ENV( "CRACKDOWN_IMMEDIATE_EVENTS" );
    auto f = [dst = phys( addr & ~3u ), v = value] { guest_write32( dst, v ); };
    if ( defer && t_cp ) renderer_defer( f ); else effect( f );
    if ( RT_ENV( "CRACKDOWN_EVENT_LOG" ) ) { static int k; if ( k++ < 200 ) rt_log( "gpu: event write %02X -> %08X = %08X\n", 0x58, addr, value ); }
}

// The GPU waiting until memory (info bit 0x10) or a register, masked, compares (info & 7) to ref.
void wait_reg_mem( uint32_t info, uint32_t poll, uint32_t ref, uint32_t mask )
{
    auto start = std::chrono::steady_clock::now(), deadline = start + std::chrono::seconds( 2 );
    bool reported = false;
    for ( ;; )
    {
        if ( t_cp ) renderer_poll( std::chrono::steady_clock::now() - start > std::chrono::milliseconds( 1 ) );  // an occlusion count may wait for the GPU
        uint32_t cur = ( info & 0x10 ? guest_read32( phys( poll & ~3u ) ) : g_regs[poll & 0xFFFF] ) & mask;
        if ( compare( info & 7, cur, ref ) )
        {
            auto took = std::chrono::steady_clock::now() - start;
            static int slow;
            if ( took > std::chrono::milliseconds( 2 ) && slow++ < 20 )
                rt_log( "gpu: WAIT_REG_MEM %08X %s %X & %X == %X took %lld us\n", info, info & 0x10 ? "mem" : "reg", poll, mask, ref,
                        (long long)std::chrono::duration_cast<std::chrono::microseconds>( took ).count() );
            break;
        }
        if ( !reported && std::chrono::steady_clock::now() > deadline )  // the real GPU waits forever too
        {
            reported = true;
            rt_log( "gpu: WAIT_REG_MEM %08X still waiting after 2 s (%s %X & %X == %X, now %X)\n", info, info & 0x10 ? "mem" : "reg", poll, mask, ref, cur );
            rt_log( "gpu:   scratch umsk %X addr %X reg0 %X mem0 %X; gpu thread %d; recent writes:\n", g_regs[REG_SCRATCH_UMSK], g_regs[REG_SCRATCH_ADDR],
                    g_regs[REG_SCRATCH0], guest_read32( phys( g_regs[REG_SCRATCH_ADDR] ) ), (int)gettid() );
            for ( auto &w : g_scratch_log )
                if ( w.ms ) rt_log( "gpu:     reg %04X = %X by thread %u at %u ms\n", w.reg, w.value, w.tid, w.ms );
        }
        std::this_thread::yield();
    }
}

// Interrupting the hardware threads in mask (the game's GPU interrupt callback runs on them). h, a,
// depth: the command that asked, for the report of a missing callback.
void interrupt( uint32_t mask, uint32_t h, uint32_t a, int depth )
{
    if ( guest_read32( phys( g_regs[REG_SCRATCH_ADDR] ) + 16 ) == 0x0BADF00D )
        rt_log( "gpu: INTERRUPT (mask %X) with no callback in memory (register %08X): header %08X at %08X depth %d, ring %X, bins %llX/%llX\n",
                mask, g_regs[REG_SCRATCH0 + 4], h, a - 4, depth, g_ring_pos,
                (unsigned long long)g_bin_mask, (unsigned long long)g_bin_select );
    if ( RT_ENV( "CRACKDOWN_GPU_IRQ_LOG" ) )
    {
        static int k, j;
        if ( g_regs[REG_SCRATCH0 + 4] == 0x82757540 && j++ < 12 )
            rt_log( "gpu: INTERRUPT -> 82757540(%08X) from packet at %08X depth %d, ring index %X (wptr %X)\n", g_regs[REG_SCRATCH0 + 5], a - 4, depth, g_ring_pos, g_wptr );
        if ( k++ < 60 ) rt_log( "gpu: INTERRUPT mask %X, scratch0 %X 1 %X 4 %08X 5 %08X\n", mask, g_regs[REG_SCRATCH0], g_regs[REG_SCRATCH0 + 1],
                                g_regs[REG_SCRATCH0 + 4], g_regs[REG_SCRATCH0 + 5] );
    }
    effect( [mask] {
        for ( int cpu = 0; cpu < 6; ++cpu )
            if ( mask & ( 1u << cpu ) ) raise_interrupt( g_irq_thread[cpu], 1 );
    } );
}

// Presenting the front buffer (its texture fetch constant) once the frame is drawn.
void present( const uint32_t *w )
{
    ++g_swaps;
    std::array<uint32_t, 6> fetch;
    for ( int i = 0; i < 6; ++i ) fetch[i] = w[i];
    xenos_frame_end();
    HiFrame hi{};
    if ( renderer_hires_show( fetch[1] & 0xFFFFF000u, hi.w, hi.h, hi.pixels, hi.rgba ) < 0 ) hi = {};
    renderer_defer( [fetch, hi] { video_present( fetch.data(), hi ); } );
}

// A type-3 packet (header h): its n dwords in host order at w, from guest address a.
void type3( uint32_t h, const uint32_t *w, uint32_t a, int depth )
{
    uint32_t n = ( ( h >> 16 ) & 0x3FFF ) + 1, op = ( h >> 8 ) & 0x7F;
    uint32_t small[8] = {};  // the logs below show four words of any packet
    if ( n < 8 ) { memcpy( small, w, n * 4 ); w = small; }
    if ( op == 0x5A && renderer_tracing() && RT_ENV( "CRACKDOWN_WATCH_EXTENTS" ) )
    {
        static bool armed;
        if ( !armed ) { armed = true; watch_arm( w[1] & ~3u ); }
    }
    if ( renderer_tracing() )  // CRACKDOWN_EDRAM_TRACE frame: every type-3 packet
        rt_log( "pkt: @%08X %*s%02X n=%u%s mask %llX sel %llX: %08X %08X %08X %08X\n", a - 4, depth * 2, "", op, n, ( h & 1 ) ? " pred" : "",
                (unsigned long long)g_bin_mask, (unsigned long long)g_bin_select, w[0], w[1], w[2],
                w[3] );
    static bool seen[128];
    if ( !seen[op] ) { seen[op] = true; rt_log( "gpu: first type-3 opcode %02X (%u dwords) at %08X\n", op, n, a - 4 ); }
    if ( g_tail_on )
    {
        char *line = g_tail[g_tail_pos++ % TAIL];
        snprintf( line, 120, "%*sop %02X n=%u @%08X: %08X %08X %08X %08X", depth * 2, "", op, n, a - 4, w[0], w[1], w[2], w[3] );
    }
    if ( RT_ENV( "CRACKDOWN_GPU_PACKETS" ) && g_log_budget-- > 0 )
        rt_log( "gpu:   %*sop %02X n=%u: %08X %08X %08X\n", depth * 2, "", op, n, w[0], w[1], w[2] );
    switch ( op )
    {
    case 0x5B:  // EVENT_WRITE_ZPD: occlusion query sample counts to RB_SAMPLE_COUNT_ADDR (renderer.cpp)
        renderer_zpd( phys( g_regs[0x2325] ) - 0xA0000000 );
        if ( RT_ENV( "CRACKDOWN_ZPD_LOG" ) )
        {
            static int k;
            if ( k++ < 40 )
            {
                uint32_t addr = g_regs[0x2325];
                rt_log( "gpu: ZPD %08X ctl %08X addr %08X mem %08X %08X %08X %08X %08X %08X %08X %08X\n", w[0], g_regs[0x2324], addr,
                        guest_read32( phys( addr ) ), guest_read32( phys( addr ) + 4 ), guest_read32( phys( addr ) + 8 ), guest_read32( phys( addr ) + 12 ),
                        guest_read32( phys( addr ) + 16 ), guest_read32( phys( addr ) + 20 ), guest_read32( phys( addr ) + 24 ), guest_read32( phys( addr ) + 28 ) );
            }
        }
        break;
    case 0x3D:  // MEM_WRITE: address, then values
    {
        uint32_t dst = phys( w[0] & ~3u );
        std::vector<uint32_t> values;
        for ( uint32_t i = 1; i < n; ++i ) values.push_back( w[i] );
        effect( [dst, values] { for ( size_t i = 0; i < values.size(); ++i ) guest_write32( dst + (uint32_t)i * 4, values[i] ); } );
    }
        break;
    case 0x5A:  // event 0x1A, address: screen extents of the draws since the last one, read back by the
    {           // game's D3D (sub_82756DA0) as big-endian halfwords min x, max x, min y, max y in 8-pixel
                // units (max inclusive) and turned into each object's tile bin mask for the next pass. We
                // don't track where draws land, so report the whole screen: every object draws in every tile.
        if ( ( w[0] & 0x3F ) == 0x1A && n >= 2 )
        {
            uint32_t addr = phys( w[1] & ~3u );
            effect( [addr] {
                const uint16_t extents[4] = { 0, 8191 >> 3, 0, 8191 >> 3 };
                for ( int i = 0; i < 4; ++i ) guest_write16( addr + i * 2, extents[i] );
            } );
        }
        break;
    }
    case 0x58:  // EVENT_WRITE_SHD: initiator, address, value
    case 0x59:  // EVENT_WRITE_EXT
        if ( RT_ENV( "CRACKDOWN_GPU_LOG" ) )
        {
            if ( g_log_budget-- > 0 ) rt_log( "gpu: event-write %02X: %08X %08X %08X\n", op, w[0], w[1], w[2] );
        }
        if ( n >= 3 ) event_write( w[1], w[2] );
        g_last_event_addr = w[1];
        g_last_event_value = w[2];
        break;
    case 0x3C:  // WAIT_REG_MEM: info, poll address/register, reference, mask, interval
        wait_reg_mem( w[0], w[1], w[2], w[3] );
        break;
    case 0x45:  // COND_WRITE: info, poll address/register, reference, mask, write address/register, value
    {
        uint32_t info = w[0], poll = w[1], ref = w[2], mask = w[3];
        uint32_t dst = w[4], val = w[5];
        uint32_t cur = ( info & 0x10 ? guest_read32( phys( poll & ~3u ) ) : g_regs[poll & 0xFFFF] ) & mask;
        if ( compare( info & 7, cur, ref ) )
        {
            if ( info & 0x100 ) effect( [d = phys( dst & ~3u ), val] { guest_write32( d, val ); } );
            else write_reg( dst, val );
        }
        break;
    }
    case 0x3F:  // INDIRECT_BUFFER: address, size in dwords
        if ( RT_ENV( "CRACKDOWN_GPU_LOG" ) && g_log_budget-- > 0 )
            rt_log( "gpu: indirect buffer %08X, %u dwords (depth %d)\n", w[0], w[1], depth );
        if ( depth < 4 )
        {
            g_ib_base[depth + 1] = phys( w[0] );
            // CRACKDOWN_IB_SNAPSHOT=1 (diagnostics): a copy of the buffer as it was when it started, for
            // the stray-packet report to compare against.
            if ( static const bool snap = RT_ENV( "CRACKDOWN_IB_SNAPSHOT" ); snap && depth == 0 )
            {
                uint32_t words = w[1] & 0xFFFFF;
                g_ib_snapshot.resize( words );
                for ( uint32_t i = 0; i < words; ++i ) g_ib_snapshot[i] = guest_read32( g_ib_base[1] + i * 4 );
            }
            execute( phys( w[0] ), w[1] & 0xFFFFF, depth + 1 );
        }
        break;
    case 0x54:  // INTERRUPT: mask of the hardware threads to interrupt
        interrupt( w[0], h, a, depth );
        break;
    case 0x60: g_bin_mask = ( g_bin_mask & ~0xFFFFFFFFull ) | w[0]; break;                     // SET_BIN_MASK_LO
    case 0x61: g_bin_mask = ( g_bin_mask & 0xFFFFFFFFull ) | ( (uint64_t)w[0] << 32 ); break;   // SET_BIN_MASK_HI
    case 0x62: g_bin_select = ( g_bin_select & ~0xFFFFFFFFull ) | w[0]; break;                 // SET_BIN_SELECT_LO
    case 0x63: g_bin_select = ( g_bin_select & 0xFFFFFFFFull ) | ( (uint64_t)w[0] << 32 ); break; // SET_BIN_SELECT_HI
    case 0x64:  // XE_SWAP (written by our VdSwap): present the front buffer
        present( w );
        break;
    case 0x2D:  // SET_CONSTANT: (type << 16 | index), values -- type 0 ALU, 1 fetch, 2 bool, 3 loop, 4 registers
    {
        static const uint32_t bases[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        uint32_t d0 = w[0], type = ( d0 >> 16 ) & 0xFF;
        if ( type < 5 )
            if ( n > 1 ) write_regs( bases[type] + ( d0 & 0x7FF ), w + 1, n - 1 );
        break;
    }
    case 0x2F:  // LOAD_ALU_CONSTANT: address, (type << 16 | index), size in dwords -- constants from memory
    {
        static const uint32_t bases[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        uint32_t src = phys( w[0] & ~3u ), d1 = w[1], size = w[2] & 0xFFF, type = ( d1 >> 16 ) & 0xFF;
        if ( type < 5 )
            for ( uint32_t i = 0; i < size; ++i ) write_reg( bases[type] + ( d1 & 0x7FF ) + i, guest_read32( src + i * 4 ) );
        break;
    }
    case 0x27:  // IM_LOAD: address | shader type, (start << 16 | size): shader microcode from memory
        xenos_shader_load( w[0] & 3, phys( w[0] & ~3u ), w[1] & 0xFFFF );
        break;
    case 0x2B:  // IM_LOAD_IMMEDIATE: shader type, (start << 16 | size), microcode
        xenos_shader_load( w[0] & 3, a + 8, w[1] & 0xFFFF );
        break;
    case 0x22:  // DRAW_INDX: viz, VGT_DRAW_INITIATOR, [index base, index size]
        xenos_draw( w[1], n >= 3 ? w[2] : 0, n >= 4 ? w[3] : 0, g_regs );
        break;
    case 0x36:  // DRAW_INDX_2: VGT_DRAW_INITIATOR, immediate indices
        xenos_draw( w[0], a + 4, 0, g_regs );
        break;
    case 0x55:  // SET_CONSTANT2: register index, values
    case 0x56:  // SET_SHADER_CONSTANTS: register index, values
    {
        uint32_t index = w[0] & 0xFFFF;
        if ( n > 1 ) write_regs( index, w + 1, n - 1 );
        break;
    }
    case 0x21:  // REG_RMW: info, AND mask, OR mask (bits 31/30 of info: masks name registers)
    {
        uint32_t info = w[0], and_m = w[1], or_m = w[2], r = info & 0x1FFF;
        uint32_t v = g_regs[r];
        v &= ( info >> 31 ) ? g_regs[and_m & 0x1FFF] : and_m;
        v |= ( ( info >> 30 ) & 1 ) ? g_regs[or_m & 0x1FFF] : or_m;
        static bool seen[0x2000];
        if ( !seen[r] ) { seen[r] = true; rt_log( "gpu: REG_RMW %04X &%08X |%08X\n", r, and_m, or_m ); }
        write_reg( r, v );
        break;
    }
    default:  // other state and events: not modelled yet (CRACKDOWN_GPU_UNKNOWN=1 logs each opcode's first use)
        if ( static const bool log = RT_ENV( "CRACKDOWN_GPU_UNKNOWN" ); log )
        {
            static bool seen[128];
            if ( !seen[op & 127] ) { seen[op & 127] = true; rt_log( "gpu: unhandled packet %02X n=%u: %08X %08X %08X\n", op, n, w[0], w[1], w[2] ); }
        }
        break;
    }
}

// One packet at `p`; returns its length in dwords.
uint32_t packet( uint32_t p, int depth )
{
    uint32_t h = guest_read32( p );
    if ( depth < 5 ) { g_pkt_addr[depth] = p; g_pkt_depth = depth; }
    switch ( h >> 30 )
    {
    case 0:  // count+1 writes starting at a register (bit 15: all to the same one)
    {
        uint32_t n = ( ( h >> 16 ) & 0x3FFF ) + 1, base = h & 0x7FFF;
        if ( g_tail_on )
            snprintf( g_tail[g_tail_pos++ % TAIL], 120, "%*sregs %04X x%u @%08X: %08X %08X", depth * 2, "", base, n, p, guest_read32( p + 4 ), n > 1 ? guest_read32( p + 8 ) : 0 );
        for ( uint32_t i = 0; i < n; ++i ) write_reg( h & 0x8000 ? base : base + i, guest_read32( p + 4 + i * 4 ) );
        return n + 1;
    }
    case 1:  // two registers
        if ( g_tail_on )
            snprintf( g_tail[g_tail_pos++ % TAIL], 120, "%*sreg2 %04X=%08X %04X=%08X @%08X", depth * 2, "", h & 0x7FF, guest_read32( p + 4 ), ( h >> 11 ) & 0x7FF, guest_read32( p + 8 ), p );
        write_reg( h & 0x7FF, guest_read32( p + 4 ) );
        write_reg( ( h >> 11 ) & 0x7FF, guest_read32( p + 8 ) );
        return 3;
    case 2: return 1;  // nop
    }
    uint32_t n = ( ( h >> 16 ) & 0x3FFF ) + 1;
    // Predicated packets (header bit 0) run only when the bin mask and bin selection overlap.
    if ( ( h & 1 ) && !( g_bin_mask & g_bin_select ) ) return n + 1;
    thread_local std::vector<uint32_t> args;
    args.resize( n );
    for ( uint32_t i = 0; i < n; ++i ) args[i] = guest_read32( p + 4 + i * 4 );
    type3( h, args.data(), p + 4, depth );
    return n + 1;
}

// Whether the dwords at p can be a packet with `room` dwords left in its buffer: a type-3 packet with a
// PM4 opcode, or a type-0 register run, that ends inside the buffer.
bool plausible( uint32_t p, uint32_t room )
{
    static const auto ops = [] {
        std::array<bool, 128> t{};
        for ( int op : { 0x10, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x34, 0x35, 0x36, 0x37, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x44, 0x45, 0x46,
                         0x48, 0x4A, 0x4B, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x60, 0x61, 0x62, 0x63, 0x64 } )
            t[op] = true;
        return t;
    }();
    uint32_t h = guest_read32( p ), n = ( ( h >> 16 ) & 0x3FFF ) + 1;
    switch ( h >> 30 )
    {
    case 0: return n + 1 <= room && ( h & 0x7FFF ) + ( h & 0x8000 ? 1 : n ) <= 0x6000;
    case 1: return room >= 3;
    case 2: return true;
    default: return n + 1 <= room && ops[( h >> 8 ) & 0x7F];
    }
}

// An indirect buffer. The command processor sometimes reaches data that is no packet (seen as the game
// opened the Agency garage's vehicle pop-up, loaded the city or played the dossier video): run, it
// wrote garbage into registers and waited on fences that never came - a hang. Such a packet is given
// up to 100 ms to become a plausible one (if the game is still writing the buffer); failing that the
// rest of the buffer is skipped, a glitch at most. Each case is reported.
void execute( uint32_t addr, uint32_t count, int depth )
{
    for ( uint32_t i = 0; i < count; )
    {
        uint32_t p = addr + i * 4;
        if ( !plausible( p, count - i ) )
        {
            uint32_t was = guest_read32( p );
            auto start = std::chrono::steady_clock::now();
            bool ok = false;
            while ( !( ok = plausible( p, count - i ) ) && std::chrono::steady_clock::now() - start < std::chrono::milliseconds( 100 ) )
                std::this_thread::sleep_for( std::chrono::microseconds( 200 ) );
            static int reports;
            if ( reports++ < 20 )
                rt_log( "gpu: no packet at %08X (%08X %08X %08X), dword %u of %u in buffer %08X (depth %d, ring %X, wptr %X): %s after %lld us (now %08X)\n", p, was,
                        guest_read32( p + 4 ), guest_read32( p + 8 ), i, count, addr, depth, g_ring_pos, g_wptr, ok ? "became one" : "skipped the rest",
                        (long long)std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::steady_clock::now() - start ).count(), guest_read32( p ) );
            if ( !ok ) return;
        }
        i += packet( p, depth );
    }
}


// Native submission (CRACKDOWN_NATIVE_D3D=1). When D3D hands the GPU new commands (the write pointer),
// the thread that wrote them decodes them on the spot into native operations for the renderer, indirect
// buffers and the precompiled command buffers the game replays included: shader programs are found
// and constants read now, predication is settled now. The renderer thread runs the operations; nothing
// reads the commands from guest memory later, so memory the game rewrites after submitting (a command
// buffer it rebuilds) can't change or break what was submitted. The ring read pointer advances when the
// renderer has run the work, as D3D reuses that memory (draws keep vertices in it) once it has.
constexpr uint32_t CONSTANT_BASES[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };  // ALU, fetch, bool, loop, registers
std::mutex g_ops_lock;
std::condition_variable g_ops_cv;
std::vector<uint32_t> g_ops;  // decoded, for the renderer thread
uint32_t g_decoded;           // ring index decoded up to

void op( std::vector<uint32_t> &o, uint32_t kind, uint32_t words ) { op_begin( o, kind, words ); }

// The native device's batches (gpu_ops.h), in stream order.
std::mutex g_batch_lock;
std::deque<std::shared_ptr<NativeBatch>> g_batches;
uint32_t g_batch_seq;

// A batch's marker reached: its operations go where the marker is.
void splice_batch( std::vector<uint32_t> &o, uint32_t seq, uint32_t mask = ~0u )
{
    std::shared_ptr<NativeBatch> b;
    {
        // Batches come in stream order (the front one), but a stream that lost a marker leaves its batch
        // behind; a later one is found from the back.
        std::lock_guard<std::mutex> l( g_batch_lock );
        auto it = g_batches.begin();
        if ( it != g_batches.end() && ( ( *it )->seq & mask ) != seq )  // from the back: the newest
        {
            auto r = std::find_if( g_batches.rbegin(), g_batches.rend(), [seq, mask]( auto &x ) { return ( x->seq & mask ) == seq; } );
            it = r == g_batches.rend() ? g_batches.end() : std::prev( r.base() );
        }
        if ( it == g_batches.end() )
        {
            static int k;
            if ( k++ < 10 ) rt_log( "gpu: native batch marker %u has no batch\n", seq );
            return;
        }
        if ( g_batches.size() > 100000 )
        {
            static int k;
            if ( k++ < 3 ) rt_log( "gpu: %zu native batches waiting (markers lost?)\n", g_batches.size() );
        }
        b = std::move( *it );
        g_batches.erase( it );
        b->taken = true;
    }
    o.insert( o.end(), b->ops.begin(), b->ops.end() );
}

void decode_buffer( std::vector<uint32_t> &o, uint32_t addr, uint32_t count, int depth );
void decode_type3( std::vector<uint32_t> &o, uint32_t h, uint32_t n, uint32_t code, uint32_t a, int depth );

// n register writes from base, the values at guest address src. The game's precompiled command
// buffers (from its data files) were recorded for its two-tile frame: their window scissor stops at
// the tile's 484 rows, which the console shifted per tile; drawn as one tile it covers the screen.
// Their materials also read the deferred-lighting buffer at the pixel's position scaled by 1/484 (a
// pixel shader constant): its height when it held one tile. Drawn as one tile it holds 720 rows.
void regs( std::vector<uint32_t> &o, uint32_t base, uint32_t src, uint32_t n )
{
    op( o, OP_REGS, n + 1 );
    o.push_back( base );
    for ( uint32_t i = 0; i < n; ++i )
    {
        uint32_t v = guest_read32( src + i * 4 );
        if ( base + i == 0x2082 && v == 0x01E40500 ) v = 0x02D00500;  // PA_SC_WINDOW_SCISSOR_BR 1280x484 -> 1280x720
        if ( base + i >= 0x4000 && base + i < 0x4800 && v == 0x3B0767AB && native_d3d() ) v = 0x3AB60B61;  // 1/484 -> 1/720
        o.push_back( v );
    }
}

// CRACKDOWN_STREAM_STATS=1: what the command stream still carries, every 10 s (words by register block
// for register writes, by opcode for commands; depth > 0 counts indirect buffers' contents too).
std::atomic<uint64_t> g_stream_words[4][256];  // registers by block (reg >> 8), commands by opcode; +2: replayed buffers (depth 2+)
bool stream_stats() { static const bool on = RT_ENV( "CRACKDOWN_STREAM_STATS" ); return on; }
void stream_stats_report()
{
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if ( now - last < std::chrono::seconds( 10 ) ) return;
    last = now;
    std::string line = "gpu: stream words:";
    for ( int k = 0; k < 4; ++k )
        for ( int i = 0; i < 256; ++i )
            if ( uint64_t n = g_stream_words[k][i].exchange( 0 ) )
            {
                char b[40];
                snprintf( b, sizeof( b ), " %s%s%02X:%llu", k & 2 ? "cb:" : "", k & 1 ? "op" : "r", i, (unsigned long long)n );
                line += b;
            }
    rt_log( "%s\n", line.c_str() );
}

// Decodes the packet at p; returns its length in dwords.
uint32_t decode( std::vector<uint32_t> &o, uint32_t p, int depth )
{
    uint32_t h = guest_read32( p );
    if ( stream_stats() )
    {
        uint32_t len = ( h >> 30 ) == 1 ? 3 : ( h >> 30 ) == 2 ? 1 : ( ( h >> 16 ) & 0x3FFF ) + 2;
        int k = depth >= 2 ? 2 : 0;
        if ( h >> 30 == 3 ) g_stream_words[k + 1][( h >> 8 ) & 0x7F] += len;
        else g_stream_words[k][h >> 30 ? 0 : ( h >> 8 ) & 0x7F] += len;
    }
    switch ( h >> 30 )
    {
    case 0:
    {
        uint32_t n = ( ( h >> 16 ) & 0x3FFF ) + 1;
        if ( h & 0x8000 )
        {
            op( o, OP_REGS_SAME, n + 1 ), o.push_back( h & 0x7FFF );
            for ( uint32_t i = 0; i < n; ++i ) o.push_back( guest_read32( p + 4 + i * 4 ) );
        }
        else regs( o, h & 0x7FFF, p + 4, n );
        return n + 1;
    }
    case 1:
        regs( o, h & 0x7FF, p + 4, 1 );
        regs( o, ( h >> 11 ) & 0x7FF, p + 8, 1 );
        return 3;
    case 2: return 1;
    }
    uint32_t n = ( ( h >> 16 ) & 0x3FFF ) + 1, code = ( h >> 8 ) & 0x7F, a = p + 4;
    if ( h & 1 )  // predicated: decided when it runs, by the bin state then
    {
        size_t at = o.size();
        op( o, OP_PRED, 1 ), o.push_back( 0 );
        decode_type3( o, h, n, code, a, depth );
        o[at + 1] = (uint32_t)( o.size() - at - 2 );
        return n + 1;
    }
    decode_type3( o, h, n, code, a, depth );
    return n + 1;
}

thread_local bool t_template;     // translating a command buffer to cache (gpu_run_buffer)
thread_local bool t_uncacheable;  // its translation can't be kept (it runs other buffers)

void decode_type3( std::vector<uint32_t> &o, uint32_t h, uint32_t n, uint32_t code, uint32_t a, int depth )
{
    auto arg = [a]( uint32_t i ) { return guest_read32( a + i * 4 ); };
    if ( t_template )
        switch ( code )
        {
        case 0x10: case 0x3F: t_uncacheable = true; break;
        case 0x27:
            op( o, OP_LATE_SHADER, 3 ), o.push_back( arg( 0 ) & 3 ), o.push_back( phys( arg( 0 ) & ~3u ) ), o.push_back( arg( 1 ) & 0xFFFF );
            return;
        case 0x2F:
        {
            uint32_t d1 = arg( 1 ), size = arg( 2 ) & 0xFFF, type = ( d1 >> 16 ) & 0xFF;
            if ( type < 5 && size ) op( o, OP_LATE_REGS, 3 ), o.push_back( CONSTANT_BASES[type] + ( d1 & 0x7FF ) ), o.push_back( phys( arg( 0 ) & ~3u ) ), o.push_back( size );
            return;
        }
        }
    auto shader = [&o]( const void *s ) {
        op( o, OP_SHADER, 2 ), o.push_back( (uint32_t)(uintptr_t)s ), o.push_back( (uint32_t)( (uintptr_t)s >> 32 ) );
    };
    switch ( code )
    {
    case 0x10:  // NOP: a native batch's marker
        if ( n >= 2 && arg( 0 ) == NATIVE_MARKER ) splice_batch( o, arg( 1 ) );
        else if ( n >= 3 && arg( 1 ) == NATIVE_MARKER_MASK ) op( o, OP_BIN, 2 ), o.push_back( 0 ), o.push_back( arg( 0 ) ), splice_batch( o, arg( 2 ) );
        else if ( n == 1 && ( arg( 0 ) & 0xFF000000 ) == NATIVE_MARKER_SHORT ) splice_batch( o, arg( 0 ) & 0xFFFFFF, 0xFFFFFF );
        break;
    case 0x60: case 0x61: case 0x62: case 0x63: op( o, OP_BIN, 2 ), o.push_back( code - 0x60 ), o.push_back( arg( 0 ) ); break;
    case 0x3F:  // INDIRECT_BUFFER
        if ( depth < 4 ) g_ib_base[depth + 1] = phys( arg( 0 ) ), decode_buffer( o, phys( arg( 0 ) ), arg( 1 ) & 0xFFFFF, depth + 1 );
        break;
    case 0x27: shader( xenos_shader_find( arg( 0 ) & 3, phys( arg( 0 ) & ~3u ), arg( 1 ) & 0xFFFF ) ); break;  // IM_LOAD
    case 0x2B: shader( xenos_shader_find( arg( 0 ) & 3, a + 8, arg( 1 ) & 0xFFFF ) ); break;                  // IM_LOAD_IMMEDIATE
    case 0x2D:  // SET_CONSTANT: (type << 16 | index), values
    {
        uint32_t type = ( arg( 0 ) >> 16 ) & 0xFF;
        if ( type < 5 && n > 1 ) regs( o, CONSTANT_BASES[type] + ( arg( 0 ) & 0x7FF ), a + 4, n - 1 );
        break;
    }
    case 0x55:  // SET_CONSTANT2: register index, values
    case 0x56:  // SET_SHADER_CONSTANTS
        if ( n > 1 ) regs( o, arg( 0 ) & 0xFFFF, a + 4, n - 1 );
        break;
    case 0x2F:  // LOAD_ALU_CONSTANT: the constants as they are now
    {
        uint32_t d1 = arg( 1 ), size = arg( 2 ) & 0xFFF, type = ( d1 >> 16 ) & 0xFF;
        if ( type < 5 && size ) regs( o, CONSTANT_BASES[type] + ( d1 & 0x7FF ), phys( arg( 0 ) & ~3u ), size );
        break;
    }
    default:
        op( o, OP_TYPE3, n + 2 );
        o.push_back( h ), o.push_back( a );
        for ( uint32_t i = 0; i < n; ++i ) o.push_back( arg( i ) );
        break;
    }
}

void decode_buffer( std::vector<uint32_t> &o, uint32_t addr, uint32_t count, int depth )
{
    for ( uint32_t i = 0; i < count; )
    {
        uint32_t p = addr + i * 4;
        if ( !plausible( p, count - i ) )  // decoded as submitted: no later writes will make it one
        {
            static int reports;
            if ( reports++ < 20 )
                rt_log( "gpu: no packet at %08X (%08X %08X %08X), dword %u of %u in buffer %08X (depth %d): skipped the rest\n", p, guest_read32( p ),
                        guest_read32( p + 4 ), guest_read32( p + 8 ), i, count, addr, depth );
            return;
        }
        i += decode( o, p, depth );
    }
}

// Runs decoded operations (renderer thread).
void run_ops( const std::vector<uint32_t> &o )
{
    for ( size_t i = 0; i < o.size(); )
    {
        uint32_t kind = o[i] >> 24, words = o[i] & 0xFFFFFF;
        const uint32_t *w = &o[i + 1];
        switch ( kind )
        {
        case OP_REGS: if ( words > 1 ) write_regs( w[0], w + 1, words - 1 ); break;
        case OP_REGS_SAME: for ( uint32_t k = 1; k < words; ++k ) write_reg( w[0], w[k] ); break;
        case OP_SHADER: xenos_shader_bind( (const void *)( (uintptr_t)w[0] | (uintptr_t)w[1] << 32 ) ); break;
        case OP_TYPE3: type3( w[0], w + 2, w[1], 1 ); break;
        case OP_DRAW: xenos_draw( w[0], w[1], w[2], g_regs ); break;
        case OP_BIN:
        {
            uint64_t &v = w[0] & 2 ? g_bin_select : g_bin_mask;
            v = w[0] & 1 ? ( v & 0xFFFFFFFFull ) | (uint64_t)w[1] << 32 : ( v & ~0xFFFFFFFFull ) | w[1];
            break;
        }
        case OP_PRED:
            if ( !( g_bin_mask & g_bin_select ) ) i += w[0];
            break;
        case OP_RMW: write_reg( w[0], ( g_regs[w[0] & 0xFFFF] & w[1] ) | w[2] ); break;
        case OP_FENCE: event_write( w[0], w[1] ); break;
        case OP_PRESENT: present( w ); break;
        case OP_WAIT: wait_reg_mem( w[0], w[1], w[2], w[3] ); break;
        case OP_INTERRUPT: interrupt( w[0], 0, 0, 0 ); break;
        case OP_CLEAR: renderer_clear( g_regs, w ); break;
        case OP_ZPD: renderer_zpd( phys( g_regs[0x2325] ) - 0xA0000000 ); break;
        case OP_EXTENTS:
        {
            const uint16_t extents[4] = { 0, 8191 >> 3, 0, 8191 >> 3 };
            for ( int k = 0; k < 4; ++k ) guest_write16( phys( w[0] ) + k * 2, extents[k] );
            break;
        }
        case OP_RPTR:
            g_rb_rptr = w[0];
            if ( g_rptr_writeback ) guest_write32( g_rptr_writeback, w[0] );
            break;
        }
        i += 1 + words;
    }
}

// The renderer thread under native submission: runs operations as they come; out of them, finishes
// GPU work as cp_thread does.
void native_thread()
{
    t_cp = true;
    std::vector<uint32_t> batch;
    for ( ;; )
    {
        {
            std::unique_lock<std::mutex> lock( g_ops_lock );
            auto idle = std::chrono::steady_clock::now();
            while ( g_ops.empty() )
            {
                lock.unlock();
                bool pending = renderer_poll( std::chrono::steady_clock::now() - idle > std::chrono::milliseconds( 1 ) );
                lock.lock();
                if ( !g_ops.empty() ) break;
                if ( pending ) g_ops_cv.wait_for( lock, std::chrono::microseconds( 250 ) );
                else g_ops_cv.wait( lock, [] { return !g_ops.empty(); } );
            }
            batch.swap( g_ops );
        }
        run_ops( batch );
        batch.clear();
    }
}

// CRACKDOWN_CP=1 keeps the command processor thread under the native device (comparisons).
bool native_submission() { static const bool on = native_d3d() && !RT_ENV( "CRACKDOWN_CP" ); return on; }

} // namespace

std::shared_ptr<NativeBatch> gpu_batch_new()
{
    auto b = std::make_shared<NativeBatch>();
    std::lock_guard<std::mutex> l( g_batch_lock );
    b->seq = ++g_batch_seq;
    g_batches.push_back( b );
    return b;
}

namespace
{
struct BufferTranslation
{
    uint64_t hash;
    std::vector<uint32_t> ops;
};
std::mutex g_translations_lock;
std::unordered_map<uint64_t, BufferTranslation> g_translations;  // by address << 32 | dwords
} // namespace

void gpu_run_buffer( std::vector<uint32_t> &o, uint32_t addr, uint32_t dwords )
{
    uint64_t hash = XXH3_64bits( g_base + addr, dwords * 4 ), key = (uint64_t)addr << 32 | dwords;
    std::unique_lock<std::mutex> l( g_translations_lock );
    BufferTranslation &t = g_translations[key];
    if ( t.ops.empty() || t.hash != hash )
    {
        t.ops.clear();
        t_template = true, t_uncacheable = false;
        decode_buffer( t.ops, addr, dwords, 2 );
        t_template = false;
        t.hash = hash;
        if ( t_uncacheable )  // decoded as it is, every time
        {
            t.ops.clear();
            l.unlock();
            decode_buffer( o, addr, dwords, 2 );
            return;
        }
    }
    o.reserve( o.size() + t.ops.size() );
    std::vector<std::pair<size_t, size_t>> preds;  // predicated runs being copied: (count word in o, end in t.ops)
    for ( size_t i = 0; i < t.ops.size(); )
    {
        while ( !preds.empty() && i >= preds.back().second ) o[preds.back().first] = (uint32_t)( o.size() - preds.back().first - 1 ), preds.pop_back();
        uint32_t kind = t.ops[i] >> 24, words = t.ops[i] & 0xFFFFFF;
        const uint32_t *w = &t.ops[i + 1];
        if ( kind == OP_PRED ) op( o, OP_PRED, 1 ), o.push_back( 0 ), preds.push_back( { o.size() - 1, i + 2 + w[0] } );
        else if ( kind == OP_LATE_REGS ) regs( o, w[0], w[1], w[2] );
        else if ( kind == OP_LATE_SHADER )
        {
            const void *s = xenos_shader_find( w[0], w[1], w[2] );
            op( o, OP_SHADER, 2 ), o.push_back( (uint32_t)(uintptr_t)s ), o.push_back( (uint32_t)( (uintptr_t)s >> 32 ) );
        }
        else o.insert( o.end(), t.ops.begin() + i, t.ops.begin() + i + 1 + words );
        i += 1 + words;
    }
    for ( ; !preds.empty(); preds.pop_back() ) o[preds.back().first] = (uint32_t)( o.size() - preds.back().first - 1 );
}

bool gpu_batch_add( NativeBatch &b, const std::vector<uint32_t> &ops )
{
    std::lock_guard<std::mutex> l( g_batch_lock );
    if ( b.taken ) return false;
    b.ops.insert( b.ops.end(), ops.begin(), ops.end() );
    return true;
}

void op_regs_from( std::vector<uint32_t> &o, uint32_t base, uint32_t src, uint32_t n )
{
    op_begin( o, OP_REGS, n + 1 );
    o.push_back( base );
    for ( uint32_t i = 0; i < n; ++i ) o.push_back( guest_read32( src + i * 4 ) );
}

namespace
{
// Decodes the ring from where decoding got to up to wptr into operations (g_cp_lock held), with the
// read pointer moving there after them.
void decode_ring( std::vector<uint32_t> &o, uint32_t wptr )
{
    if ( !g_rb_base || g_decoded == wptr ) return;
    static uint32_t staging = rt_alloc_physical( 0x10000, 0x1000 );
    uint32_t size = g_rb_size_dwords, r = g_decoded;
    for ( int guard = 0; r != wptr && guard < 1 << 20; ++guard )
    {
        uint32_t h = guest_read32( g_rb_base + r * 4 ), len;
        switch ( h >> 30 )
        {
        case 1: len = 3; break;
        case 2: len = 1; break;
        default: len = ( ( h >> 16 ) & 0x3FFF ) + 2; break;
        }
        if ( r + len <= size ) decode( o, g_rb_base + r * 4, 0 );
        else
        {
            for ( uint32_t i = 0; i < len && i < 0x4000; ++i ) guest_write32( staging + i * 4, guest_read32( g_rb_base + ( ( r + i ) % size ) * 4 ) );
            decode( o, staging, 0 );
        }
        r = ( r + len ) % size;
    }
    g_decoded = wptr;
    op( o, OP_RPTR, 1 ), o.push_back( wptr );
}

// Decodes the ring up to wptr (the submitting thread).
void native_submit( uint32_t wptr )
{
    std::vector<uint32_t> o;
    {
        std::lock_guard<std::mutex> g( g_cp_lock );
        decode_ring( o, wptr );
        if ( stream_stats() ) stream_stats_report();
    }
    {
        std::lock_guard<std::mutex> l( g_ops_lock );
        g_ops.insert( g_ops.end(), o.begin(), o.end() );
    }
    g_ops_cv.notify_one();
}
} // namespace

bool gpu_native_submission() { return native_submission(); }

void start_gpu_thread();

// The native device's submissions, without the ring (native_d3d.cpp 827440F0, 82756B80): in the order
// D3D makes them, a command segment decoded now (as its INDIRECT_BUFFER in the ring was), or
// operations. Neither goes through the ring; what D3D still writes there itself (only its start-up
// packets) is decoded before the next segment, up to D3D's position in the ring.
namespace
{
void submit( std::vector<uint32_t> &o )
{
    start_gpu_thread();
    ++g_stats.wptr_writes;
    if ( stream_stats() ) stream_stats_report();
    {
        std::lock_guard<std::mutex> l( g_ops_lock );
        g_ops.insert( g_ops.end(), o.begin(), o.end() );
    }
    g_ops_cv.notify_one();
}
} // namespace

void gpu_submit_segment( uint32_t gpu_addr, uint32_t dwords, uint32_t ring_position )
{
    std::vector<uint32_t> o;
    {
        std::lock_guard<std::mutex> g( g_cp_lock );
        decode_ring( o, ring_position );  // what D3D itself wrote in the ring first (its start-up packets)
        g_ib_base[1] = phys( gpu_addr );
        decode_buffer( o, phys( gpu_addr ), dwords & 0xFFFFF, 1 );
        submit( o );
    }
}

void gpu_submit_ops( const std::vector<uint32_t> &ops )
{
    std::vector<uint32_t> o( ops );
    std::lock_guard<std::mutex> g( g_cp_lock );
    submit( o );
}

// VdInitializeRingBuffer(physical base, log2 size): the ring holds 2^(log2 + 1) dwords.
RT_IMPORT( VdInitializeRingBuffer )
{
    std::lock_guard<std::mutex> g( g_cp_lock );
    g_rb_base = phys( RT_ARG( 0 ) );
    g_rb_size_dwords = 1u << ( RT_ARG( 1 ) + 1 );
    g_rb_rptr = 0;
    g_decoded = 0;
    {
        std::lock_guard<std::mutex> w( g_wptr_lock );
        g_wptr = 0;
    }
    g_log_budget = RT_ENV( "CRACKDOWN_GPU_PACKETS" ) ? 400 : 120;
    rt_log( "gpu: ring buffer at %08X, log2 %u\n", g_rb_base, RT_ARG( 1 ) );
    RT_RETURN( 0 );
}

// VdEnableRingBufferRPtrWriteBack(physical address, log2 block size)
RT_IMPORT( VdEnableRingBufferRPtrWriteBack )
{
    g_rptr_writeback = phys( RT_ARG( 0 ) );
    rt_log( "gpu: read pointer write-back at %08X\n", g_rptr_writeback );
    RT_RETURN( 0 );
}

// VdSetGraphicsInterruptCallback(callback, context)
RT_IMPORT( VdSetGraphicsInterruptCallback )
{
    g_int_callback = RT_ARG( 0 );
    g_int_context = RT_ARG( 1 );
    static std::once_flag once;
    std::call_once( once, [] {
        for ( int cpu = 0; cpu < 6; ++cpu )
        {
            g_irq_thread[cpu] = rt_guest_thread_create( "gpu-irq" );
            g_base[g_irq_thread[cpu]->pcr + 0x10C] = (uint8_t)cpu;
        }
        std::thread( [] {
            GuestThread *vblank = rt_guest_thread_create( "vblank" );
            g_base[vblank->pcr + 0x10C] = 0;
            auto next = std::chrono::steady_clock::now();
            for ( ;; )
            {
                next += std::chrono::microseconds( 16667 );
                std::this_thread::sleep_until( next );
                raise_interrupt( vblank, 0 );
            }
        } ).detach();
    } );
    RT_RETURN( 0 );
}

// CPU writes to GPU registers through MMIO (0x7FC80000 + 4 * register). Lock-free: interrupt
// callbacks running on the command-processor thread write registers too.
void gpu_mmio_write( uint32_t reg, uint32_t value ) { write_reg( reg, value ); }
uint32_t gpu_mmio_read( uint32_t reg ) { return g_regs[reg & 0xFFFF]; }

namespace
{

// Runs the ring from the read pointer up to `wptr`. Packets may straddle the end of the ring:
// one that does is copied, wrapped, into a staging buffer and run from there.
void cp_run_to( uint32_t wptr )
{
    std::lock_guard<std::mutex> g( g_cp_lock );
    if ( !g_rb_base ) return;
    static uint32_t staging = rt_alloc_physical( 0x10000, 0x1000 );
    uint32_t size = g_rb_size_dwords, r = g_rb_rptr;
    for ( int guard = 0; r != wptr && guard < 1 << 20; ++guard )
    {
        uint32_t h = guest_read32( g_rb_base + r * 4 ), len;
        switch ( h >> 30 )
        {
        case 1: len = 3; break;
        case 2: len = 1; break;
        default: len = ( ( h >> 16 ) & 0x3FFF ) + 2; break;
        }
        g_ring_pos = r;
        if ( r + len <= size ) packet( g_rb_base + r * 4, 0 );
        else
        {
            for ( uint32_t i = 0; i < len && i < 0x4000; ++i ) guest_write32( staging + i * 4, guest_read32( g_rb_base + ( ( r + i ) % size ) * 4 ) );
            packet( staging, 0 );
        }
        r = ( r + len ) % size;
        g_rb_rptr = r;
        if ( g_rptr_writeback ) guest_write32( g_rptr_writeback, r );
    }
    g_rb_rptr = wptr;
    if ( g_rptr_writeback ) guest_write32( g_rptr_writeback, wptr );
}

void cp_thread()
{
    t_cp = true;
    uint64_t seen = 0;
    for ( ;; )
    {
        uint32_t wptr;
        {
            std::unique_lock<std::mutex> lock( g_wptr_lock );
            // Out of packets: finish what the GPU completes (polled every 0.25 ms while work is pending)
            // and, once idle for 1 ms, submit recorded work with effects behind it (the game may be
            // waiting for them; until then more packets may join it).
            auto idle = std::chrono::steady_clock::now();
            while ( g_wptr_seq == seen )
            {
                lock.unlock();
                bool pending;
                {
                    std::lock_guard<std::mutex> g( g_cp_lock );
                    pending = renderer_poll( std::chrono::steady_clock::now() - idle > std::chrono::milliseconds( 1 ) );
                }
                lock.lock();
                if ( g_wptr_seq != seen ) break;
                if ( pending ) g_wptr_cv.wait_for( lock, std::chrono::microseconds( 250 ) );
                else g_wptr_cv.wait( lock, [&] { return g_wptr_seq != seen; } );
            }
            seen = g_wptr_seq;
            wptr = g_wptr;
        }
        cp_run_to( wptr );
    }
}
} // namespace

// The CPU advanced the write pointer (CP_RB_WPTR, register 0x1C5): hand it to the CP thread.
// The thread that runs the GPU work (the renderer's), at the first submission.
void start_gpu_thread()
{
    static std::once_flag started;
    std::call_once( started, [] { std::thread( native_submission() ? native_thread : cp_thread ).detach(); } );
}

void gpu_write_wptr( uint32_t wptr )
{
    start_gpu_thread();
    ++g_stats.wptr_writes;
    if ( RT_ENV( "CRACKDOWN_GPU_LOG" ) && g_log_budget-- > 0 ) rt_log( "gpu: wptr %X (rptr %X)\n", wptr, g_rb_rptr );
    if ( native_submission() )
    {
        g_wptr = wptr;
        native_submit( wptr );
        return;
    }
    {
        std::lock_guard<std::mutex> g( g_wptr_lock );
        g_wptr = wptr;
        ++g_wptr_seq;
    }
    g_wptr_cv.notify_one();
}

uint64_t gpu_swap_count() { return g_swaps; }
