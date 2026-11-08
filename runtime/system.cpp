// Console settings, video mode, timers, error codes and the C runtime helpers the kernel exports.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "gpu_ops.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <string>

// The 360 timebase (and performance counter) runs at 50 MHz.
RT_IMPORT( KeQueryPerformanceFrequency ) { RT_RETURN( 50000000 ); }

// XGetGameRegion: 0x00FF = North America.
RT_IMPORT( XGetGameRegion ) { RT_RETURN( 0x00FF ); }

// ExGetXConfigSetting(category, setting, buffer, size, PUSHORT required)
RT_IMPORT( ExGetXConfigSetting )
{
    uint16_t category = (uint16_t)RT_ARG( 0 ), setting = (uint16_t)RT_ARG( 1 );
    uint32_t buf = RT_ARG( 2 ), size = RT_ARG( 3 ), preq = RT_ARG( 4 );
    uint32_t value = 0, len = 4;
    if ( category == 3 )  // XCONFIG_USER_CATEGORY
    {
        switch ( setting )
        {
        case 0x09: value = 1; break;           // language: English
        case 0x0A: value = 0x00040000; break;  // video flags: widescreen
        case 0x0E: value = 0x67; len = 1; break;  // country: United States
        default: break;
        }
    }
    if ( buf && size >= len )
    {
        if ( len == 1 ) g_base[buf] = (uint8_t)value;
        else guest_write32( buf, value );
    }
    if ( preq ) guest_write16( preq, (uint16_t)len );
    RT_RETURN( 0 );
}

// XVIDEO_MODE { Width, Height, Interlaced, Widescreen, HighDefinition, float Refresh, Standard, ... }
static void write_video_mode( uint32_t p )
{
    if ( !p ) return;
    memset( g_base + p, 0, 0x30 );
    guest_write32( p + 0x00, 1280 );
    guest_write32( p + 0x04, 720 );
    guest_write32( p + 0x08, 0 );
    guest_write32( p + 0x0C, 1 );
    guest_write32( p + 0x10, 1 );
    float hz = 60.0f;
    uint32_t bits;
    memcpy( &bits, &hz, 4 );
    guest_write32( p + 0x14, bits );
    guest_write32( p + 0x18, 1 );     // NTSC-M
    guest_write32( p + 0x1C, 0x8A );  // filled in by the real kernel; D3D divides by it
    guest_write32( p + 0x20, 1 );
}
RT_IMPORT( XGetVideoMode ) { write_video_mode( RT_ARG( 0 ) ); RT_RETURN( 0 ); }
RT_IMPORT( VdQueryVideoMode ) { write_video_mode( RT_ARG( 0 ) ); RT_RETURN( 0 ); }
RT_IMPORT( VdQueryVideoFlags ) { RT_RETURN( 0x3 ); }       // widescreen | HD
RT_IMPORT( VdIsHSIOTrainingSucceeded ) { RT_RETURN( 1 ); }

// Physical addresses: guest virtual 0xA0000000 + P (our physical allocations) is physical P.
RT_IMPORT( MmGetPhysicalAddress ) { RT_RETURN( RT_ARG( 0 ) & 0x1FFFFFFF ); }

RT_IMPORT( RtlNtStatusToDosError )
{
    switch ( RT_ARG( 0 ) )
    {
    case 0x00000000: RT_RETURN( 0 );
    case 0xC0000034: RT_RETURN( 2 );    // ERROR_FILE_NOT_FOUND
    case 0xC000003A: RT_RETURN( 3 );    // ERROR_PATH_NOT_FOUND
    case 0xC0000022: RT_RETURN( 5 );    // ERROR_ACCESS_DENIED
    case 0xC0000008: RT_RETURN( 6 );    // ERROR_INVALID_HANDLE
    case 0xC0000017: RT_RETURN( 8 );    // ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000011: RT_RETURN( 38 );   // ERROR_HANDLE_EOF
    case 0xC000000D: RT_RETURN( 87 );   // ERROR_INVALID_PARAMETER
    case 0x00000102: RT_RETURN( 1460 ); // ERROR_TIMEOUT
    case 0x00000103: RT_RETURN( 997 );  // ERROR_IO_PENDING (overlapped reads: the DLC MD5 check)
    case 0xC0000004: RT_RETURN( 24 );   // ERROR_BAD_LENGTH
    case 0x80000005: RT_RETURN( 234 );  // ERROR_MORE_DATA
    case 0xC0000035: RT_RETURN( 183 );  // ERROR_ALREADY_EXISTS (STATUS_OBJECT_NAME_COLLISION)
    default: RT_RETURN( 317 );          // ERROR_MR_MID_NOT_FOUND
    }
}

// ------------------------------------------------------------------ guest printf
// On the 360 a va_list points at the caller's parameter save area: one 8-byte slot per argument
// (integers and pointers in the low word, doubles as 8 bytes).
// Big-endian UTF-16 guest string -> narrow (non-ASCII becomes '?').
static std::string narrow16( uint32_t p )
{
    std::string s;
    for ( uint16_t ch; ( ch = guest_read16( p ) ); p += 2 ) s += ch < 0x80 ? (char)ch : '?';
    return s;
}

// wide = true: the format is a guest wchar_t string, and %s takes wide strings (%S narrow).
static std::string guest_format( uint32_t fmt, uint32_t va, bool wide = false )
{
    std::string out, wfmt;
    if ( wide ) wfmt = narrow16( fmt );
    const char *f = wide ? wfmt.c_str() : (const char *)g_base + fmt;
    auto next = [&]() { uint64_t v = guest_read64( va ); va += 8; return v; };
    while ( *f )
    {
        if ( *f != '%' ) { out += *f++; continue; }
        std::string spec = "%";
        ++f;
        while ( *f && strchr( "-+ #0", *f ) ) spec += *f++;
        if ( *f == '*' ) { spec += std::to_string( (int32_t)next() ); ++f; }
        while ( *f >= '0' && *f <= '9' ) spec += *f++;
        if ( *f == '.' )
        {
            spec += *f++;
            if ( *f == '*' ) { spec += std::to_string( (int32_t)next() ); ++f; }
            while ( *f >= '0' && *f <= '9' ) spec += *f++;
        }
        bool wide64 = false;
        while ( *f && strchr( "hlLqIjzt", *f ) )
        {
            if ( *f == 'l' && f[1] == 'l' ) { wide64 = true; ++f; }
            else if ( *f == 'I' && f[1] == '6' && f[2] == '4' ) { wide64 = true; f += 2; }
            ++f;
        }
        char conv = *f ? *f++ : 0;
        char buf[512];
        switch ( conv )
        {
        case 'd': case 'i':
            spec += wide64 ? "lld" : "d";
            snprintf( buf, sizeof(buf), spec.c_str(), wide64 ? (long long)next() : (long long)(int32_t)next() );
            if ( !wide64 ) snprintf( buf, sizeof(buf), ( spec.substr( 0, spec.size() - 1 ) + "d" ).c_str(), (int32_t)strtoll( buf, nullptr, 10 ) );
            break;
        case 'u': case 'x': case 'X': case 'o':
            if ( wide64 ) { spec += "ll"; spec += conv; snprintf( buf, sizeof(buf), spec.c_str(), (unsigned long long)next() ); }
            else { spec += conv; snprintf( buf, sizeof(buf), spec.c_str(), (uint32_t)next() ); }
            break;
        case 'p': snprintf( buf, sizeof(buf), "%08X", (uint32_t)next() ); break;
        case 'c': spec += 'c'; snprintf( buf, sizeof(buf), spec.c_str(), (int)(char)next() ); break;
        case 'f': case 'g': case 'e': case 'G': case 'E':
        {
            uint64_t bits = next();
            double d;
            memcpy( &d, &bits, 8 );
            spec += conv;
            snprintf( buf, sizeof(buf), spec.c_str(), d );
            break;
        }
        case 's': case 'S':
        {
            uint32_t p = (uint32_t)next();
            bool wide_arg = wide ? conv == 's' : conv == 'S';
            std::string str = !p ? "(null)" : wide_arg ? narrow16( p ) : std::string( (const char *)g_base + p );
            spec += 's';
            snprintf( buf, sizeof(buf), spec.c_str(), str.c_str() );
            break;
        }
        case '%': strcpy( buf, "%" ); break;
        default: buf[0] = 0; break;
        }
        out += buf;
    }
    return out;
}

// Writes `s` into a guest buffer of `count` bytes the way MSVC's _vsnprintf does: returns -1 when
// it does not fit (no terminator then), else the length.
static int32_t guest_store( uint32_t dst, uint32_t count, const std::string &s )
{
    if ( s.size() < count ) { memcpy( g_base + dst, s.c_str(), s.size() + 1 ); return (int32_t)s.size(); }
    if ( count ) memcpy( g_base + dst, s.data(), count );
    return -1;
}

// Wide forms: count in characters, output big-endian UTF-16.
static void store16( uint32_t dst, const std::string &s )
{
    for ( size_t i = 0; i < s.size(); ++i ) guest_write16( dst + (uint32_t)i * 2, (uint8_t)s[i] );
    guest_write16( dst + (uint32_t)s.size() * 2, 0 );
}
// Variadic forms: spill r3-r10 into the caller's parameter save area (8-byte slots at r1+0x10, the
// layout stack arguments already use) so the variable arguments form a guest va_list.
static uint32_t spill_varargs( PPCContext &c, int first_variadic )
{
    const uint64_t r[8] = { c.r3.u64, c.r4.u64, c.r5.u64, c.r6.u64, c.r7.u64, c.r8.u64, c.r9.u64, c.r10.u64 };
    for ( int i = 0; i < 8; ++i ) guest_write64( c.r1.u32 + 0x10 + i * 8, r[i] );
    return c.r1.u32 + 0x10 + first_variadic * 8;
}
RT_IMPORT( swprintf ) { std::string s = guest_format( RT_ARG( 1 ), spill_varargs( c, 2 ), true ); store16( RT_ARG( 0 ), s ); RT_RETURN( s.size() ); }
RT_IMPORT( sprintf )
{
    std::string s = guest_format( RT_ARG( 1 ), spill_varargs( c, 2 ) );
    memcpy( g_base + RT_ARG( 0 ), s.c_str(), s.size() + 1 );
    RT_RETURN( s.size() );
}
RT_IMPORT( _vscwprintf ) { RT_RETURN( guest_format( RT_ARG( 0 ), RT_ARG( 1 ), true ).size() ); }
RT_IMPORT( vswprintf ) { std::string s = guest_format( RT_ARG( 1 ), RT_ARG( 2 ), true ); store16( RT_ARG( 0 ), s ); RT_RETURN( s.size() ); }
RT_IMPORT( _vsnwprintf )
{
    std::string s = guest_format( RT_ARG( 2 ), RT_ARG( 3 ), true );
    uint32_t count = RT_ARG( 1 );
    if ( s.size() >= count ) { if ( count ) { s.resize( count ); for ( size_t i = 0; i < count; ++i ) guest_write16( RT_ARG( 0 ) + i * 2, (uint8_t)s[i] ); } RT_RETURN( -1 ); }
    store16( RT_ARG( 0 ), s );
    RT_RETURN( s.size() );
}

RT_IMPORT( _vsnprintf ) { RT_RETURN( (int64_t)guest_store( RT_ARG( 0 ), RT_ARG( 1 ), guest_format( RT_ARG( 2 ), RT_ARG( 3 ) ) ) ); }
RT_IMPORT( vsprintf ) { std::string s = guest_format( RT_ARG( 1 ), RT_ARG( 2 ) ); memcpy( g_base + RT_ARG( 0 ), s.c_str(), s.size() + 1 ); RT_RETURN( s.size() ); }

// Title services the game registers with but does not need answers from yet.
RT_IMPORT( ExRegisterTitleTerminateNotification ) { RT_RETURN( 0 ); }
// XAudioRegisterRenderDriverClient({ callback, context } *, PDWORD driver): the audio hardware asks
// the client for a frame of 256 samples (48 kHz) every 5.33 ms by calling callback(context). The
// game's audio clock -- and with it movie playback -- advances on those calls. While the game waits
// for its voices to stop (audio.cpp: audio_hurry), frames are asked for back to back.
// With a sound device open the device sets the pace: the next frame is asked for when what is queued
// for it has drained to two of its periods (audio_output_target), so a sound reaches it a short, fixed
// time after the game mixes it. On a clock of its own the queue sat wherever bursts (loading, catching
// up) had left it -- up to 125 ms more delay, different each session. While the device takes nothing
// for 100 ms (none, suspended), or headless, the steady 5.33 ms clock.
std::atomic<bool> g_audio_hurry;
double input_script_clock();  // xam.cpp
int audio_output_backlog();    // audio.cpp
int audio_output_target();     // audio.cpp
RT_IMPORT( XAudioRegisterRenderDriverClient )
{
    uint32_t callback = guest_read32( RT_ARG( 0 ) ), context = guest_read32( RT_ARG( 0 ) + 4 );
    static int clients;
    uint32_t cookie = 0x41550000 | (uint32_t)clients++;
    if ( RT_ARG( 1 ) ) guest_write32( RT_ARG( 1 ), cookie );
    rt_log( "audio: render client %08X(%08X) registered\n", callback, context );
    std::thread( [callback, context] {
        GuestThread *t = rt_guest_thread_create( "audio-driver" );
        // The game mixes its frames on this thread: denormals flushed for scalar floats too (ppc_context.h).
        // Zoomed in, its filters' tails decayed into denormals and a 5.3 ms frame took ~46 ms to mix: the
        // sound fell behind (fuzzy, lines cut short).
        PPCFPSCRRegister::keep_flush = true;
        _mm_setcsr( _mm_getcsr() | PPCFPSCRRegister::FlushMask );
        auto next = std::chrono::steady_clock::now();
        for ( ;; )
        {
            if ( g_audio_hurry )
            {
                std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );  // the waiting thread polls in between
                next = std::chrono::steady_clock::now();
            }
            else
            {
                next += std::chrono::microseconds( 5333 );
                static auto drained = std::chrono::steady_clock::now();  // when the device last took some
                static int before = 0;
                const int target = audio_output_target();
                int backlog = audio_output_backlog();
                auto now = std::chrono::steady_clock::now();
                if ( backlog < before ) drained = now;
                if ( target > 0 && now - drained < std::chrono::milliseconds( 100 ) )
                {
                    while ( backlog > target && std::chrono::steady_clock::now() - drained < std::chrono::milliseconds( 100 ) )
                    {
                        std::this_thread::sleep_for( std::chrono::microseconds( 500 ) );
                        int b = audio_output_backlog();
                        if ( b < backlog ) drained = std::chrono::steady_clock::now();
                        backlog = b;
                    }
                    next = std::chrono::steady_clock::now();
                }
                else std::this_thread::sleep_until( next );
                before = audio_output_backlog() + 256;  // after this frame
            }
            static const bool gap_log = getenv( "CRACKDOWN_AUDIO_GAPS" );  // debug: the clock held up 15 ms or more
            if ( gap_log )
            {
                static auto last = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                double ms = std::chrono::duration<double, std::milli>( now - last ).count();
                if ( ms >= 15 ) rt_log( "audio: render clock held up %.1f ms (script clock %.2f s)\n", ms, input_script_clock() );
                last = now;
            }
            rt_guest_call( t, callback, context, 0 );
        }
    } ).detach();
    RT_RETURN( 0 );
}
RT_IMPORT( XAudioUnregisterRenderDriverClient ) { RT_RETURN( 0 ); }
// XAudioSubmitRenderDriverFrame(driver, samples): one finished frame of the game's mix.
RT_IMPORT( XAudioSubmitRenderDriverFrame )
{
    if ( RT_ARG( 1 ) ) audio_submit_frame( g_base + RT_ARG( 1 ) );
    RT_RETURN( 0 );
}

RT_IMPORT( ExTerminateThread ) { throw GuestThreadExit{ RT_ARG( 0 ) }; }

// NtQueryVirtualMemory(address, MEMORY_BASIC_INFORMATION *, ...): everything is committed RW memory.
RT_IMPORT( NtQueryVirtualMemory )
{
    uint32_t a = RT_ARG( 0 ) & ~0xFFFu, p = RT_ARG( 1 );
    guest_write32( p + 0x00, a );           // BaseAddress
    guest_write32( p + 0x04, a & ~0xFFFFu ); // AllocationBase
    guest_write32( p + 0x08, 4 );           // AllocationProtect: PAGE_READWRITE
    guest_write32( p + 0x0C, 0x10000 - ( a & 0xFFFF ) );  // RegionSize
    guest_write32( p + 0x10, 0x1000 );      // State: MEM_COMMIT
    guest_write32( p + 0x14, 4 );           // Protect
    guest_write32( p + 0x18, 0x20000 );     // Type: MEM_PRIVATE
    RT_RETURN( 0 );
}

// RtlFillMemoryUlong(destination, length, pattern)
RT_IMPORT( RtlFillMemoryUlong )
{
    for ( uint32_t i = 0; i + 4 <= RT_ARG( 1 ); i += 4 ) guest_write32( RT_ARG( 0 ) + i, RT_ARG( 2 ) );
    RT_RETURN( 0 );
}

// ------------------------------------------------------------------ display (stage 1: no output)
// VdGetSystemCommandBuffer(state, PULONG id): a 0x94-byte swap state block D3D keeps across
// presents (all zero = nothing to carry over), plus an id the game passes back into VdSwap.
RT_IMPORT( VdGetSystemCommandBuffer )
{
    if ( RT_ARG( 0 ) ) memset( g_base + RT_ARG( 0 ), 0, 0x94 );
    if ( RT_ARG( 1 ) ) guest_write32( RT_ARG( 1 ), 0xBEEF0000 );
    RT_RETURN( 0 );
}

// VdSwap(ring space, fetch constant, front buffer, swap state, id, ...): the kernel fills the 64
// dwords D3D reserved in the ring with the swap command (D3D's pointer is to the last dword written,
// so the space is ptr+4 .. ptr+256 and VdSwap gets ptr+4). With the native device the present is an
// operation for the renderer, the space a marker for where it goes (a NOP over the 64 dwords); else a
// single type-3 packet (opcode 0x64, the command processor presents it) whose body carries the front
// buffer's fetch constant.
RT_IMPORT( VdSwap )
{
    uint32_t p = RT_ARG( 0 );
    constexpr uint32_t DWORDS = 64;
    memset( g_base + p, 0, DWORDS * 4 );
    if ( native_d3d_ops() )
    {
        std::vector<uint32_t> o;
        op_begin( o, OP_PRESENT, 6 );
        for ( int i = 0; i < 6; ++i ) o.push_back( guest_read32( RT_ARG( 1 ) + i * 4 ) );
        auto b = gpu_batch_new();
        gpu_batch_add( *b, o );
        guest_write32( p, ( 3u << 30 ) | ( ( DWORDS - 2 ) << 16 ) | ( 0x10 << 8 ) );
        guest_write32( p + 4, NATIVE_MARKER );
        guest_write32( p + 8, b->seq );
    }
    else
    {
        guest_write32( p, ( 3u << 30 ) | ( ( DWORDS - 2 ) << 16 ) | ( 0x64 << 8 ) );
        for ( int i = 0; i < 6; ++i ) guest_write32( p + 4 + i * 4, guest_read32( RT_ARG( 1 ) + i * 4 ) );  // front buffer fetch constant
    }
    if ( ++g_stats.swaps == 1 ) rt_log( "VdSwap: first frame\n" );
    RT_RETURN( 0 );
}
RT_IMPORT( VdSetDisplayMode ) { RT_RETURN( 0 ); }
RT_IMPORT( VdInitializeScalerCommandBuffer ) { RT_RETURN( 0 ); }
RT_IMPORT( VdPersistDisplay ) { RT_RETURN( 0 ); }
RT_IMPORT( VdRetrainEDRAM ) { RT_RETURN( 0 ); }
RT_IMPORT( VdRetrainEDRAMWorker ) { RT_RETURN( 0 ); }
// VdGetCurrentDisplayInformation (0x58 bytes): front buffer size at 0, display size at 0x48/0x4A
// and the display pitch-in-pixels at 0x56 (what D3D reads back); everything is 1280x720.
RT_IMPORT( VdGetCurrentDisplayInformation )
{
    uint32_t p = RT_ARG( 0 );
    memset( g_base + p, 0, 0x58 );
    guest_write16( p + 0x00, 1280 );
    guest_write16( p + 0x02, 720 );
    guest_write16( p + 0x48, 1280 );
    guest_write16( p + 0x4A, 720 );
    guest_write16( p + 0x56, 1280 );
    RT_RETURN( 0 );
}

// KeDelayExecutionThread(mode, alertable, PLARGE_INTEGER interval): negative = relative 100 ns.
RT_IMPORT( KeDelayExecutionThread )
{
    int64_t t = (int64_t)guest_read64( RT_ARG( 2 ) );
    if ( t < 0 ) t = -t;
    auto t0 = std::chrono::steady_clock::now();
    if ( t ) std::this_thread::sleep_for( std::chrono::nanoseconds( t * 100 ) );
    else std::this_thread::yield();
    void wait_stat( uint32_t lr, const char *kind, double ms );  // sync.cpp
    {
        uint32_t back = guest_read32( c.r1.u32 ), up = back ? guest_read32( back - 8 ) : 0, back2 = back ? guest_read32( back ) : 0, up2 = back2 ? guest_read32( back2 - 8 ) : 0;
        static const bool on = getenv( "CRACKDOWN_WAIT_STATS" );
        if ( on && t >= 10000 )
        {
            static int n;
            if ( n++ % 200 == 0 ) rt_log( "waits: Sleep %.1f ms from %08X <- %08X <- %08X\n", t / 10000.0, (uint32_t)c.lr, up, up2 );
        }
    }
    wait_stat( (uint32_t)c.lr, "Delay", std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
    RT_RETURN( 0 );
}
RT_IMPORT( MmQueryAddressProtect ) { RT_RETURN( 4 ); }  // PAGE_READWRITE
// VdGetCurrentDisplayGamma(PULONG type, float *power): sRGB-ish 2.2 ramp.
RT_IMPORT( VdGetCurrentDisplayGamma )
{
    float g = 2.2f;
    uint32_t bits;
    memcpy( &bits, &g, 4 );
    if ( RT_ARG( 0 ) ) guest_write32( RT_ARG( 0 ), 1 );
    if ( RT_ARG( 1 ) ) guest_write32( RT_ARG( 1 ), bits );
    RT_RETURN( 0 );
}
RT_IMPORT( VdCallGraphicsNotificationRoutines ) { RT_RETURN( 0 ); }
RT_IMPORT( ObDeleteSymbolicLink ) { RT_RETURN( 0 ); }
RT_IMPORT( ObCreateSymbolicLink ) { RT_RETURN( 0 ); }

RT_IMPORT( KeSaveFloatingPointState ) { RT_RETURN( 0 ); }
RT_IMPORT( KeRestoreFloatingPointState ) { RT_RETURN( 0 ); }
RT_IMPORT( KiApcNormalRoutineNop ) { RT_RETURN( 0 ); }
RT_IMPORT( VdSetSystemCommandBufferGpuIdentifierAddress ) { RT_RETURN( 0 ); }

// XMACreateContext / XMAReleaseContext: xma.cpp.

// ------------------------------------------------------------------ string conversion
// RtlMultiByteToUnicodeN(PWCH dst, ULONG dst bytes, PULONG written bytes, PCH src, ULONG src bytes)
// and the reverse: the title's code page is Latin-1 here (guest UTF-16 is big-endian).
RT_IMPORT( RtlMultiByteToUnicodeN )
{
    uint32_t dst = RT_ARG( 0 ), max = RT_ARG( 1 ) / 2, out = RT_ARG( 2 ), src = RT_ARG( 3 ), n = std::min( RT_ARG( 4 ), max );
    for ( uint32_t i = 0; i < n; ++i ) guest_write16( dst + i * 2, g_base[src + i] );
    if ( out ) guest_write32( out, n * 2 );
    RT_RETURN( 0 );
}
RT_IMPORT( RtlUnicodeToMultiByteN )
{
    uint32_t dst = RT_ARG( 0 ), max = RT_ARG( 1 ), out = RT_ARG( 2 ), src = RT_ARG( 3 ), n = std::min( RT_ARG( 4 ) / 2, max );
    for ( uint32_t i = 0; i < n; ++i )
    {
        uint16_t w = __builtin_bswap16( *(uint16_t *)( g_base + src + i * 2 ) );
        g_base[dst + i] = w < 256 ? (uint8_t)w : '?';
    }
    if ( out ) guest_write32( out, n );
    RT_RETURN( 0 );
}
