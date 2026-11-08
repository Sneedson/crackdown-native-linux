#include <functional>
#include <algorithm>
#include <tuple>
#include <map>
// Events, mutants, semaphores, threads and the waits on them (Nt* handle-based forms).
// One process-wide lock and condition variable guard all object state: simple, and correct
// for multi-object waits; per-object locks can come later if profiling asks for them.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "objects.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string>
#include <vector>

namespace
{

std::mutex g_lock;
std::condition_variable g_cv;
std::unordered_map<uint32_t, Object *> g_handles;
uint32_t g_next_handle = 0x1000;

constexpr uint32_t STATUS_SUCCESS = 0, STATUS_TIMEOUT = 0x102, STATUS_ABANDONED_WAIT_0 = 0x80,
                   STATUS_INVALID_HANDLE = 0xC0000008, STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t CURRENT_THREAD = 0xFFFFFFFE, CURRENT_PROCESS = 0xFFFFFFFF;

// Setting an event releases the threads already waiting on it even if it is reset right after
// (the kernel hands the wake-up to the waiters at set time). `pulses` records sets for manual-reset
// events; `grants` counts wake-ups owed to waiters of auto-reset ones.
struct EventObject : Object
{
    bool manual = false, signaled = false;
    unsigned waiters = 0, grants = 0;
    uint64_t pulses = 0;
};
void event_signal_locked( EventObject *e, bool pulse_only )
{
    if ( e->manual ) { ++e->pulses; if ( !pulse_only ) e->signaled = true; return; }
    if ( e->waiters > e->grants ) ++e->grants;
    else if ( !pulse_only ) e->signaled = true;
}
struct MutantObject : Object { uint32_t owner = 0; unsigned count = 0; };
struct SemaphoreObject : Object { int32_t count = 0, max = 0; };
struct ThreadObject : Object
{
    bool finished = false, suspended = false;
    uint32_t entry = 0, arg1 = 0, arg2 = 0, startup = 0;
    uint32_t thread_id = 0;
    uint32_t kthread = 0;  // guest KTHREAD stand-in (r13+256 of the thread, ObReferenceObjectByHandle)
    int processor = -1;    // requested hardware thread, applied when the thread starts
    bool started = false;
};

thread_local uint32_t t_thread_handle;  // this host thread's guest thread handle

uint32_t insert_locked( Object *o )
{
    uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = o;
    return h;
}

Object *get_locked( uint32_t h )
{
    if ( h == CURRENT_THREAD ) h = t_thread_handle;
    auto it = g_handles.find( h );
    return it == g_handles.end() ? nullptr : it->second;
}

bool is_signaled_locked( Object *o )
{
    switch ( o->type )
    {
    case ObjType::Event: return static_cast<EventObject *>( o )->signaled;
    case ObjType::Mutant: { auto *m = static_cast<MutantObject *>( o ); return !m->count || m->owner == t_thread_handle; }
    case ObjType::Semaphore: return static_cast<SemaphoreObject *>( o )->count > 0;
    case ObjType::Thread: return static_cast<ThreadObject *>( o )->finished;
    case ObjType::File: return true;
    }
    return false;
}

// What satisfying a wait does to the object (auto-reset, ownership, count).
void acquire_locked( Object *o )
{
    switch ( o->type )
    {
    case ObjType::Event: { auto *e = static_cast<EventObject *>( o ); if ( !e->manual ) { if ( e->grants ) --e->grants; else e->signaled = false; } break; }
    case ObjType::Mutant: { auto *m = static_cast<MutantObject *>( o ); m->owner = t_thread_handle; ++m->count; break; }
    case ObjType::Semaphore: --static_cast<SemaphoreObject *>( o )->count; break;
    default: break;
    }
}

// Guest timeout: NULL = infinite; negative 100 ns units = relative; positive = absolute (treated as relative).
bool deadline_from( uint32_t ptimeout, std::chrono::steady_clock::time_point &deadline )
{
    if ( !ptimeout ) return false;
    int64_t t = (int64_t)( ( (uint64_t)guest_read32( ptimeout ) << 32 ) | guest_read32( ptimeout + 4 ) );
    if ( t < 0 ) t = -t;
    deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds( t * 100 );
    return true;
}

// Infinite waits report themselves once if they last 5 s: the usual sign of a missing device.
void wait_forever( std::unique_lock<std::mutex> &lock, bool &reported, std::chrono::steady_clock::time_point since,
                   const char *what, const uint32_t *list, size_t count )
{
    g_cv.wait_for( lock, std::chrono::seconds( 1 ) );
    if ( !reported && std::chrono::steady_clock::now() - since > std::chrono::seconds( 5 ) )
    {
        reported = true;
        uint32_t first = list[0];
        const char *type = "?";
        if ( !strcmp( what, "Nt" ) )
            if ( Object *o = get_locked( first ) )
                type = o->type == ObjType::Event ? "event" : o->type == ObjType::Mutant ? "mutant" : o->type == ObjType::Semaphore ? "semaphore" : o->type == ObjType::Thread ? "thread" : "file";
        if ( !strcmp( what, "Ke" ) ) type = g_base[first] == 0 ? "notify-event" : g_base[first] == 1 ? "sync-event" : g_base[first] == 2 ? "mutant" : g_base[first] == 5 ? "semaphore" : "ke?";
        char bt[256];
        rt_guest_backtrace( bt, sizeof(bt) );
        std::string all;
        for ( size_t i = 1; i < count; ++i ) { char b[12]; snprintf( b, sizeof( b ), " %08X", list[i] ); all += b; }
        rt_log( "stuck: %s wait on %08X (%s), %zu objects, guest %s%s%s\n", what, first, type, count, bt, count > 1 ? "; also" : "", all.c_str() );
        if ( getenv( "CRACKDOWN_TRACE_STUCK" ) ) rt_dump_trace( "stuck" );
    }
}

// Waits on `handles`; any = true: the first signaled one; any = false: all of them.
uint32_t wait_objects( const std::vector<uint32_t> &handles, bool any, uint32_t ptimeout )
{
    std::chrono::steady_clock::time_point deadline;
    bool timed = deadline_from( ptimeout, deadline );
    std::unique_lock<std::mutex> lock( g_lock );
    bool reported = false;
    auto since = std::chrono::steady_clock::now();
    std::vector<Object *> objs;
    for ( uint32_t h : handles )
    {
        Object *o = get_locked( h );
        if ( !o ) return STATUS_INVALID_HANDLE;
        objs.push_back( o );
    }
    std::vector<uint64_t> start( objs.size() );
    for ( size_t i = 0; i < objs.size(); ++i )
        if ( objs[i]->type == ObjType::Event )
        {
            auto *e = static_cast<EventObject *>( objs[i] );
            ++e->waiters;
            start[i] = e->pulses;
        }
    auto ready = [&]( size_t i ) {
        if ( objs[i]->type != ObjType::Event ) return is_signaled_locked( objs[i] );
        auto *e = static_cast<EventObject *>( objs[i] );
        return e->signaled || ( e->manual ? e->pulses != start[i] : e->grants > 0 );
    };
    auto finish = [&]( uint32_t status ) {
        for ( Object *o : objs )
            if ( o->type == ObjType::Event ) --static_cast<EventObject *>( o )->waiters;
        return status;
    };
    for ( ;; )
    {
        if ( any )
        {
            for ( size_t i = 0; i < objs.size(); ++i )
                if ( ready( i ) ) { acquire_locked( objs[i] ); return finish( STATUS_SUCCESS + (uint32_t)i ); }
        }
        else
        {
            bool all = true;
            for ( size_t i = 0; i < objs.size(); ++i ) all = all && ready( i );
            if ( all ) { for ( Object *o : objs ) acquire_locked( o ); return finish( STATUS_SUCCESS ); }
        }
        if ( timed )
        {
            if ( g_cv.wait_until( lock, deadline ) == std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline )
                return finish( STATUS_TIMEOUT );
        }
        else wait_forever( lock, reported, since, "Nt", handles.data(), handles.size() );
    }
}

void write_handle( uint32_t p, uint32_t h ) { if ( p ) guest_write32( p, h ); }

} // namespace

uint32_t obj_insert( Object *o ) { std::lock_guard<std::mutex> g( g_lock ); return insert_locked( o ); }
Object *obj_get( uint32_t h ) { std::lock_guard<std::mutex> g( g_lock ); return get_locked( h ); }
bool obj_close( uint32_t h )
{
    std::lock_guard<std::mutex> g( g_lock );
    auto it = g_handles.find( h );
    if ( it == g_handles.end() ) return false;
    // Threads stay alive (their host thread still references them); others are freed.
    if ( it->second->type != ObjType::Thread ) delete it->second;
    g_handles.erase( it );
    return true;
}
void sync_signal_all() { g_cv.notify_all(); }
void sync_set_event( uint32_t h )
{
    {
        std::lock_guard<std::mutex> g( g_lock );
        Object *o = get_locked( h );
        if ( o && o->type == ObjType::Event ) event_signal_locked( static_cast<EventObject *>( o ), false );
    }
    g_cv.notify_all();
}
void sync_reset_event( uint32_t h )
{
    std::lock_guard<std::mutex> g( g_lock );
    Object *o = get_locked( h );
    if ( o && o->type == ObjType::Event ) static_cast<EventObject *>( o )->signaled = false;
}
uint32_t sync_current_thread_handle() { return t_thread_handle; }
uint32_t sync_create_event( bool manual, bool signaled )
{
    auto *e = new EventObject;
    e->type = ObjType::Event;
    e->manual = manual;
    e->signaled = signaled;
    return obj_insert( e );
}

// ------------------------------------------------------------------ guest-facing handlers
#define ARG(i) ( (i) == 0 ? c.r3.u32 : (i) == 1 ? c.r4.u32 : (i) == 2 ? c.r5.u32 : (i) == 3 ? c.r6.u32 : \
                 (i) == 4 ? c.r7.u32 : (i) == 5 ? c.r8.u32 : (i) == 6 ? c.r9.u32 : c.r10.u32 )

// NtCreateEvent(PHANDLE, POBJECT_ATTRIBUTES, EVENT_TYPE (0 notification, 1 synchronization), BOOLEAN initial)
void imp_NtCreateEvent( PPCContext &c )
{
    auto *e = new EventObject;
    e->type = ObjType::Event;
    e->manual = ARG( 2 ) == 0;
    e->signaled = ARG( 3 ) != 0;
    uint32_t h = obj_insert( e );
    write_handle( ARG( 0 ), h );
    if ( getenv( "CRACKDOWN_TRACE_EVENTS" ) )
    {
        char bt[256];
        rt_guest_backtrace( bt, sizeof(bt) );
        rt_log( "event %04X created (%s): %s\n", h, e->manual ? "manual" : "auto", bt );
    }
    c.r3.u64 = STATUS_SUCCESS;
}

void event_set( PPCContext &c, int mode )  // 0 set, 1 clear, 2 pulse
{
    std::unique_lock<std::mutex> lock( g_lock );
    Object *o = get_locked( ARG( 0 ) );
    if ( !o || o->type != ObjType::Event ) { c.r3.u64 = STATUS_INVALID_HANDLE; return; }
    auto *e = static_cast<EventObject *>( o );
    // NtSetEvent/NtPulseEvent(handle, PLONG previous); NtClearEvent(handle) has no second argument.
    if ( mode != 1 && ARG( 1 ) ) guest_write32( ARG( 1 ), e->signaled );
    if ( mode == 1 ) e->signaled = false;
    else event_signal_locked( e, mode == 2 );
    lock.unlock();
    if ( mode != 1 ) g_cv.notify_all();
    c.r3.u64 = STATUS_SUCCESS;
}
void imp_NtSetEvent( PPCContext &c ) { event_set( c, 0 ); }
void imp_NtClearEvent( PPCContext &c ) { event_set( c, 1 ); }
void imp_NtPulseEvent( PPCContext &c ) { event_set( c, 2 ); }

// NtCreateMutant(PHANDLE, POBJECT_ATTRIBUTES, BOOLEAN initialOwner)
void imp_NtCreateMutant( PPCContext &c )
{
    auto *m = new MutantObject;
    m->type = ObjType::Mutant;
    if ( ARG( 2 ) ) { m->owner = t_thread_handle; m->count = 1; }
    write_handle( ARG( 0 ), obj_insert( m ) );
    c.r3.u64 = STATUS_SUCCESS;
}

// NtReleaseMutant(HANDLE, PLONG previousCount)
void imp_NtReleaseMutant( PPCContext &c )
{
    std::unique_lock<std::mutex> lock( g_lock );
    Object *o = get_locked( ARG( 0 ) );
    if ( !o || o->type != ObjType::Mutant ) { c.r3.u64 = STATUS_INVALID_HANDLE; return; }
    auto *m = static_cast<MutantObject *>( o );
    if ( ARG( 1 ) ) guest_write32( ARG( 1 ), m->count );
    if ( !m->count || m->owner != t_thread_handle ) { c.r3.u64 = STATUS_MUTANT_NOT_OWNED; return; }
    if ( !--m->count ) m->owner = 0;
    lock.unlock();
    g_cv.notify_all();
    c.r3.u64 = STATUS_SUCCESS;
}

// NtCreateSemaphore(PHANDLE, POBJECT_ATTRIBUTES, LONG initial, LONG maximum)
void imp_NtCreateSemaphore( PPCContext &c )
{
    auto *s = new SemaphoreObject;
    s->type = ObjType::Semaphore;
    s->count = (int32_t)ARG( 2 );
    s->max = (int32_t)ARG( 3 );
    write_handle( ARG( 0 ), obj_insert( s ) );
    c.r3.u64 = STATUS_SUCCESS;
}

// NtReleaseSemaphore(HANDLE, LONG count, PLONG previous)
void imp_NtReleaseSemaphore( PPCContext &c )
{
    std::unique_lock<std::mutex> lock( g_lock );
    Object *o = get_locked( ARG( 0 ) );
    if ( !o || o->type != ObjType::Semaphore ) { c.r3.u64 = STATUS_INVALID_HANDLE; return; }
    auto *s = static_cast<SemaphoreObject *>( o );
    if ( ARG( 2 ) ) guest_write32( ARG( 2 ), s->count );
    s->count += (int32_t)ARG( 1 );
    if ( s->count > s->max ) s->count = s->max;
    lock.unlock();
    g_cv.notify_all();
    c.r3.u64 = STATUS_SUCCESS;
}

// CRACKDOWN_WAIT_STATS=1 (perf): time each thread spends blocked, by the guest code that waited (its
// return address), every 5 s.
void wait_stat( uint32_t lr, const char *kind, double ms )
{
    static const bool on = getenv( "CRACKDOWN_WAIT_STATS" );
    if ( !on ) return;
    static std::mutex m;
    static std::map<std::tuple<size_t, uint32_t, const char *>, std::pair<double, uint32_t>> stats;
    static auto last = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l( m );
    auto &s = stats[{ std::hash<std::thread::id>()( std::this_thread::get_id() ) % 997, lr, kind }];
    s.first += ms, s.second++;
    auto now = std::chrono::steady_clock::now();
    if ( now - last < std::chrono::seconds( 5 ) ) return;
    last = now;
    std::vector<std::pair<double, std::string>> v;
    for ( auto &[k, x] : stats )
    {
        char b[160];
        snprintf( b, sizeof b, "thread %3zu lr %08X %s: %.0f ms in %u waits", std::get<0>( k ), std::get<1>( k ), std::get<2>( k ), x.first, x.second );
        v.push_back( { x.first, b } );
    }
    std::sort( v.rbegin(), v.rend() );
    for ( size_t i = 0; i < v.size() && i < 14; ++i ) rt_log( "waits: %s\n", v[i].second.c_str() );
    stats.clear();
}

// NtWaitForSingleObjectEx(HANDLE, KPROCESSOR_MODE, BOOLEAN alertable, PLARGE_INTEGER timeout)
void imp_NtWaitForSingleObjectEx( PPCContext &c )
{
    auto t0 = std::chrono::steady_clock::now();
    c.r3.u64 = wait_objects( { ARG( 0 ) }, true, ARG( 3 ) );
    // (keyed by the waiting wrapper's caller too: its saved return address one frame up the guest stack)
    uint32_t back = guest_read32( c.r1.u32 ), up = back ? guest_read32( back - 8 ) : 0;
    wait_stat( (uint32_t)c.lr ^ ( up << 1 ), up ? "NtWait(caller<<1 xor)" : "NtWait", std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
    if ( getenv( "CRACKDOWN_WAIT_STATS" ) )
    {
        static std::mutex m;
        static std::map<uint32_t, double> by_caller;
        static auto last = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> l( m );
        by_caller[up] += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
        if ( std::chrono::steady_clock::now() - last > std::chrono::seconds( 5 ) )
        {
            last = std::chrono::steady_clock::now();
            for ( auto &[k, v] : by_caller ) if ( v > 500 ) rt_log( "waits: NtWait caller %08X %.0f ms\n", k, v );
            by_caller.clear();
        }
    }
}

// NtWaitForMultipleObjectsEx(count, PHANDLE handles, WAIT_TYPE (0 all, 1 any), mode, alertable, timeout)
void imp_NtWaitForMultipleObjectsEx( PPCContext &c )
{
    std::vector<uint32_t> handles;
    for ( uint32_t i = 0; i < ARG( 0 ); ++i ) handles.push_back( guest_read32( ARG( 1 ) + i * 4 ) );
    c.r3.u64 = wait_objects( handles, ARG( 2 ) == 1, ARG( 5 ) );
}

// ------------------------------------------------------------------ threads
// ExCreateThread(PHANDLE, stackSize, PDWORD threadId, apiStartup, startAddress, parameter, flags)
// With an XAPI startup routine the thread runs apiStartup(startAddress, parameter).
void imp_ExCreateThread( PPCContext &c )
{
    static uint32_t next_id = 1;
    auto *t = new ThreadObject;
    t->type = ObjType::Thread;
    t->startup = ARG( 3 );
    t->entry = ARG( 4 );
    t->arg1 = ARG( 5 );
    t->suspended = ARG( 6 ) & 1;  // CREATE_SUSPENDED
    t->thread_id = __atomic_fetch_add( &next_id, 1, __ATOMIC_SEQ_CST );
    // Flags bits 24-31: with 0x80, the hardware thread number; otherwise an affinity mask.
    uint32_t flags = ARG( 6 ), top = flags >> 24;
    if ( flags & 0x80 ) t->processor = (int)( top % 6 );
    else if ( top ) t->processor = __builtin_ctz( top ) % 6;
    t->kthread = rt_alloc_virtual( 0x200, 0x10 );
    memset( g_base + t->kthread, 0, 0x200 );
    g_base[t->kthread] = 6;  // dispatcher type: thread
    guest_write32( t->kthread + 0x14C, t->thread_id );  // KTHREAD thread id (rt_guest_thread_create)
    uint32_t h = obj_insert( t );
    write_handle( ARG( 0 ), h );
    if ( ARG( 2 ) ) guest_write32( ARG( 2 ), t->thread_id );
    rt_log( "ExCreateThread %04X: entry %08X(%08X) flags %08X%s, lr=%08X\n", h, t->entry, t->arg1, (uint32_t)ARG( 6 ), t->suspended ? " suspended" : "", (uint32_t)c.lr );

    std::thread( [t, h] {
        t_thread_handle = h;
        {
            std::unique_lock<std::mutex> lock( g_lock );
            g_cv.wait( lock, [t] { return !t->suspended; } );
        }
        rt_log( "thread %04X resumed\n", h );
        int processor;
        {
            std::lock_guard<std::mutex> g( g_lock );
            processor = t->processor;
        }
        GuestThread *gt = rt_guest_thread_create( t->startup ? "xapi" : "kernel", t->kthread, processor );
        {
            std::lock_guard<std::mutex> g( g_lock );
            t->started = true;  // from now on KeSetAffinityThread moves the running thread
            if ( t->processor != processor ) rt_kthread_set_processor( t->kthread, t->processor, nullptr );
        }
        try
        {
            if ( t->startup ) rt_guest_call( gt, t->startup, t->entry, t->arg1 );
            else rt_guest_call( gt, t->entry, t->arg1, 0 );
        }
        catch ( const GuestThreadExit & ) {}
        char why[64];
        snprintf( why, sizeof(why), "thread %04X exit", h );
        rt_dump_trace( why );
        {
            std::lock_guard<std::mutex> g( g_lock );
            t->finished = true;
        }
        g_cv.notify_all();
    } ).detach();
    c.r3.u64 = STATUS_SUCCESS;
}

// NtResumeThread(HANDLE, PULONG previousSuspendCount)
void imp_NtResumeThread( PPCContext &c )
{
    std::unique_lock<std::mutex> lock( g_lock );
    Object *o = get_locked( ARG( 0 ) );
    if ( ARG( 1 ) ) guest_write32( ARG( 1 ), o && o->type == ObjType::Thread && static_cast<ThreadObject *>( o )->suspended );
    if ( o && o->type == ObjType::Thread ) static_cast<ThreadObject *>( o )->suspended = false;
    lock.unlock();
    g_cv.notify_all();
    c.r3.u64 = STATUS_SUCCESS;
}

void imp_NtClose( PPCContext &c ) { c.r3.u64 = obj_close( ARG( 0 ) ) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE; }

// NtDuplicateObject(sourceProcess, sourceHandle, targetProcess, PHANDLE target, ...): same object, same handle.
void imp_NtDuplicateObject( PPCContext &c )
{
    uint32_t h = ARG( 1 ) == CURRENT_THREAD ? t_thread_handle : ARG( 1 );
    write_handle( ARG( 3 ), h );
    c.r3.u64 = STATUS_SUCCESS;
}

// ------------------------------------------------------------------ Ke* objects in guest memory
// DISPATCHER_HEADER { u8 Type; u8 Absolute; u8 ProcessType; u8 Inserted; s32 SignalState; ... }
// Type 0 = notification event, 1 = synchronization (auto-reset) event, 2 = mutant, 5 = semaphore.
// Waits read the signal state straight from guest memory, under the same lock as the handles.
namespace
{
int32_t ke_state( uint32_t obj ) { return (int32_t)guest_read32( obj + 4 ); }
void ke_set_state( uint32_t obj, int32_t v ) { guest_write32( obj + 4, (uint32_t)v ); }

// Same set-time hand-off as handle events, tracked host-side per guest object address.
struct KeWaitInfo { unsigned waiters = 0, grants = 0; uint64_t pulses = 0; };
std::unordered_map<uint32_t, KeWaitInfo> g_ke_info;

void ke_signal_locked( uint32_t obj, bool pulse_only )
{
    KeWaitInfo &w = g_ke_info[obj];
    if ( g_base[obj] == 0 ) { ++w.pulses; if ( !pulse_only ) ke_set_state( obj, 1 ); return; }  // notification
    if ( w.waiters > w.grants ) ++w.grants;
    else if ( !pulse_only ) ke_set_state( obj, 1 );
}

uint32_t ke_wait( const std::vector<uint32_t> &objs, bool any, uint32_t ptimeout )
{
    std::chrono::steady_clock::time_point deadline;
    bool timed = deadline_from( ptimeout, deadline );
    std::unique_lock<std::mutex> lock( g_lock );
    bool reported = false;
    auto since = std::chrono::steady_clock::now();
    std::vector<uint64_t> start( objs.size() );
    for ( size_t i = 0; i < objs.size(); ++i ) { KeWaitInfo &w = g_ke_info[objs[i]]; ++w.waiters; start[i] = w.pulses; }
    auto ready = [&]( size_t i ) {
        uint32_t o = objs[i];
        if ( ke_state( o ) > 0 ) return true;
        uint8_t type = g_base[o];
        KeWaitInfo &w = g_ke_info[o];
        return type == 0 ? w.pulses != start[i] : type == 1 ? w.grants > 0 : false;
    };
    auto acquire = [&]( uint32_t o ) {
        uint8_t type = g_base[o];
        KeWaitInfo &w = g_ke_info[o];
        if ( type == 1 ) { if ( ke_state( o ) > 0 ) ke_set_state( o, 0 ); else if ( w.grants ) --w.grants; }
        else if ( type == 5 && ke_state( o ) > 0 ) ke_set_state( o, ke_state( o ) - 1 );
    };
    auto finish = [&]( uint32_t status ) {
        for ( uint32_t o : objs ) --g_ke_info[o].waiters;
        static const char *watch = getenv( "CRACKDOWN_WATCH_KESET" );
        if ( watch && strtoul( watch, nullptr, 16 ) == objs[0] && status != STATUS_TIMEOUT )
            rt_log( "watched Ke wait on %08X done: status %X type %u state %d\n", objs[0], status, g_base[objs[0]], ke_state( objs[0] ) );
        return status;
    };
    for ( ;; )
    {
        if ( any )
        {
            for ( size_t i = 0; i < objs.size(); ++i )
                if ( ready( i ) ) { acquire( objs[i] ); return finish( STATUS_SUCCESS + (uint32_t)i ); }
        }
        else
        {
            bool all = true;
            for ( size_t i = 0; i < objs.size(); ++i ) all = all && ready( i );
            if ( all ) { for ( uint32_t o : objs ) acquire( o ); return finish( STATUS_SUCCESS ); }
        }
        if ( timed )
        {
            if ( ptimeout && guest_read32( ptimeout ) == 0 && guest_read32( ptimeout + 4 ) == 0 ) return finish( STATUS_TIMEOUT );
            if ( g_cv.wait_until( lock, deadline ) == std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline )
                return finish( STATUS_TIMEOUT );
        }
        else wait_forever( lock, reported, since, "Ke", objs.data(), objs.size() );
    }
}
} // namespace

// KeWaitForSingleObject(object, reason, mode, alertable, PLARGE_INTEGER timeout)
RT_IMPORT( KeWaitForSingleObject )
{
    auto t0 = std::chrono::steady_clock::now();
    uint32_t r = ke_wait( { RT_ARG( 0 ) }, true, RT_ARG( 4 ) );
    wait_stat( (uint32_t)c.lr, "KeWait", std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
    RT_RETURN( r );
}

// KeWaitForMultipleObjects(count, objects[], WAIT_TYPE, reason, mode, alertable, timeout, waitBlocks)
RT_IMPORT( KeWaitForMultipleObjects )
{
    std::vector<uint32_t> objs;
    for ( uint32_t i = 0; i < RT_ARG( 0 ); ++i ) objs.push_back( guest_read32( RT_ARG( 1 ) + i * 4 ) );
    RT_RETURN( ke_wait( objs, RT_ARG( 2 ) == 1, RT_ARG( 6 ) ) );
}

// KeSetEvent(event, increment, wait) -> previous state
RT_IMPORT( KeSetEvent )
{
    int32_t prev;
    {
        std::lock_guard<std::mutex> g( g_lock );
        prev = ke_state( RT_ARG( 0 ) );
        ke_signal_locked( RT_ARG( 0 ), false );
    }
    static const char *watch = getenv( "CRACKDOWN_WATCH_KESET" );  // hex guest address: who sets it
    if ( watch && strtoul( watch, nullptr, 16 ) == RT_ARG( 0 ) ) rt_dump_trace( "KeSetEvent watch" );
    g_cv.notify_all();
    RT_RETURN( prev );
}

RT_IMPORT( KeResetEvent )
{
    std::lock_guard<std::mutex> g( g_lock );
    int32_t prev = ke_state( RT_ARG( 0 ) );
    ke_set_state( RT_ARG( 0 ), 0 );
    RT_RETURN( prev );
}

RT_IMPORT( KePulseEvent )
{
    {
        std::lock_guard<std::mutex> g( g_lock );
        ke_signal_locked( RT_ARG( 0 ), true );
    }
    g_cv.notify_all();
    RT_RETURN( 0 );
}

// KeReleaseSemaphore(semaphore, increment, adjustment, wait) -> previous count
RT_IMPORT( KeReleaseSemaphore )
{
    int32_t prev;
    {
        std::lock_guard<std::mutex> g( g_lock );
        prev = ke_state( RT_ARG( 0 ) );
        ke_set_state( RT_ARG( 0 ), prev + (int32_t)RT_ARG( 2 ) );
    }
    g_cv.notify_all();
    RT_RETURN( prev );
}

// KeInitializeSemaphore(semaphore, count, limit) / KeInitializeEvent(event, type, state)
RT_IMPORT( KeInitializeSemaphore )
{
    uint32_t p = RT_ARG( 0 );
    memset( g_base + p, 0, 0x14 );
    g_base[p] = 5;
    ke_set_state( p, (int32_t)RT_ARG( 1 ) );
    guest_write32( p + 8, p + 8 );  // empty wait list
    guest_write32( p + 12, p + 8 );
    guest_write32( p + 0x10, RT_ARG( 2 ) );
    RT_RETURN( 0 );
}
RT_IMPORT( KeInitializeEvent )
{
    uint32_t p = RT_ARG( 0 );
    memset( g_base + p, 0, 0x10 );
    g_base[p] = (uint8_t)RT_ARG( 1 );
    ke_set_state( p, (int32_t)( RT_ARG( 2 ) & 0xFF ) );
    guest_write32( p + 8, p + 8 );
    guest_write32( p + 12, p + 8 );
    RT_RETURN( 0 );
}

// ------------------------------------------------------------------ object pointers
// ObReferenceObjectByHandle hands out a small guest stand-in object per handle (a KTHREAD for
// threads), so Ke* calls that take object pointers can find their way back to the handle.
namespace
{
std::unordered_map<uint32_t, uint32_t> g_obj_ptr, g_ptr_handle;  // handle -> guest object, back
}

RT_IMPORT( ObReferenceObjectByHandle )  // (handle, type, PVOID *object)
{
    std::lock_guard<std::mutex> g( g_lock );
    uint32_t h = RT_ARG( 0 ) == CURRENT_THREAD ? t_thread_handle : RT_ARG( 0 );
    uint32_t &p = g_obj_ptr[h];
    if ( !p )
    {
        Object *o = get_locked( h );
        if ( o && o->type == ObjType::Thread ) p = static_cast<ThreadObject *>( o )->kthread;
        else
        {
            p = rt_alloc_virtual( 0x200, 0x10 );
            g_base[p] = 0;
        }
        g_ptr_handle[p] = h;
    }
    if ( RT_ARG( 2 ) ) guest_write32( RT_ARG( 2 ), p );
    RT_RETURN( STATUS_SUCCESS );
}
RT_IMPORT( ObDereferenceObject ) { RT_RETURN( 0 ); }

// KeSetAffinityThread(KTHREAD *, affinity mask, PULONG previous mask): the thread moves to the lowest
// hardware thread in the mask (r13+268). A thread not started yet takes it when it starts.
RT_IMPORT( KeSetAffinityThread )
{
    uint32_t kthread = RT_ARG( 0 ), mask = RT_ARG( 1 ) & 0x3F;
    int cpu = mask ? __builtin_ctz( mask ) : 0, prev = -1;
    if ( !rt_kthread_set_processor( kthread, cpu, &prev ) )
    {
        std::lock_guard<std::mutex> g( g_lock );
        auto it = g_ptr_handle.find( kthread );
        Object *o = it != g_ptr_handle.end() ? get_locked( it->second ) : nullptr;
        if ( o && o->type == ObjType::Thread )
        {
            auto *t = static_cast<ThreadObject *>( o );
            prev = t->processor;
            if ( !t->started ) t->processor = cpu;
            else rt_kthread_set_processor( kthread, cpu, &prev );  // started in between
        }
    }
    uint32_t prev_mask = prev >= 0 ? 1u << prev : 0x3F;
    if ( RT_ARG( 2 ) ) guest_write32( RT_ARG( 2 ), prev_mask );
    RT_RETURN( prev_mask );
}

// KeResumeThread(KTHREAD *) -> previous suspend count
RT_IMPORT( KeResumeThread )
{
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> g( g_lock );
        auto it = g_ptr_handle.find( RT_ARG( 0 ) );
        Object *o = it == g_ptr_handle.end() ? nullptr : get_locked( it->second );
        if ( o && o->type == ObjType::Thread )
        {
            auto *t = static_cast<ThreadObject *>( o );
            prev = t->suspended;
            t->suspended = false;
        }
        else rt_log( "KeResumeThread: unknown thread object %08X\n", RT_ARG( 0 ) );
    }
    g_cv.notify_all();
    RT_RETURN( prev );
}
RT_IMPORT( KeSetBasePriorityThread ) { RT_RETURN( 0 ); }
RT_IMPORT( KeQueryBasePriorityThread ) { RT_RETURN( 0 ); }

// ------------------------------------------------------------------ IRQL and spin locks
// No interrupt levels here: IRQL calls report PASSIVE_LEVEL; spin locks spin on the guest word.
namespace
{
void spin_acquire( uint32_t lock )
{
    uint32_t *w = (uint32_t *)( g_base + lock );
    uint32_t want = __builtin_bswap32( 0x10000 | ( sync_current_thread_handle() & 0xFFFF ) ), zero;
    for ( ;; )
    {
        zero = 0;
        if ( __atomic_compare_exchange_n( w, &zero, want, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED ) ) return;
        std::this_thread::yield();
    }
}
void spin_release( uint32_t lock ) { __atomic_store_n( (uint32_t *)( g_base + lock ), 0u, __ATOMIC_RELEASE ); }
}

RT_IMPORT( KeRaiseIrqlToDpcLevel ) { RT_RETURN( 0 ); }
RT_IMPORT( KfLowerIrql ) { RT_RETURN( 0 ); }
RT_IMPORT( KfAcquireSpinLock ) { spin_acquire( RT_ARG( 0 ) ); RT_RETURN( 0 ); }
RT_IMPORT( KfReleaseSpinLock ) { spin_release( RT_ARG( 0 ) ); RT_RETURN( 0 ); }
RT_IMPORT( KeAcquireSpinLockAtRaisedIrql ) { spin_acquire( RT_ARG( 0 ) ); RT_RETURN( 0 ); }
RT_IMPORT( KeReleaseSpinLockFromRaisedIrql ) { spin_release( RT_ARG( 0 ) ); RT_RETURN( 0 ); }
RT_IMPORT( KeTryToAcquireSpinLockAtRaisedIrql )
{
    uint32_t zero = 0, want = __builtin_bswap32( 0x10000 );
    RT_RETURN( __atomic_compare_exchange_n( (uint32_t *)( g_base + RT_ARG( 0 ) ), &zero, want, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED ) );
}
RT_IMPORT( KeEnterCriticalRegion ) { RT_RETURN( 0 ); }
RT_IMPORT( KeLeaveCriticalRegion ) { RT_RETURN( 0 ); }
