// xam: title-side system services -- memory, notifications, controllers.
// Input: controller 0 from the host keyboard and mouse (video.cpp, input.h); other users not connected.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "settings.h"
#include "input.h"
#include "objects.h"
#include "profile.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <atomic>
#include <string>

uint32_t xui_element( uint32_t h );  // xui.cpp
void watch_arm( uint32_t lo, uint32_t hi, double seconds );  // watch.cpp
void watch_arm_views( uint32_t phys_page, double seconds );
void watch_axis_callers( uint32_t lo, uint32_t hi, double seconds );
void watch_callers( const char *list, double seconds );
void finder_poll( double t );  // finder.cpp
void finder_fov_poll( double t );  // finder.cpp
void finder_peek_poll( double t );  // finder.cpp
void finder_float_poll( double t );  // finder.cpp
uint32_t xui_focus( int user );
void xui_dump( const char *why, uint32_t h );
void xui_list_controls( uint32_t h );
void xui_tree( uint32_t h );
#include <vector>

namespace
{
constexpr uint32_t ERROR_SUCCESS = 0, ERROR_DEVICE_NOT_CONNECTED = 0x48F, ERROR_EMPTY = 0x10D2;
constexpr uint32_t HDD_DEVICE_ID = 1;
}

// XamAlloc(flags, size, PVOID *out) / XamFree(p)
RT_IMPORT( XamAlloc )
{
    uint32_t p = rt_alloc_virtual( ( RT_ARG( 1 ) + 0xF ) & ~0xFu, 0x10 );
    if ( !p ) RT_RETURN( 0x8007000E );  // E_OUTOFMEMORY
    memset( g_base + p, 0, RT_ARG( 1 ) );
    guest_write32( RT_ARG( 2 ), p );
    RT_RETURN( 0 );
}
RT_IMPORT( XamFree ) { RT_RETURN( 0 ); }

// ------------------------------------------------------------------ notifications
// A listener is a waitable handle (signalled while notifications are queued) for a mask of areas;
// an ID's area is bits 25+. Like the console, the first system-area listener is told at once that
// the system UI opened and closed and that sign-in changed (games wait for those before input).
namespace
{
constexpr uint32_t XN_SYS_UI = 0x9, XN_SYS_SIGNINCHANGED = 0xA, XN_SYS_STORAGEDEVICESCHANGED = 0xB;
struct Listener { uint64_t mask; std::deque<std::pair<uint32_t, uint32_t>> queue; };
std::mutex g_notify_lock;
std::unordered_map<uint32_t, Listener> g_listeners;
bool g_startup_notified;

void enqueue_locked( uint32_t handle, Listener &l, uint32_t id, uint32_t param )
{
    l.queue.push_back( { id, param } );
    sync_set_event( handle );
}
}

void xam_broadcast_notification( uint32_t id, uint32_t param )
{
    std::lock_guard<std::mutex> g( g_notify_lock );
    for ( auto &[h, l] : g_listeners )
        if ( l.mask & ( 1ull << ( id >> 25 ) ) ) enqueue_locked( h, l, id, param );
}

// XamNotifyCreateListener(u64 areas, version) -> handle
RT_IMPORT( XamNotifyCreateListener )
{
    uint64_t mask = c.r3.u64;
    uint32_t h = sync_create_event( true, false );
    std::lock_guard<std::mutex> g( g_notify_lock );
    Listener &l = g_listeners[h];
    l.mask = mask;
    rt_log( "xam: notification listener %X for areas %llX\n", h, (unsigned long long)mask );
    if ( !g_startup_notified && ( mask & 1 ) )
    {
        g_startup_notified = true;
        enqueue_locked( h, l, XN_SYS_UI, 1 );
        enqueue_locked( h, l, XN_SYS_UI, 0 );
        enqueue_locked( h, l, XN_SYS_SIGNINCHANGED, 1 );
        enqueue_locked( h, l, XN_SYS_SIGNINCHANGED, 1 );
    }
    else if ( mask & 1 )
        enqueue_locked( h, l, XN_SYS_STORAGEDEVICESCHANGED, 0 );  // the hard drive is present
    RT_RETURN( h );
}

// XNotifyGetNext(handle, match id (0 = any), PDWORD id, PULONG_PTR param) -> BOOL
RT_IMPORT( XNotifyGetNext )
{
    uint32_t h = RT_ARG( 0 ), match = RT_ARG( 1 );
    std::lock_guard<std::mutex> g( g_notify_lock );
    auto it = g_listeners.find( h );
    if ( it == g_listeners.end() ) RT_RETURN( 0 );
    auto &q = it->second.queue;
    auto n = q.begin();
    if ( match ) n = std::find_if( q.begin(), q.end(), [&]( auto &e ) { return e.first == match; } );
    if ( n == q.end() )
    {
        static int misses;
        if ( match && misses++ < 20 ) rt_log( "xam: XNotifyGetNext(%X, match %X): none (%zu queued)\n", h, match, q.size() );
        RT_RETURN( 0 );
    }
    uint32_t id = n->first, param = n->second;
    q.erase( n );
    if ( q.empty() ) sync_reset_event( h );
    if ( RT_ARG( 2 ) ) guest_write32( RT_ARG( 2 ), id );
    if ( RT_ARG( 3 ) ) guest_write32( RT_ARG( 3 ), param );
    rt_log( "xam: notification %X(%X) delivered to %X\n", id, param, h );
    RT_RETURN( 1 );
}

// ------------------------------------------------------------------ users
// One local (offline) profile (profile.h), signed in on controller 0.
namespace
{
constexpr uint32_t ERROR_NO_SUCH_USER = 0x525;
}
// XamUserGetSigninState(user) -> 0 not signed in, 1 locally, 2 Live
RT_IMPORT( XamUserGetSigninState ) { RT_RETURN( ( RT_ARG( 0 ) & 0xFF ) == 0 ? 1 : 0 ); }
// XamUserGetXUID(user, type mask, PXUID)
RT_IMPORT( XamUserGetXUID )
{
    if ( ( RT_ARG( 0 ) & 0xFF ) != 0 ) RT_RETURN( ERROR_NO_SUCH_USER );
    if ( RT_ARG( 2 ) ) guest_write64( RT_ARG( 2 ), profile_xuid() );
    RT_RETURN( ERROR_SUCCESS );
}
// XamUserGetName(user, char *buffer, size)
RT_IMPORT( XamUserGetName )
{
    if ( ( RT_ARG( 0 ) & 0xFF ) != 0 ) RT_RETURN( ERROR_NO_SUCH_USER );
    uint32_t size = RT_ARG( 2 );
    if ( RT_ARG( 1 ) && size )
    {
        const std::string name = profile_name();
        uint32_t n = std::min<uint32_t>( size - 1, (uint32_t)name.size() );
        memcpy( g_base + RT_ARG( 1 ), name.data(), n );
        g_base[RT_ARG( 1 ) + n] = 0;
    }
    RT_RETURN( ERROR_SUCCESS );
}
// ------------------------------------------------------------------ async completion
// XOVERLAPPED { InternalLow (result), InternalHigh (length), InternalContext, hEvent, pCompletionRoutine,
// dwCompletionContext, dwExtendedError }: XAM finishes the request at once and signals the event.
namespace
{
constexpr uint32_t ERROR_IO_PENDING = 997, ERROR_NO_MORE_FILES = 0x12, ERROR_INSUFFICIENT_BUFFER = 0x7A;
uint32_t complete( uint32_t overlapped, uint32_t result, uint32_t length = 0 )
{
    if ( !overlapped ) return result;
    guest_write32( overlapped + 0x00, result );
    guest_write32( overlapped + 0x04, length );
    guest_write32( overlapped + 0x18, result );
    if ( guest_read32( overlapped + 0x10 ) ) rt_log( "xam: overlapped completion routine %08X not called\n", guest_read32( overlapped + 0x10 ) );
    if ( uint32_t ev = guest_read32( overlapped + 0x0C ) ) sync_set_event( ev );
    return ERROR_IO_PENDING;
}
uint32_t stack_arg( PPCContext &c, int i ) { return guest_read32( c.r1.u32 + 0x54 + ( i - 8 ) * 8 ); }  // 0-based arg index >= 8
}

// XamUserReadProfileSettings(title, user, xuid count, xuids, setting count, PDWORD setting ids,
//                            PDWORD buffer size, XUSER_READ_PROFILE_SETTING_RESULT *, XOVERLAPPED *)
// result { u32 count; XUSER_PROFILE_SETTING *settings } then 0x28-byte settings:
//   +0 source (0 none, 1 default, 2 the title's), +8 user index, +0x10 id, +0x18 XUSER_DATA { u8 type;
//   +8 value (strings and binary: +8 length, +0xC pointer) }, then the strings' and binary values' bytes.
// The settings the game wrote come back (profile.h); the others without a value: its own defaults.
RT_IMPORT( XamUserReadProfileSettings )
{
    uint32_t xuids = RT_ARG( 2 ), count = RT_ARG( 4 ), ids = RT_ARG( 5 ), size_ptr = RT_ARG( 6 ), buf = RT_ARG( 7 ), ov = stack_arg( c, 8 );
    uint32_t users = std::max( xuids, 1u );
    uint32_t need = 8 + users * count * 0x28;
    for ( uint32_t i = 0; i < count; ++i )
    {
        uint8_t type;
        std::vector<uint8_t> v;
        if ( profile_setting( guest_read32( ids + i * 4 ), type, v ) && ( type == 4 || type == 6 ) ) need += users * ( ( (uint32_t)v.size() + 7 ) & ~7u );
    }
    static int logged;
    if ( logged++ < 6 )
        rt_log( "xam: XamUserReadProfileSettings(title %08X, user %u, xuids %u @%08X, %u settings [%08X], buffer %08X size %u) overlapped %08X\n", RT_ARG( 0 ),
                RT_ARG( 1 ), xuids, RT_ARG( 3 ), count, count ? guest_read32( ids ) : 0, buf, size_ptr ? guest_read32( size_ptr ) : 0, ov );
    if ( !buf || !size_ptr || guest_read32( size_ptr ) < need )
    {
        if ( size_ptr ) guest_write32( size_ptr, need );
        RT_RETURN( ERROR_INSUFFICIENT_BUFFER );  // synchronous: the overlapped request never starts
    }
    memset( g_base + buf, 0, need );
    guest_write32( buf, users * count );
    guest_write32( buf + 4, buf + 8 );
    uint32_t extra = buf + 8 + users * count * 0x28;  // strings' and binary values' bytes
    for ( uint32_t u = 0; u < users; ++u )
        for ( uint32_t i = 0; i < count; ++i )
        {
            uint32_t id = guest_read32( ids + i * 4 ), e = buf + 8 + ( u * count + i ) * 0x28;
            guest_write32( e + 0x00, 0 );
            guest_write32( e + 0x08, RT_ARG( 1 ) );
            guest_write32( e + 0x10, id );
            g_base[e + 0x18] = id >> 28;
            uint8_t type;
            std::vector<uint8_t> v;
            if ( !profile_setting( id, type, v ) ) continue;
            guest_write32( e + 0x00, 2 );
            g_base[e + 0x18] = type;
            if ( type == 4 || type == 6 )
            {
                memcpy( g_base + extra, v.data(), v.size() );
                guest_write32( e + 0x20, (uint32_t)v.size() ), guest_write32( e + 0x24, extra );
                extra += ( (uint32_t)v.size() + 7 ) & ~7u;
            }
            else memcpy( g_base + e + 0x20, v.data(), std::min<size_t>( v.size(), 8 ) );
        }
    RT_RETURN( complete( ov, ERROR_SUCCESS ) );
}
// XamUserWriteProfileSettings(title, user, count, XUSER_PROFILE_SETTING *, XOVERLAPPED *): kept in the profile.
RT_IMPORT( XamUserWriteProfileSettings )
{
    const uint32_t count = RT_ARG( 2 ), settings = RT_ARG( 3 );
    for ( uint32_t i = 0; settings && i < count; ++i )
    {
        const uint32_t e = settings + i * 0x28, id = guest_read32( e + 0x10 );
        const uint8_t type = g_base[e + 0x18];
        std::vector<uint8_t> v;
        if ( type == 4 || type == 6 )
        {
            const uint32_t len = std::min( guest_read32( e + 0x20 ), 0x10000u ), at = guest_read32( e + 0x24 );
            if ( at ) v.assign( g_base + at, g_base + at + len );
        }
        else v.assign( g_base + e + 0x20, g_base + e + 0x28 );
        profile_set_setting( id, type, v );
    }
    static int logged;
    if ( logged++ < 4 ) rt_log( "xam: XamUserWriteProfileSettings(%u settings, first %08X)\n", count, count ? guest_read32( settings + 0x10 ) : 0 );
    RT_RETURN( complete( RT_ARG( 4 ), ERROR_SUCCESS ) );
}

// ------------------------------------------------------------------ content (saves)
// A content package is a host folder: <content folder>/<content type>/<file name>/ (profile.h: saves are
// the profile's, the rest shared in <data dir>/content), with the
// display name kept beside it in <file name>.name (UTF-8). XCONTENT_DATA (0x134) { u32 DeviceID;
// u32 ContentType; WCHAR DisplayName[128]; CHAR FileName[42]; }
namespace
{
namespace fs = std::filesystem;
constexpr uint32_t XCONTENT_DATA_SIZE = 0x134;

std::string content_file_name( uint32_t data )
{
    std::string name( (const char *)g_base + data + 0x108, strnlen( (const char *)g_base + data + 0x108, 42 ) );
    for ( char &ch : name ) if ( ch == '/' || ch == '\\' ) ch = '_';
    return name;
}

fs::path content_path( uint32_t data )
{
    char type[16];
    snprintf( type, sizeof( type ), "%08X", guest_read32( data + 4 ) );
    return profile_content_dir( guest_read32( data + 4 ) ) / type / content_file_name( data );
}

// Enumerators hand out fixed-size records until they run out.
struct Enumerator
{
    uint32_t record_size;
    std::vector<std::vector<uint8_t>> records;
    size_t next = 0;
    // Or its own filling (records that point at more in the buffer): (buffer, size, next) -> items written.
    std::function<uint32_t( uint32_t, uint32_t, size_t & )> fill;
};
std::mutex g_enum_lock;
std::unordered_map<uint32_t, Enumerator> g_enumerators;

uint32_t make_enumerator( Enumerator e )
{
    uint32_t h = sync_create_event( true, true );
    std::lock_guard<std::mutex> g( g_enum_lock );
    g_enumerators[h] = std::move( e );
    return h;
}
}

// XamUserCreateAchievementEnumerator(title, user, u64 xuid, detail flags, first, count, PDWORD size, PHANDLE):
// the game's achievements (profile.h) with the profile's unlocks. Each enumeration fills an array of
// XACHIEVEMENT_DETAILS { id; label, description, unachieved (UTF-16 pointers); image id; points; FILETIME
// unlocked; flags (| 0x20000 unlocked) } (0x24 bytes) with the strings after it.
RT_IMPORT( XamUserCreateAchievementEnumerator )
{
    constexpr uint32_t DETAILS = 0x24, FULL = DETAILS + ( 33 + 100 + 100 ) * 2;  // with the longest strings
    const uint32_t flags = RT_ARG( 3 ), first = RT_ARG( 4 ), want = std::max( RT_ARG( 5 ), 1u );
    const auto &all = achievements();
    Enumerator e{ FULL, {} };
    e.next = first;
    e.fill = [flags, want, &all]( uint32_t buf, uint32_t size, size_t &next ) {
        uint32_t n = 0;
        const size_t end = std::min( all.size(), (size_t)want );
        while ( next < end && ( n + 1 ) * FULL <= size ) ++n, ++next;
        next -= n;
        uint32_t strings = buf + n * DETAILS;
        auto put = [&]( const std::string &s, bool want_it ) -> uint32_t {
            if ( !want_it ) return 0;
            const uint32_t at = strings;
            for ( unsigned char ch : s ) guest_write16( strings, ch ), strings += 2;
            guest_write16( strings, 0 ), strings += 2;
            return at;
        };
        for ( uint32_t i = 0; i < n; ++i, ++next )
        {
            const Achievement &a = all[next];
            const uint32_t d = buf + i * DETAILS;
            uint64_t when = 0;
            const bool got = achievement_unlocked( a.id, &when );
            guest_write32( d + 0x00, a.id );
            guest_write32( d + 0x04, put( a.label, flags & 2 ) );
            guest_write32( d + 0x08, put( a.description, flags & 4 ) );
            guest_write32( d + 0x0C, put( a.unachieved, flags & 8 ) );
            guest_write32( d + 0x10, a.image );
            guest_write32( d + 0x14, a.points );
            guest_write64( d + 0x18, when );
            guest_write32( d + 0x20, a.flags | ( got ? 0x20000u : 0u ) );
        }
        return n;
    };
    if ( RT_ARG( 6 ) ) guest_write32( RT_ARG( 6 ), want * FULL );
    if ( RT_ARG( 7 ) ) guest_write32( RT_ARG( 7 ), make_enumerator( std::move( e ) ) );
    rt_log( "xam: achievement enumerator (flags %X, from %u, %u)\n", flags, first, want );
    RT_RETURN( ERROR_SUCCESS );
}

// XamContentCreateEnumerator(user, device, content type, flags, items, PDWORD size, PHANDLE)
RT_IMPORT( XamContentCreateEnumerator )
{
    uint32_t device = RT_ARG( 1 ), type = RT_ARG( 2 );
    Enumerator e{ XCONTENT_DATA_SIZE, {} };
    char sub[16];
    snprintf( sub, sizeof( sub ), "%08X", type );
    std::error_code ec;
    if ( device == 0 || device == HDD_DEVICE_ID )
        for ( auto &entry : fs::directory_iterator( profile_content_dir( type ) / sub, ec ) )
        {
            if ( !entry.is_directory() ) continue;
            std::vector<uint8_t> r( XCONTENT_DATA_SIZE, 0 );
            auto put32 = [&]( size_t o, uint32_t v ) { for ( int i = 0; i < 4; ++i ) r[o + i] = v >> ( 24 - i * 8 ); };
            put32( 0, HDD_DEVICE_ID );
            put32( 4, type );
            std::string file = entry.path().filename().string(), display = file;
            if ( FILE *f = fopen( ( entry.path().string() + ".name" ).c_str(), "rb" ) )
            {
                char b[256];
                size_t n = fread( b, 1, sizeof( b ), f );
                display.assign( b, n );
                fclose( f );
            }
            for ( size_t i = 0; i < display.size() && i < 127; ++i ) { r[8 + i * 2] = 0; r[9 + i * 2] = (uint8_t)display[i]; }
            memcpy( r.data() + 0x108, file.data(), std::min<size_t>( file.size(), 42 ) );  // FileName[42]: no terminator when full
            e.records.push_back( std::move( r ) );
        }
    rt_log( "xam: content enumerator (device %u, type %X): %zu packages\n", device, type, e.records.size() );
    if ( RT_ARG( 5 ) ) guest_write32( RT_ARG( 5 ), std::max( RT_ARG( 4 ), 1u ) * XCONTENT_DATA_SIZE );
    if ( RT_ARG( 6 ) ) guest_write32( RT_ARG( 6 ), make_enumerator( std::move( e ) ) );
    RT_RETURN( ERROR_SUCCESS );
}

// XamEnumerate(handle, flags, buffer, size, PDWORD items, XOVERLAPPED *)
RT_IMPORT( XamEnumerate )
{
    uint32_t h = RT_ARG( 0 ), buf = RT_ARG( 2 ), size = RT_ARG( 3 ), n = 0;
    {
        std::lock_guard<std::mutex> g( g_enum_lock );
        auto it = g_enumerators.find( h );
        if ( it != g_enumerators.end() )
        {
            Enumerator &e = it->second;
            if ( e.fill ) n = e.fill( buf, size, e.next );
            else while ( e.next < e.records.size() && ( n + 1 ) * e.record_size <= size )
            {
                memcpy( g_base + buf + n * e.record_size, e.records[e.next].data(), e.record_size );
                ++e.next, ++n;
            }
        }
    }
    uint32_t result = n ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
    if ( RT_ARG( 4 ) ) guest_write32( RT_ARG( 4 ), n );
    RT_RETURN( complete( RT_ARG( 5 ), result, n ) );
}

// XamContentCreateEx(user, root name, XCONTENT_DATA *, flags, PDWORD disposition, PDWORD license,
//                    cache size, u64 content size, XOVERLAPPED * [stack])
// flags & 0xF: 1 create new, 2 create always, 3 open existing, 4 open always, 5 truncate existing.
RT_IMPORT( XamContentCreateEx )
{
    constexpr uint32_t ERROR_FILE_NOT_FOUND = 2, ERROR_ALREADY_EXISTS = 0xB7, ERROR_ACCESS_DENIED = 5;
    std::string root( (const char *)g_base + RT_ARG( 1 ) );
    uint32_t data = RT_ARG( 2 ), mode = RT_ARG( 3 ) & 0xF, ov = stack_arg( c, 8 );
    fs::path dir = content_path( data );
    std::error_code ec;
    bool exists = fs::is_directory( dir, ec );
    uint32_t result = ERROR_SUCCESS, disposition = 2;  // 1 created, 2 opened
    if ( ( mode == 3 || mode == 5 ) && !exists ) result = ERROR_FILE_NOT_FOUND;
    else if ( mode == 1 && exists ) result = ERROR_ALREADY_EXISTS;
    else
    {
        if ( exists && ( mode == 2 || mode == 5 ) ) fs::remove_all( dir, ec ), exists = false;
        if ( !exists )
        {
            fs::create_directories( dir, ec );
            disposition = 1;
            std::string display;
            for ( int i = 0; i < 128; ++i )
            {
                uint16_t w = ( g_base[data + 8 + i * 2] << 8 ) | g_base[data + 9 + i * 2];
                if ( !w ) break;
                display += w < 128 ? (char)w : '?';
            }
            if ( FILE *f = fopen( ( dir.string() + ".name" ).c_str(), "wb" ) ) { fwrite( display.data(), 1, display.size(), f ); fclose( f ); }
        }
        if ( !fs::is_directory( dir, ec ) ) result = ERROR_ACCESS_DENIED;
        else files_mount( root, dir.string() );
    }
    rt_log( "xam: XamContentCreateEx(%s:, %s, mode %u) -> %s (%u)\n", root.c_str(), dir.string().c_str(), mode,
            result ? "failed" : disposition == 1 ? "created" : "opened", result );
    if ( !result && RT_ARG( 4 ) ) guest_write32( RT_ARG( 4 ), disposition );
    // The license mask: installed content counts as bought (a zero mask is content the profile doesn't
    // own, which the game only lets be played in co-op with someone who does: solo Street Racing asked
    // for "an additional downloadable content package").
    if ( RT_ARG( 5 ) ) guest_write32( RT_ARG( 5 ), 0xFFFFFFFF );
    RT_RETURN( complete( ov, result ) );
}

// XamContentClose(root name, XOVERLAPPED *)
RT_IMPORT( XamContentClose )
{
    std::string root( (const char *)g_base + RT_ARG( 0 ) );
    bool ok = files_unmount( root );
    rt_log( "xam: XamContentClose(%s:)\n", root.c_str() );
    RT_RETURN( complete( RT_ARG( 1 ), ok ? ERROR_SUCCESS : 0x3 ) );
}

// XamContentDelete(user, XCONTENT_DATA *, XOVERLAPPED *)
RT_IMPORT( XamContentDelete )
{
    fs::path dir = content_path( RT_ARG( 1 ) );
    std::error_code ec;
    bool ok = fs::remove_all( dir, ec ) > 0;
    fs::remove( dir.string() + ".name", ec );
    rt_log( "xam: XamContentDelete(%s)\n", dir.string().c_str() );
    RT_RETURN( complete( RT_ARG( 2 ), ok ? ERROR_SUCCESS : 2 ) );
}

// XamReadTileToTexture(type, title, u64 tile, user, buffer, pitch, height, XOVERLAPPED *): a blank tile.
RT_IMPORT( XamReadTileToTexture )
{
    if ( RT_ARG( 4 ) && RT_ARG( 5 ) && RT_ARG( 6 ) ) memset( g_base + RT_ARG( 4 ), 0, (size_t)RT_ARG( 5 ) * RT_ARG( 6 ) );
    RT_RETURN( complete( RT_ARG( 7 ), ERROR_SUCCESS ) );
}

// XamShowDeviceSelectorUI(user, content type, content flags, u64 bytes needed, PXCONTENTDEVICEID, XOVERLAPPED *):
// only the hard drive exists, so it is chosen without showing anything.
RT_IMPORT( XamShowDeviceSelectorUI )
{
    rt_log( "xam: XamShowDeviceSelectorUI(user %u, type %X) -> hard drive\n", RT_ARG( 0 ), RT_ARG( 1 ) );
    if ( RT_ARG( 4 ) ) guest_write32( RT_ARG( 4 ), HDD_DEVICE_ID );
    xam_broadcast_notification( XN_SYS_UI, 1 );
    xam_broadcast_notification( XN_SYS_UI, 0 );
    RT_RETURN( complete( RT_ARG( 5 ), ERROR_SUCCESS ) );
}
// XamContentGetDeviceState(device, XOVERLAPPED *)
RT_IMPORT( XamContentGetDeviceState )
{
    RT_RETURN( complete( RT_ARG( 1 ), RT_ARG( 0 ) == HDD_DEVICE_ID ? ERROR_SUCCESS : 0x48F ) );
}

// XMsgStartIORequest(app, message, XOVERLAPPED *, buffer, size): messages to system apps. 0xFB 0xB0008
// writes achievements (buffer { count; XUSER_ACHIEVEMENT * { user; id } }): the profile unlocks them. The
// others have nothing to report: the buffer stays zeroed.
RT_IMPORT( XMsgStartIORequest )
{
    if ( RT_ARG( 0 ) == 0xFB && RT_ARG( 1 ) == 0xB0008 && RT_ARG( 3 ) && RT_ARG( 4 ) >= 8 )
    {
        const uint32_t count = guest_read32( RT_ARG( 3 ) ), list = guest_read32( RT_ARG( 3 ) + 4 );
        for ( uint32_t i = 0; list && i < count && i < 64; ++i ) achievement_unlock( guest_read32( list + i * 8 + 4 ) );
        RT_RETURN( complete( RT_ARG( 2 ), ERROR_SUCCESS ) );
    }
    static int n;
    if ( n++ < 5 ) rt_log( "xam: XMsgStartIORequest(app %X, message %X, size %u)\n", RT_ARG( 0 ), RT_ARG( 1 ), RT_ARG( 4 ) );
    if ( RT_ARG( 3 ) && RT_ARG( 4 ) ) memset( g_base + RT_ARG( 3 ), 0, RT_ARG( 4 ) );
    RT_RETURN( complete( RT_ARG( 2 ), ERROR_SUCCESS ) );
}

// XamUserCheckPrivilege(user, privilege, PBOOL result): everything allowed.
RT_IMPORT( XamUserCheckPrivilege )
{
    if ( ( RT_ARG( 0 ) & 0xFF ) != 0 && ( RT_ARG( 0 ) & 0xFF ) != 0xFF ) RT_RETURN( ERROR_NO_SUCH_USER );
    if ( RT_ARG( 2 ) ) guest_write32( RT_ARG( 2 ), 1 );
    RT_RETURN( ERROR_SUCCESS );
}
RT_IMPORT( XNotifyPositionUI ) { RT_RETURN( 0 ); }
RT_IMPORT( XNotifyDelayUI ) { RT_RETURN( 0 ); }

// XamInputGetCapabilities(user, flags, XINPUT_CAPABILITIES *) { u8 Type, SubType; u16 Flags; XINPUT_GAMEPAD; XINPUT_VIBRATION }
RT_IMPORT( XamInputGetCapabilities )
{
    if ( ( RT_ARG( 0 ) & 0xFF ) != 0 ) RT_RETURN( ERROR_DEVICE_NOT_CONNECTED );
    uint32_t p = RT_ARG( 2 );
    memset( g_base + p, 0, 0x14 );
    g_base[p + 0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
    g_base[p + 1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    guest_write16( p + 4, 0xF3FF );  // every button
    g_base[p + 6] = 0xFF;
    g_base[p + 7] = 0xFF;
    for ( int i = 0; i < 4; ++i ) guest_write16( p + 8 + i * 2, 0xFFC0 );
    guest_write16( p + 0x10, 0xFFFF );
    guest_write16( p + 0x12, 0xFFFF );
    RT_RETURN( ERROR_SUCCESS );
}

uint64_t gpu_swap_count();  // gpu.cpp

// CRACKDOWN_INPUT_SCRIPT="40:START,48:A,52:DOWN,60:RSRIGHT": press buttons (0.25 s each) or push the
// right stick fully (RSLEFT/RSRIGHT/RSUP/RSDOWN) or pull a trigger (RT/LT), 1 s each, at those seconds.
namespace
{
struct ScriptedPress { double at; uint32_t buttons; };  // above 0xFFFF: right stick directions
const std::vector<ScriptedPress> &input_script()
{
    static std::vector<ScriptedPress> script = [] {
        std::vector<ScriptedPress> v;
        const char *env = getenv( "CRACKDOWN_INPUT_SCRIPT" );
        if ( !env ) return v;
        static const struct { const char *name; uint32_t bit; } names[] = {
            { "RSLEFT", 0x10000 }, { "RSRIGHT", 0x20000 }, { "RSUP", 0x40000 }, { "RSDOWN", 0x80000 },
            { "RT", 0x100000 }, { "LT", 0x200000 },
            { "LSUP", 0x400000 }, { "LSDOWN", 0x800000 }, { "LSLEFT", 0x1000000 }, { "LSRIGHT", 0x2000000 },
            { "UP", 0x1 }, { "DOWN", 0x2 }, { "LEFT", 0x4 }, { "RIGHT", 0x8 }, { "START", 0x10 }, { "BACK", 0x20 }, { "LS", 0x40 }, { "RS", 0x80 },
            { "LB", 0x100 }, { "RB", 0x200 }, { "A", 0x1000 }, { "B", 0x2000 }, { "X", 0x4000 }, { "Y", 0x8000 } };
        std::string all = env;
        for ( size_t pos = 0; pos < all.size(); )
        {
            size_t end = all.find( ',', pos );
            if ( end == std::string::npos ) end = all.size();
            std::string item = all.substr( pos, end - pos );
            pos = end + 1;
            size_t colon = item.find( ':' );
            if ( colon == std::string::npos ) continue;
            for ( auto &n : names )
                if ( item.substr( colon + 1 ) == n.name ) v.push_back( { atof( item.c_str() ), n.bit } );
        }
        return v;
    }();
    return script;
}
// Seconds since start; CRACKDOWN_SCRIPT_FRAMES=1: presented frames / 30 instead, so scripted input lands at
// the same point of the game however slowly it runs (captures, recording).
double script_clock()
{
    static const bool frames = getenv( "CRACKDOWN_SCRIPT_FRAMES" );
    if ( frames ) return gpu_swap_count() / ( settings().fps60 ? 60.0 : 30.0 );  // (the game's frame rate)
    static auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count();
}
void scripted_stick( PadState &pad )
{
    double t = script_clock();
    for ( auto &p : input_script() )
        if ( p.buttons > 0xFFFF && t >= p.at && t < p.at + 1.0 )
        {
            if ( p.buttons & 0x10000 ) pad.rx = -32767;
            if ( p.buttons & 0x20000 ) pad.rx = 32767;
            if ( p.buttons & 0x40000 ) pad.ry = 32767;
            if ( p.buttons & 0x80000 ) pad.ry = -32767;
            if ( p.buttons & 0x100000 ) pad.rt = 255;
            if ( p.buttons & 0x200000 ) pad.lt = 255;
            if ( p.buttons & 0x400000 ) pad.ly = 32767;
            if ( p.buttons & 0x800000 ) pad.ly = -32767;
            if ( p.buttons & 0x1000000 ) pad.lx = -32767;
            if ( p.buttons & 0x2000000 ) pad.lx = 32767;
        }
}
uint16_t scripted_buttons()
{
    double t = script_clock();
    uint16_t b = 0;
    for ( auto &p : input_script() )
        if ( p.buttons <= 0xFFFF && t >= p.at && t < p.at + 0.25 ) b |= p.buttons;
    // CRACKDOWN_SNAP="100,103,106": at those seconds (input script clock), write guest 40000000-4FFFFFFF to
    // build/snap-<n>.bin, for finding values that change with input (camera angles).
    static const char *snap = getenv( "CRACKDOWN_SNAP" );
    static double next_snap = snap ? atof( snap ) : -1;
    static int snaps;
    if ( next_snap >= 0 && t >= next_snap )
    {
        char path[64];
        snprintf( path, sizeof( path ), "build/snap-%d.bin", snaps++ );
        if ( FILE *f = fopen( path, "wb" ) ) { fwrite( g_base + 0x40000000u, 1, 0x10000000u, f ); fclose( f ); }
        rt_log( "snap: %s at %.1fs\n", path, t );
        const char *c = strchr( snap, ',' );
        snap = c ? c + 1 : snap;
        next_snap = c ? atof( snap ) : -1;
    }
    // CRACKDOWN_WATCH_PAD="105:52:40:3": at that second find controller 0's pad object (four 2216-byte
    // objects with one vtable, XINPUT_STATE copy at +2180) and watch bytes [+52, +52+40) for 3 s.
    static const char *wpad = getenv( "CRACKDOWN_WATCH_PAD" );
    if ( wpad && t >= atof( wpad ) )
    {
        double at, secs; unsigned off, len;
        if ( sscanf( wpad, "%lf:%u:%u:%lf", &at, &off, &len, &secs ) == 4 )
            for ( uint32_t a = 0x40000000u; a < 0x50000000u; a += 4 )
            {
                uint32_t vt = guest_read32( a );
                if ( vt >> 24 != 0x82 || guest_read32( a + 2216 ) != vt || guest_read32( a + 4432 ) != vt || guest_read32( a + 6648 ) != vt ) continue;
                if ( guest_read32( a + 2180 ) == 0 ) continue;
                rt_log( "watch: pad object %08X (vtable %08X)\n", a, vt );
                if ( getenv( "CRACKDOWN_WATCH_CALLERS" ) ) watch_axis_callers( a + off, a + off + len, secs );
                else watch_arm( a + off, a + off + len, secs );
                break;
            }
        wpad = nullptr;
    }
    // CRACKDOWN_POKEF="103:405bb7e4:1.5": at that second add 1.5 to the guest float there (finding camera
    // angles).
    static const char *pokef = getenv( "CRACKDOWN_POKEF" );
    if ( pokef && t >= atof( pokef ) )
    {
        double at; unsigned a; float d;
        if ( sscanf( pokef, "%lf:%x:%f", &at, &a, &d ) == 3 )
        {
            uint32_t bits = guest_read32( a );
            float v;
            memcpy( &v, &bits, 4 );
            float nv = v + d;
            memcpy( &bits, &nv, 4 );
            guest_write32( a, bits );
            rt_log( "pokef: %08X %g -> %g\n", a, v, nv );
        }
        pokef = nullptr;
    }
    finder_poll( t );
    finder_fov_poll( t );
    finder_peek_poll( t );
    finder_float_poll( t );
    // CRACKDOWN_CAPTURE_AT=<seconds>[,<seconds>...]: as F12 at those script times.
    if ( const char *cap = getenv( "CRACKDOWN_CAPTURE_AT" ) )
    {
        static size_t done;
        size_t i = 0;
        for ( const char *c = cap; c && *c; c = strchr( c, ',' ) ? strchr( c, ',' ) + 1 : nullptr, ++i )
            if ( i == done && t >= atof( c ) ) { ++done; video_request_capture(); rt_log( "capture at %.1f s\n", t ); break; }
    }
    static const char *wmem = getenv( "CRACKDOWN_WATCH" );  // "t:addr:len:secs"
    if ( wmem && t >= atof( wmem ) )
    {
        double at, secs; unsigned a, len;
        if ( sscanf( wmem, "%lf:%x:%u:%lf", &at, &a, &len, &secs ) == 4 ) watch_arm( a, a + len, secs );
        wmem = nullptr;
    }
    static const char *wphys = getenv( "CRACKDOWN_WATCH_PHYS" );  // "t:physical page:secs"
    if ( wphys && t >= atof( wphys ) )
    {
        double at, secs; unsigned a;
        if ( sscanf( wphys, "%lf:%x:%lf", &at, &a, &secs ) == 3 ) watch_arm_views( a, secs );
        wphys = nullptr;
    }
    static const char *wcall = getenv( "CRACKDOWN_CALLERS" );
    if ( wcall && t >= atof( wcall ) )
    {
        double at, secs; int used = 0;
        if ( sscanf( wcall, "%lf:%lf:%n", &at, &secs, &used ) == 2 && used ) watch_callers( wcall + used, secs );
        wcall = nullptr;
    }
    // CRACKDOWN_BINK_SCAN="4,6": at those seconds, find Bink's movie structs (big-endian 1280, 720, frames
    // of a start-up logo) in guest memory and log their first words.
    static const char *bscan = getenv( "CRACKDOWN_BINK_SCAN" );
    static double next_bscan = bscan ? atof( bscan ) : -1;
    if ( next_bscan >= 0 && t >= next_bscan )
    {
        for ( uint32_t region : { 0x40000000u, 0x70000000u, 0xA0000000u } )
            for ( uint32_t a = region; a < region + 0x20000000u; a += 4 )
                if ( guest_read32( a ) == 1280 && guest_read32( a + 4 ) == 720 && ( guest_read32( a + 8 ) == 264 || guest_read32( a + 8 ) == 241 ) )
                {
                    std::string w;
                    char b[16];
                    for ( int i = 0; i < 24; ++i ) { snprintf( b, sizeof( b ), " %X", guest_read32( a + i * 4 ) ); w += b; }
                    rt_log( "bink: %08X at %.1fs:%s\n", a, t, w.c_str() );
                }
        const char *c = strchr( bscan, ',' );
        bscan = c ? c + 1 : bscan;
        next_bscan = c ? atof( bscan ) : -1;
    }
    // CRACKDOWN_XUI_DUMP="50,55": at those seconds (input script clock), log the focused XUI element,
    // its parent and the parent's other children.
    static const char *dump = getenv( "CRACKDOWN_XUI_DUMP" );
    static double next_dump = dump ? atof( dump ) : -1;
    if ( next_dump >= 0 && t >= next_dump )
    {
        uint32_t f = xui_focus( 0 ) ? xui_focus( 0 ) : xui_focus( -1 );
        rt_log( "xui: focus user0 %08X any %08X at %.1fs\n", xui_focus( 0 ), xui_focus( -1 ), t );
        xui_list_controls( f );
        if ( getenv( "CRACKDOWN_XUI_TREE" ) ) xui_tree( f );
        if ( const char *peek = getenv( "CRACKDOWN_XUI_PEEK" ) )  // "addr:words": guest words at dump time
        {
            unsigned pa = 0, pn = 0;
            if ( sscanf( peek, "%x:%u", &pa, &pn ) == 2 )
                for ( unsigned i = 0; i < pn; i += 8 )
                {
                    std::string w;
                    char b[16];
                    for ( unsigned k = i; k < i + 8 && k < pn; ++k ) { snprintf( b, sizeof( b ), " %08X", guest_read32( pa + k * 4 ) ); w += b; }
                    rt_log( "xui: peek %08X:%s\n", pa + i * 4, w.c_str() );
                }
        }
        if ( getenv( "CRACKDOWN_XUI_TIMELINE" ) )  // the focus chain's timelines (element data +224)
            for ( uint32_t e = xui_element( f ), g = 0; e && g < 4; e = guest_read32( e + 40 ), ++g )
                if ( uint32_t tl = guest_read32( e + 224 ) )
                {
                    std::string w;
                    char b[16];
                    for ( int i = 0; i < 32; ++i ) { snprintf( b, sizeof( b ), " %X", guest_read32( tl + i * 4 ) ); w += b; }
                    rt_log( "xui: timeline of up%u at %08X:%s\n", g, tl, w.c_str() );
                }
        const char *c = strchr( dump, ',' );
        dump = c ? c + 1 : dump;
        next_dump = c ? atof( dump ) : -1;
    }
    return b;
}
uint32_t g_packet;
}

// The scripted buttons held now (CRACKDOWN_INPUT_SCRIPT), for input the game doesn't read through XInput
// (splash.cpp's skip).
uint16_t xam_scripted_buttons() { return scripted_buttons(); }


uint64_t audio_sample_count();  // audio.cpp
// XamInputGetState(user, flags, XINPUT_STATE *) { u32 PacketNumber; XINPUT_GAMEPAD { u16 Buttons; u8 LT, RT; s16 LX, LY, RX, RY } }
RT_IMPORT( XamInputGetState )
{
    static uint64_t polls[256];
    if ( ++polls[RT_ARG( 0 ) & 0xFF] % 600 == 1 && getenv( "CRACKDOWN_INPUT_SCRIPT" ) )
        rt_log( "input: XamInputGetState(user %u) poll #%llu, flags %X\n", RT_ARG( 0 ) & 0xFF, (unsigned long long)polls[RT_ARG( 0 ) & 0xFF], RT_ARG( 1 ) );
    if ( ( RT_ARG( 0 ) & 0xFF ) != 0 ) RT_RETURN( ERROR_DEVICE_NOT_CONNECTED );
    uint32_t p = RT_ARG( 2 );
    memset( g_base + p, 0, 0x10 );
    PadState pad = video_pad_state();
    scripted_stick( pad );
    xui_menu_mouse( c );  // the mouse works the menus through XUI and the game's own calls (xui.cpp), never as buttons or keys
    uint16_t buttons = scripted_buttons() | pad.buttons;
    static PadState last;
    if ( buttons != last.buttons || pad.lt != last.lt || pad.rt != last.rt || pad.lx != last.lx || pad.ly != last.ly || pad.rx != last.rx ||
         pad.ry != last.ry )
        ++g_packet;
    if ( buttons != last.buttons && buttons && getenv( "CRACKDOWN_INPUT_SCRIPT" ) ) rt_log( "input: buttons %04X\n", buttons );
    // CRACKDOWN_VOICE_LOG: each press with the audio clock, to set against the sounds it starts (latency in real play).
    if ( static const bool vlog = getenv( "CRACKDOWN_VOICE_LOG" ); vlog && ( buttons & ~last.buttons ) )
        rt_log( "input: pressed %04X at audio sample %llu\n", buttons & ~last.buttons, (unsigned long long)audio_sample_count() );
    last = pad;
    last.buttons = buttons;
    guest_write32( p, g_packet );
    guest_write16( p + 4, buttons );
    g_base[p + 6] = pad.lt;
    g_base[p + 7] = pad.rt;
    guest_write16( p + 8, (uint16_t)pad.lx );
    guest_write16( p + 10, (uint16_t)pad.ly );
    guest_write16( p + 12, (uint16_t)pad.rx );
    guest_write16( p + 14, (uint16_t)pad.ry );
    RT_RETURN( ERROR_SUCCESS );
}
RT_IMPORT( XamInputSetState ) { RT_RETURN( ( RT_ARG( 0 ) & 0xFF ) ? ERROR_DEVICE_NOT_CONNECTED : ERROR_SUCCESS ); }
// XamInputGetKeystrokeEx(PDWORD user, flags, XINPUT_KEYSTROKE *): no keystrokes queued.
// XamInputGetKeystrokeEx(PDWORD user (in: index or 0xFF any; out: who), flags, XINPUT_KEYSTROKE *)
//   XINPUT_KEYSTROKE { u16 VirtualKey; u16 Unicode; u16 Flags; u8 UserIndex; u8 HidCode }
// Keystrokes are made from controller 0's button and stick changes, like the console's input
// system does for its UI (XUI menus read these, not XInputGetState).
namespace
{
std::mutex g_key_lock;
std::deque<std::pair<uint16_t, uint16_t>> g_keys;  // virtual key, flags
uint32_t g_key_state;  // buttons in bits 0-15, stick directions in 16-19
}
RT_IMPORT( XamInputGetKeystrokeEx )
{
    uint32_t user_ptr = RT_ARG( 0 ), out = RT_ARG( 2 );
    uint32_t want = user_ptr ? guest_read32( user_ptr ) & 0xFF : 0xFF;
    if ( want != 0 && want != 0xFF ) RT_RETURN( ERROR_DEVICE_NOT_CONNECTED );
    static const struct { uint32_t bit; uint16_t vk; } map[] = {
        { 0x1000, 0x5800 }, { 0x2000, 0x5801 }, { 0x4000, 0x5802 }, { 0x8000, 0x5803 }, { 0x0200, 0x5804 }, { 0x0100, 0x5805 },
        { 0x0001, 0x5810 }, { 0x0002, 0x5811 }, { 0x0004, 0x5812 }, { 0x0008, 0x5813 }, { 0x0010, 0x5814 }, { 0x0020, 0x5815 },
        { 0x0040, 0x5816 }, { 0x0080, 0x5817 },
        { 0x10000, 0x5820 }, { 0x20000, 0x5821 }, { 0x40000, 0x5822 }, { 0x80000, 0x5823 } };  // left stick up/down/right/left
    PadState pad = video_pad_state();
    int16_t lx = pad.lx, ly = pad.ly;
    xui_menu_mouse( c );
    void xui_pause_poll( PPCContext &c );  // xui.cpp: Esc's pause, on this (the game's) thread
    xui_pause_poll( c );
    uint32_t now = pad.buttons | scripted_buttons();
    if ( ly > 16384 ) now |= 0x10000;
    if ( ly < -16384 ) now |= 0x20000;
    if ( lx > 16384 ) now |= 0x40000;
    if ( lx < -16384 ) now |= 0x80000;
    // CRACKDOWN_PRESS_TRACE=1 (testing): the functions this thread ran between delivering a d-pad down
    // keystroke and its next poll (what the menu does with the press).
    static const bool press_trace = getenv( "CRACKDOWN_PRESS_TRACE" );
    static const uint16_t press_trace_vk = press_trace ? ( strtoul( getenv( "CRACKDOWN_PRESS_TRACE" ), nullptr, 16 ) > 1 ? strtoul( getenv( "CRACKDOWN_PRESS_TRACE" ), nullptr, 16 ) : 0x5811 ) : 0;
    thread_local bool traced_press;
    if ( press_trace && traced_press ) { traced_press = false; rt_dump_trace( "after the traced press" ); }
    std::lock_guard<std::mutex> g( g_key_lock );
    for ( auto &m : map )
        if ( ( now ^ g_key_state ) & m.bit ) g_keys.push_back( { m.vk, (uint16_t)( ( now & m.bit ) ? 1 : 2 ) } );  // KEYDOWN / KEYUP
    g_key_state = now;
    // The keyboard's menu keys as keyboard keystrokes (CRACKDOWN_KEYBOARD_VK): up, down, left, right,
    // return, escape, tab, space, backspace.
    {
        uint32_t video_menu_keys();
        static const uint16_t vks[] = { 0x26, 0x28, 0x25, 0x27, 0x0D, 0x1B, 0x09, 0x20, 0x08 };
        static uint32_t kb_state;
        uint32_t kb = video_menu_keys();
        // The first key in a menu with nothing highlighted only shows where the focus is.
        const uint32_t typed = video_menu_keys() & ~kb_state & ( 0xF | 1u << 4 | 1u << 7 );
        if ( typed && xui_menu_key( c, typed & 0xF ) ) kb_state |= typed;
        for ( int i = 0; i < 9; ++i )
            if ( ( kb ^ kb_state ) >> i & 1 ) g_keys.push_back( { vks[i], (uint16_t)( ( kb >> i & 1 ) ? 1 : 2 ) } );
        kb_state = kb;
    }
    if ( g_keys.empty() ) RT_RETURN( ERROR_EMPTY );
    auto [vk, flags] = g_keys.front();
    g_keys.pop_front();
    if ( out )
    {
        memset( g_base + out, 0, 8 );
        guest_write16( out, vk );
        if ( press_trace && vk == press_trace_vk && flags == 1 ) traced_press = true;
        guest_write16( out + 4, flags );
        g_base[out + 6] = 0;
    }
    if ( user_ptr ) guest_write32( user_ptr, 0 );
    rt_log( "input: keystroke %04X %s\n", vk, flags == 1 ? "down" : "up" );
    void xui_note_back_key();
    if ( vk == 0x1B && flags == 1 && getenv( "CRACKDOWN_KEY_LOG" ) ) xui_note_back_key();
    RT_RETURN( ERROR_SUCCESS );
}
RT_IMPORT( XamInputGetKeystroke ) { RT_RETURN( ERROR_EMPTY ); }

RT_IMPORT( KeEnableFpuExceptions ) { RT_RETURN( 0 ); }

// The input script clock, for keyboard.cpp's key script.
double input_script_clock() { return script_clock(); }

namespace { std::atomic<uint32_t> g_scene{ ~0u }; }
namespace { void c_restore_r3( uintptr_t ctx, uint32_t v ) { ( (PPCContext *)( ctx - 0x80 ) )->r3.u64 = v; } }  // the front end's current scene id (8269BDC0 below)
uint32_t front_end_scene() { return g_scene; }
namespace { extern std::atomic<bool> g_title_music_muted; }
bool title_music_muted() { return g_title_music_muted; }
void title_music_unmute() { g_title_music_muted = false; }
namespace
{
// The front end's waits the runtime hides (sign-in 10, storage 19, save loading 18) between START and
// the main menu, until the menu has been up a second (preload_step): in game the scene stays the last
// front-end one (loading, 18), and the hooks below that hurry those waits along would hide or rerun
// in-game screens (the garage's vehicle pop-up has a waitText of its own).
std::atomic<bool> g_menu_reached;
// From START until the game resets its audio for the save it loads (audio.cpp, 82275BA0: the music
// starts over from there, just before the main menu comes up), the title's music is muted (xma.cpp): it
// would play on for half a second and then start again.
std::atomic<bool> g_title_music_muted;
bool hidden_wait( bool sign_in_only = false )
{
    if ( g_menu_reached ) return false;
    uint32_t sc = g_scene;
    return sign_in_only ? sc == 10 : sc == 10 || sc == 18 || sc == 19;
}
} // namespace
// The front end plays one scene-change sound per scene. Between the title and the main menu it steps
// through the storage (19) and save-loading (18) scenes the runtime hides, then the main menu (82):
// three more of that sound right after START's. For the moment each of those steps begins (until 0.1 s
// of audio has been mixed), short one-shot sounds that start are silenced (xma.cpp), so START plays
// one sound and the main menu comes up.
namespace { std::atomic<uint64_t> g_silence_until{ 0 }; }
uint64_t audio_sample_count();  // audio.cpp
bool front_end_silencing() { return audio_sample_count() < g_silence_until; }
namespace { void preload_step( PPCContext &c ); }
// CRACKDOWN_TASK_LOG=1: the front end's task queue (825BA6E8: queue +36 list, +40 head node (+8 task),
// +44 count): each new head task, its state (+4) changes, its vtable and "finished?" method (vtable +12).
extern "C" PPC_FUNC( __imp__sub_825BA6E8 );
PPC_FUNC( sub_825BA6E8 )
{
    uint32_t q = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    static const bool log = getenv( "CRACKDOWN_TASK_LOG" );
    preload_step( *(PPCContext *)( ctx - 0x80 ) );
    if ( hidden_wait() )
    {
        void xui_hide_wait_message();
        xui_hide_wait_message();
    }
    if ( const char *at = getenv( "CRACKDOWN_XUI_TREE_AT" ) )  // debug: the element tree around the focus then
    {
        static bool done;
        void xui_tree( uint32_t h );
        uint32_t xui_focus( int user );
        if ( !done && input_script_clock() >= atof( at ) ) { done = true; rt_log( "xui tree at %.1f s (focus %08X)\n", input_script_clock(), xui_focus( -1 ) ); xui_tree( xui_focus( -1 ) ); }
    }
    if ( log && guest_read32( q + 44 ) )
    {
        static uint32_t last, last_state;
        uint32_t node = guest_read32( guest_read32( q + 40 ) ), task = node ? guest_read32( node + 8 ) : 0;
        if ( task && ( task != last || guest_read32( task + 4 ) != last_state ) ) rt_log( "task: %08X state %u\n", task, last_state = guest_read32( task + 4 ) );
        if ( task != last )
        {
            last = task;
            uint32_t vt = task ? guest_read32( task ) : 0;
            rt_log( "task: head %08X vtable %08X update %08X finished %08X\n", task, vt, vt ? guest_read32( vt + 4 ) : 0, vt ? guest_read32( vt + 12 ) : 0 );
        }
    }
    __imp__sub_825BA6E8( ctx, base, frame );
    // The front end's tasks (storage check, save loading) move one state per frame. While the runtime
    // hides their scenes, the queue is run more often per frame so START reaches the main menu sooner;
    // it stops once the scene changes (the next step's own setup runs at its frame).
    for ( int i = 0; i < 8 && hidden_wait() && g_scene != 10 && guest_read32( q + 44 ); ++i )
    {
        uint32_t scene = g_scene;
        c_restore_r3( ctx, q );
        __imp__sub_825BA6E8( ctx, base, frame );
        if ( g_scene != scene ) break;
    }
}

// The hidden wait scenes (10, 19, 18) still load their art when they are built: the menu bar and the
// rotating Agency building, megabytes of PNG decoded in START's frame (XUI's PNG loader 82794DE0 via
// 82794098 (path, texture out), which looks the path up in XUI's texture cache first). The scenes are
// gone before the main menu is built, their textures with them, so the menu decoded the same images
// again in its first frame. Their loads fail now (nothing of these scenes shows), and the images are
// loaded again one per frame while the waits run, held until the main menu has been up a second: the
// menu finds them in the cache and its first frame decodes only its small buttons.
extern "C" PPC_FUNC( __imp__sub_82794098 );
namespace
{
std::vector<std::u16string> g_preload;  // paths the hidden scenes asked for, not loaded yet
std::vector<uint32_t> g_preloaded;      // the textures loaded for them
int g_menu_frames;

uint32_t call_guest( PPCContext &c, PPCFunc *f, uint32_t a0, uint32_t a1, uint32_t sp )
{
    PPCContext saved = c;
    c.r3.u64 = a0, c.r4.u64 = a1, c.r1.u64 = sp;
    f( (uintptr_t)&c + 0x80, (uintptr_t)g_base, 0 );
    uint32_t r = c.r3.u32;
    c = saved;
    return r;
}

// Once a frame from the front end's task queue (main thread).
void preload_step( PPCContext &c )
{
    uint32_t scene = g_scene;
    if ( ( scene == 10 || scene == 19 || scene == 18 || ( scene == 82 && g_menu_frames == 0 ) ) && !g_preload.empty() )
    {
        std::u16string path = g_preload.front();
        g_preload.erase( g_preload.begin() );
        uint32_t area = ( c.r1.u32 - 2048 ) & ~15u, out = area, str = area + 16;  // below the caller's frame
        for ( size_t i = 0; i <= path.size(); ++i ) guest_write16( str + (uint32_t)i * 2, i < path.size() ? path[i] : 0 );
        guest_write32( out, 0 );
        uint32_t hr = call_guest( c, __imp__sub_82794098, str, out, area - 256 );
        if ( (int32_t)hr >= 0 && guest_read32( out ) ) g_preloaded.push_back( guest_read32( out ) );
    }
    if ( scene == 82 && g_menu_frames == 29 ) g_menu_reached = true, g_title_music_muted = false;
    if ( scene == 82 && ++g_menu_frames == 30 )  // the menu holds its own now
    {
        for ( uint32_t t : g_preloaded ) call_guest( c, rt_lookup( guest_read32( guest_read32( t ) ) ), t, 1, ( c.r1.u32 - 256 ) & ~15u );  // as the loader's failure path frees one
        g_preloaded.clear();
        g_preload.clear();
    }
}
}

PPC_FUNC( sub_82794098 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    if ( hidden_wait() && !g_menu_frames )
    {
        std::u16string path;
        for ( uint32_t a = c->r3.u32; path.size() < 512; a += 2 )
            if ( uint16_t ch = guest_read16( a ) ) path += (char16_t)ch;
            else break;
        if ( std::find( g_preload.begin(), g_preload.end(), path ) == g_preload.end() ) g_preload.push_back( path );
        guest_write32( c->r4.u32, 0 );
        c->r3.u64 = 0x80004005;  // E_FAIL: the element has no image
        return;
    }
    __imp__sub_82794098( ctx, base, frame );
}

// The front end shows its sign-in / storage / loading messages for at least 2 s each (the dialog
// manager, returned by 827165C0, keeps the time its message went up at +100, in GetTickCount ms;
// 826EEC30 waits until 2000 ms have passed). Loading here takes no time, so the message is dated
// back to 0 and the flow goes straight on.
namespace { uint32_t g_dialogs; }
extern "C" PPC_FUNC( __imp__sub_827165C0 );
PPC_FUNC( sub_827165C0 )
{
    __imp__sub_827165C0( ctx, base, frame );
    g_dialogs = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
}
extern "C" PPC_FUNC( __imp__sub_826EEC30 );
PPC_FUNC( sub_826EEC30 )
{
    if ( g_dialogs ) guest_write32( g_dialogs + 100, 0 );
    __imp__sub_826EEC30( ctx, base, frame );
}
// The same for the storage message (826F0260: state +12 == 2 waits 2000 ms from +16).
extern "C" PPC_FUNC( __imp__sub_826F0260 );
PPC_FUNC( sub_826F0260 )
{
    uint32_t o = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    if ( guest_read32( o + 12 ) == 2 ) guest_write32( o + 16, 0 );
    __imp__sub_826F0260( ctx, base, frame );
}
// The "Signing In to Gamer Profile" message stays up 3 s (826E7AE8: QueryPerformanceCounter start at
// +16, compared in seconds with the constant at 82D91514); the start is dated back to 0.
extern "C" PPC_FUNC( __imp__sub_826E7AE8 );
PPC_FUNC( sub_826E7AE8 )
{
    uint32_t o = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    guest_write32( o + 16, 0 );
    guest_write32( o + 20, 0 );
    __imp__sub_826E7AE8( ctx, base, frame );
}
// The profile manager (824674B0, r3 = manager +132) waits up to 2 s for the signed-in profile to show up
// in a list nothing here fills (82488920), timing it from a QueryPerformanceCounter stamp at r3 - 60;
// it then goes on without it. The stamp is dated back so the wait ends at once.
extern "C" PPC_FUNC( __imp__sub_824674B0 );
PPC_FUNC( sub_824674B0 )
{
    uint32_t o = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32 - 60;
    if ( guest_read32( o ) | guest_read32( o + 4 ) ) { guest_write32( o, 0 ); guest_write32( o + 4, 1 ); }
    __imp__sub_824674B0( ctx, base, frame );
}

// The front end's scenes (8269BDC0: scene manager, id): 11 title, 10 "signIn", 19 storage, 18 loading the
// save, 82 main menu. 10, 18 and 19 only carry a wait message over the title backdrop while their
// controllers sign in, check the storage and load the save; that message is never shown (its elements
// are hidden each frame, xui_hide_wait_message), so START goes straight on to the main menu.
extern "C" PPC_FUNC( __imp__sub_8269BDC0 );
PPC_FUNC( sub_8269BDC0 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    uint64_t audio_sample_count();  // audio.cpp
    if ( getenv( "CRACKDOWN_TASK_LOG" ) ) rt_log( "task: scene %u (from %08X) at audio sample %llu\n", c->r4.u32, (uint32_t)c->lr, (unsigned long long)audio_sample_count() );
    static uint32_t previous = ~0u;
    if ( c->r4.u32 == 19 || c->r4.u32 == 18 || ( c->r4.u32 == 82 && previous == 18 ) ) g_silence_until = audio_sample_count() + 4800;
    if ( previous == 11 && c->r4.u32 == 10 ) g_title_music_muted = true;  // START on the title
    previous = c->r4.u32;
    g_scene = c->r4.u32;
    // With the title skipped (CRACKDOWN_SKIP_TITLE=1), the window holds the splash's last frame and the
    // game is silent until the main menu: the title's movie would start and be cut off, and the hidden
    // waits (10, 18, 19) show only the city backdrop.
    static const bool skip = settings().skip_title;
    uint32_t id = c->r4.u32;
    video_reveal( !skip || !( id == 10 || id == 11 || id == 18 || id == 19 ) );
    void video_cover_scene( uint32_t id );  // video.cpp
    video_cover_scene( id );
    extern thread_local bool rt_trace_imports;
    static const bool trace = getenv( "CRACKDOWN_SCENE_TRACE" );
    rt_trace_imports = trace;
    __imp__sub_8269BDC0( ctx, base, frame );
    rt_trace_imports = false;
}

// CRACKDOWN_SKIP_TITLE=1 passes the title ("press START") at once: when the title scene (11) is up and
// the sign-in flow (826E7608, this = flow; state +8 == 2 waits for START) is waiting, the flow's START
// handler 826E7928 (flow - 4) is called for controller 0 (the global at 82C37520), as the flow's own
// automatic path does (826E76E4).
extern "C" PPC_FUNC( __imp__sub_826E7608 );
extern "C" PPC_FUNC( __imp__sub_826E7928 );
// A click or Enter on the title (xui.cpp): the same START handling, once, for controller 0.
std::atomic<bool> g_title_start{ false };
void title_start() { if ( g_scene == 11 ) g_title_start = true; }
PPC_FUNC( sub_826E7608 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    static const bool skip = settings().skip_title;
    uint32_t flow = c->r3.u32;
    if ( getenv( "CRACKDOWN_TASK_LOG" ) )
    {
        static uint32_t last = ~0u;
        if ( guest_read32( flow + 8 ) != last ) rt_log( "task: sign-in flow state %u\n", last = guest_read32( flow + 8 ) );
    }
    if ( ( skip || g_title_start.exchange( false ) ) && g_scene == 11 && guest_read32( flow + 8 ) == 2 )
    {
        rt_log( skip ? "title: skipped\n" : "title: started (click or Enter)\n" );
        guest_write32( 0x82C37520, 0 );
        c->r3.u64 = flow - 4;
        __imp__sub_826E7928( ctx, base, frame );
        c->r3.u64 = flow;
    }
    __imp__sub_826E7608( ctx, base, frame );
    // While its scenes are hidden (10, 19, 18), the flow is run again within the frame as long as it
    // moves on: one state per frame was a third of a second between START and the main menu.
    for ( int i = 0; i < 16 && hidden_wait(); ++i )
    {
        uint32_t state = guest_read32( flow + 8 );
        c->r3.u64 = flow;
        __imp__sub_826E7608( ctx, base, frame );
        if ( guest_read32( flow + 8 ) == state ) break;
    }
}

// The downloadable content check (826FCDE8, called by sign-in flow state 11 at 826E814C with a result
// byte): one step per call, a 2 MB read and its MD5 (md5.cpp), 0 until every file of every pack is
// done. At a step a frame both packs took two seconds between START and the main menu; while the
// sign-in scenes are hidden it runs on within the frame, up to 10 ms of it so the screen keeps moving.
extern "C" PPC_FUNC( __imp__sub_826FCDE8 );
PPC_FUNC( sub_826FCDE8 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    uint32_t result = c->r3.u32;
    auto t0 = std::chrono::steady_clock::now();
    __imp__sub_826FCDE8( ctx, base, frame );
    int i = 1;
    for ( ; !c->r3.u32 && hidden_wait() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds( 10 ); ++i )
    {
        c->r3.u64 = result;
        __imp__sub_826FCDE8( ctx, base, frame );
    }
    if ( getenv( "CRACKDOWN_TASK_LOG" ) )
        rt_log( "task: content check %d steps in %.1f ms%s\n", i, std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count(), c->r3.u32 ? ", done" : "" );
}

// CRACKDOWN_SOUND_LOG=1: the front end's sound requests (826D0568 sends each queued one on through
// 82696B20(target, message string, ...)), with the scene they play in.
extern "C" PPC_FUNC( __imp__sub_82696B20 );
PPC_FUNC( sub_82696B20 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    if ( getenv( "CRACKDOWN_SOUND_LOG" ) )
    {
        uint32_t s = c->r4.u32, len = guest_read32( s + 20 ), cap = guest_read32( s + 24 );
        uint32_t chars = cap >= 8 ? guest_read32( s + 4 ) : s + 4;  // std::wstring: inline buffer below 8
        std::string text;
        for ( uint32_t i = 0; i < len && i < 120; ++i ) text += (char)guest_read16( chars + i * 2 );
        rt_log( "sound: scene %u lr %08X \"%s\"\n", (unsigned)g_scene, (uint32_t)c->lr, text.c_str() );
    }
    __imp__sub_82696B20( ctx, base, frame );
}
// CRACKDOWN_SOUND_LOG=1 also logs the sound cue calls (8227C9A0(emitter, cue, volume, ...)).
extern "C" PPC_FUNC( __imp__sub_8227C9A0 );
PPC_FUNC( sub_8227C9A0 )
{
    auto *c = (PPCContext *)( ctx - 0x80 );
    static const bool log = getenv( "CRACKDOWN_SOUND_LOG" );
    if ( log )
    {
        uint64_t audio_sample_count();
        rt_log( "cue: %u emitter %08X lr %08X scene %u sample %llu\n", c->r4.u32, c->r3.u32, (uint32_t)c->lr, (unsigned)g_scene,
                (unsigned long long)audio_sample_count() );
    }
    __imp__sub_8227C9A0( ctx, base, frame );
}
// CRACKDOWN_SOUND_LOG: calls into XuiSoundXAudio's class functions (testing).
#define XUI_SOUND_PROBE( addr )                                                                                         \
    extern "C" PPC_FUNC( __imp__sub_##addr );                                                                           \
    PPC_FUNC( sub_##addr )                                                                                              \
    {                                                                                                                   \
        auto *c = (PPCContext *)( ctx - 0x80 );                                                                         \
        if ( getenv( "CRACKDOWN_SOUND_LOG" ) )                                                                          \
        {                                                                                                               \
            uint64_t audio_sample_count();                                                                              \
            uint32_t m = c->r4.u32, id = m ? guest_read32( m + 4 ) : 0;                                                 \
            rt_log( "xuisound: " #addr " r3 %08X r4 %08X msg %08X lr %08X scene %u sample %llu\n", c->r3.u32, m, id,  \
                    (uint32_t)c->lr, (unsigned)g_scene, (unsigned long long)audio_sample_count() );                     \
        }                                                                                                               \
        __imp__sub_##addr( ctx, base, frame );                                                                          \
    }
XUI_SOUND_PROBE( 82771F78 )
XUI_SOUND_PROBE( 82772000 )
XUI_SOUND_PROBE( 82772078 )

// The profile manager's per-frame update (8246AD08, through its vtable) moves its read and write
// request queues one step per call: the ten settings reads after START took 0.6 s at two frames each.
// While the sign-in scene (10) is hidden it runs several times per frame.
extern "C" PPC_FUNC( __imp__sub_8246AD08 );
PPC_FUNC( sub_8246AD08 )
{
    uint32_t self = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    // CRACKDOWN_WATCH_AT=seconds:lo:hi:duration (hex addresses): arm the memory watch then (testing).
    if ( static const char *w = getenv( "CRACKDOWN_WATCH_AT" ); w )
    {
        static bool armed;
        double at, dur; unsigned lo, hi;
        if ( !armed && sscanf( w, "%lf:%x:%x:%lf", &at, &lo, &hi, &dur ) == 4 && input_script_clock() >= at )
        {
            armed = true;
            void watch_arm( uint32_t lo, uint32_t hi, double seconds );
            watch_arm( lo, hi, dur );
        }
    }
    __imp__sub_8246AD08( ctx, base, frame );
    for ( int i = 0; i < 24 && hidden_wait( true ); ++i )
    {
        c_restore_r3( ctx, self );
        __imp__sub_8246AD08( ctx, base, frame );
    }
}
