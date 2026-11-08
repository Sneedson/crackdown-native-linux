// CRACKDOWN_STREAM_LOG=1 (testing): the game's sound streaming, timed. Two threads run it (TU6):
// 82281200 takes requests from a queue of nine (it waits on 17 events: two per stream slot and one
// more), 82281070 keeps the playing streams fed (it waits on 9: one per slot and one more): the next
// chunk read (82280A60) and handed to the slot's voice (82AD2970). The slots are 88 bytes each at
// 82DC6FC0. Each wake of those waits (82AD8968: count, handles, ...) and each read and hand-off is
// logged with the input script clock and the audio clock.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <map>

double input_script_clock();     // xam.cpp
uint64_t audio_sample_count();   // audio.cpp

namespace
{
const bool g_log = getenv( "CRACKDOWN_STREAM_LOG" ) != nullptr;
double audio_s() { return audio_sample_count() / 48000.0; }
}

extern "C" PPC_FUNC( __imp__sub_82AD8968 );
PPC_FUNC( sub_82AD8968 )
{
    auto &c = *(PPCContext *)( ctx - 0x80 );
    const uint32_t count = c.r3.u32, lr = (uint32_t)c.lr;
    __imp__sub_82AD8968( ctx, base, frame );
    if ( g_log && ( count == 9 || count == 17 ) )
        rt_log( "stream: %s woke on %u of %u (script %.2f s, audio %.2f s, lr %08X)\n", count == 9 ? "feeder" : "requests", c.r3.u32, count,
                input_script_clock(), audio_s(), lr );
}

extern "C" PPC_FUNC( __imp__sub_82280A60 );
PPC_FUNC( sub_82280A60 )
{
    auto &c = *(PPCContext *)( ctx - 0x80 );
    const uint32_t slot = c.r3.u32;
    __imp__sub_82280A60( ctx, base, frame );
    if ( g_log ) rt_log( "stream: slot %u read a chunk (script %.2f s, audio %.2f s)\n", ( slot - 0x82DC6FC0u ) / 88, input_script_clock(), audio_s() );
}

extern "C" PPC_FUNC( __imp__sub_82AD2970 );
PPC_FUNC( sub_82AD2970 )
{
    auto &c = *(PPCContext *)( ctx - 0x80 );
    if ( g_log ) rt_log( "stream: voice %08X given a buffer (script %.2f s, audio %.2f s, lr %08X)\n", c.r3.u32, input_script_clock(), audio_s(), (uint32_t)c.lr );
    if ( static const bool chain = getenv( "CRACKDOWN_SUBMIT_TRACE" ); chain )  // who starts each sound (testing)
    {
        char bt[512];
        rt_guest_backtrace( bt, sizeof( bt ) );
        rt_log( "submit: voice %08X buffer %08X at audio %.3f s from %s\n", c.r3.u32, guest_read32( c.r4.u32 + 4 ), audio_s(), bt );
    }
    __imp__sub_82AD2970( ctx, base, frame );
}

// CRACKDOWN_XAUDIO_TRACE=<from>,<to> (testing): calls of the audio library's small functions between those
// script times, with their first arguments.
static bool xaudio_trace_now()
{
    static const char *e = getenv( "CRACKDOWN_XAUDIO_TRACE" );
    if ( !e ) return false;
    static double from = atof( e ), to = strchr( e, ',' ) ? atof( strchr( e, ',' ) + 1 ) : 1e9;
    double t = input_script_clock();
    return t >= from && t <= to;
}
#define XAUDIO_PROBE( addr )                                                                                         \
    extern "C" PPC_FUNC( __imp__sub_##addr );                                                                        \
    PPC_FUNC( sub_##addr )                                                                                           \
    {                                                                                                                \
        auto &c = *(PPCContext *)( ctx - 0x80 );                                                                     \
        if ( xaudio_trace_now() )                                                                                    \
            rt_log( "xaudio: " #addr " r3 %08X r4 %08X r5 %08X f1 %.3f lr %08X\n", c.r3.u32, c.r4.u32, c.r5.u32, c.f1.f64, (uint32_t)c.lr ); \
        __imp__sub_##addr( ctx, base, frame );                                                                       \
    }
XAUDIO_PROBE( 82AD0580 )
XAUDIO_PROBE( 82AD0680 )
XAUDIO_PROBE( 82AD06E8 )
XAUDIO_PROBE( 82AD07E8 )
XAUDIO_PROBE( 82AD0860 )
XAUDIO_PROBE( 82AD08D8 )
XAUDIO_PROBE( 82AD0B38 )
XAUDIO_PROBE( 82AD0BA0 )
XAUDIO_PROBE( 82AD0C08 )
XAUDIO_PROBE( 82AD0C50 )
XAUDIO_PROBE( 82AD0CD8 )
XAUDIO_PROBE( 82AD0F18 )
XAUDIO_PROBE( 82AD0F90 )
XAUDIO_PROBE( 82AD1040 )
XAUDIO_PROBE( 82AD1298 )
XAUDIO_PROBE( 82AD1300 )
XAUDIO_PROBE( 82AD1348 )
XAUDIO_PROBE( 82AD13A8 )
XAUDIO_PROBE( 82AD15A8 )
XAUDIO_PROBE( 82AD1618 )
XAUDIO_PROBE( 82AD1808 )
XAUDIO_PROBE( 82AD18C8 )
XAUDIO_PROBE( 82AD1940 )
XAUDIO_PROBE( 82AD1BD0 )
XAUDIO_PROBE( 82AD1C28 )
XAUDIO_PROBE( 82AD1C90 )
XAUDIO_PROBE( 82AD1FE0 )
XAUDIO_PROBE( 82AD20F0 )
XAUDIO_PROBE( 82AD22C8 )
XAUDIO_PROBE( 82AD2390 )
XAUDIO_PROBE( 82AD2418 )
XAUDIO_PROBE( 82AD24B8 )
XAUDIO_PROBE( 82AD2550 )
XAUDIO_PROBE( 82AD25A0 )
XAUDIO_PROBE( 82AD2608 )
XAUDIO_PROBE( 82AD2670 )
XAUDIO_PROBE( 82AD26D0 )
XAUDIO_PROBE( 82AD2740 )
XAUDIO_PROBE( 82AD27B0 )
XAUDIO_PROBE( 82AD2808 )
XAUDIO_PROBE( 82AD2860 )
XAUDIO_PROBE( 82AD28B8 )
XAUDIO_PROBE( 82AD2918 )
XAUDIO_PROBE( 82AD29D0 )
XAUDIO_PROBE( 82AD2A18 )
XAUDIO_PROBE( 82AD2A70 )
XAUDIO_PROBE( 82AD2AD8 )
XAUDIO_PROBE( 82AD2B30 )
XAUDIO_PROBE( 82AD2B88 )
XAUDIO_PROBE( 82AD2BE0 )
XAUDIO_PROBE( 82AD2C58 )
XAUDIO_PROBE( 82AD2CD0 )
XAUDIO_PROBE( 82AD2D48 )
XAUDIO_PROBE( 82AD2DA8 )
XAUDIO_PROBE( 82AD2E80 )
XAUDIO_PROBE( 82AD2F00 )
XAUDIO_PROBE( 82AD2F88 )
XAUDIO_PROBE( 82AD2FE8 )
XAUDIO_PROBE( 82AD3278 )
XAUDIO_PROBE( 82AD3330 )
XAUDIO_PROBE( 82AD33C8 )
XAUDIO_PROBE( 82AD3418 )
XAUDIO_PROBE( 82AD3468 )
XAUDIO_PROBE( 82AD34B8 )
XAUDIO_PROBE( 82AD3538 )
XAUDIO_PROBE( 82AD3580 )
XAUDIO_PROBE( 82AD3608 )
XAUDIO_PROBE( 82AD3660 )
XAUDIO_PROBE( 82AD37F0 )
XAUDIO_PROBE( 82AD38B8 )
XAUDIO_PROBE( 82AD3AD8 )
XAUDIO_PROBE( 82AD3BC8 )
XAUDIO_PROBE( 82AD3C50 )
XAUDIO_PROBE( 82AD3CB0 )
XAUDIO_PROBE( 82AD3EC0 )
XAUDIO_PROBE( 82AD4B88 )
XAUDIO_PROBE( 82AD4C80 )
XAUDIO_PROBE( 82AD4CE8 )
XAUDIO_PROBE( 82AD4DA8 )
XAUDIO_PROBE( 82AD4E80 )
XAUDIO_PROBE( 82AD4EF8 )
XAUDIO_PROBE( 82AD4F70 )
XAUDIO_PROBE( 82AD6690 )

// CRACKDOWN_NO_BUS_SWAP=<script time> (testing): the audio tick's bus swap (82274FB0) skipped from then on.
extern "C" PPC_FUNC( __imp__sub_82274FB0 );
PPC_FUNC( sub_82274FB0 )
{
    static const char *e = getenv( "CRACKDOWN_NO_BUS_SWAP" );
    if ( e && input_script_clock() >= atof( e ) ) return;
    __imp__sub_82274FB0( ctx, base, frame );
}

// CRACKDOWN_SOUND_LEVELS=<from>,<to> (testing): each sound's final volume between those script times: its
// item (r3 - 76), kind (+40), voice kind (r3 + 16), own level (+12), attenuation input (+0), group (+32)
// and the group's +128, the source of its target (+48) and that target (+4).
extern "C" PPC_FUNC( __imp__sub_8227FE18 );
PPC_FUNC( sub_8227FE18 )
{
    auto &c = *(PPCContext *)( ctx - 0x80 );
    static const char *e = getenv( "CRACKDOWN_SOUND_LEVELS" );
    if ( e )
    {
        static double from = atof( e ), to = strchr( e, ',' ) ? atof( strchr( e, ',' ) + 1 ) : 1e9;
        double t = input_script_clock();
        if ( t >= from && t <= to )
        {
            auto f = []( uint32_t a ) { uint32_t v = guest_read32( a ); float x; memcpy( &x, &v, 4 ); return (double)x; };
            uint32_t it = c.r3.u32 - 76, group = guest_read32( it + 32 ), src = guest_read32( it + 48 );
            rt_log( "level: item %08X kind %u voice %u out %.4f own %.3f in %.2f group %08X g128 %.2f src %08X target %.2f\n", it, g_base[it + 40], g_base[c.r3.u32 + 16], c.f1.f64, f( it + 12 ), f( it ), group,
                    group ? f( group + 128 ) : 0, src, src ? f( src + 4 ) : 0 );
        }
    }
    // Its play time (+12 frames played, +8 the frames the sound is let play: its length at 30 frames a
    // second plus a margin, after which it stops the voice). At 60 fps the frames came twice as fast:
    // narrator lines and the Agency's chatter were stopped 1.5-2 s before their ends. The count goes up by
    // the 30 Hz ticks of the game's measured frame time (82E2C268, ms) instead: as before at 30 fps,
    // every other frame at 60.
    const uint32_t slot = c.r3.u32;
    const uint32_t before = slot ? guest_read32( slot + 12 ) : 0;
    __imp__sub_8227FE18( ctx, base, frame );
    if ( !slot ) return;
    const uint32_t after = guest_read32( slot + 12 );
    static std::map<uint32_t, double> ticks;  // per slot: the part of a tick not yet counted
    if ( after != before + 1 ) { ticks.erase( slot ); return; }  // not one frame's step (a new sound): as the game has it
    double &owed = ticks[slot];
    owed += std::clamp( (double)guest_read32( 0x82E2C268 ), 1.0, 100.0 ) * 30.0 / 1000.0;
    const uint32_t step = (uint32_t)owed;
    owed -= step;
    guest_write32( slot + 12, before + step );
}

// CRACKDOWN_NO_BUS_REBUILD=<script time> (testing): the audio tick's bus rebuild (8227E878) skipped from then on.
extern "C" PPC_FUNC( __imp__sub_8227E878 );
PPC_FUNC( sub_8227E878 )
{
    static const char *e = getenv( "CRACKDOWN_NO_BUS_REBUILD" );
    if ( e && input_script_clock() >= atof( e ) ) return;
    __imp__sub_8227E878( ctx, base, frame );
}

// The mixer's dst += src * gain (82AD4638: dst, src, gains, gain steps, 256 samples). A voice sometimes puts out
// a value that isn't a sound (not finite, or past 2^14): its sends carry that to the mix buses, whose state
// holds it, and from then on the front channels are silent until the game restarts: all the music and menu
// sounds gone a minute or so into play. Such samples are zeroed here, where every send passes.
extern "C" PPC_FUNC( __imp__sub_82AD4638 );
PPC_FUNC( sub_82AD4638 )
{
    auto &c = *(PPCContext *)( ctx - 0x80 );
    const uint32_t src = c.r4.u32;
    uint32_t *s = (uint32_t *)( g_base + src );
    uint32_t any = 0;
    for ( int i = 0; i < 256; ++i ) any |= ( ( __builtin_bswap32( s[i] ) >> 23 ) & 0xFF ) >= 141;
    if ( any )
    {
        static std::atomic<int> reports;
        for ( int i = 0; i < 256; ++i )
            if ( ( ( __builtin_bswap32( s[i] ) >> 23 ) & 0xFF ) >= 141 )
            {
                if ( reports++ < 8 ) rt_log( "audio: bad sample %08X in mixer source %08X [%d] at %.2f s, zeroed\n", __builtin_bswap32( s[i] ), src, i, input_script_clock() );
                s[i] = 0;
            }
    }
    __imp__sub_82AD4638( ctx, base, frame );
}

// The game's five-channel delay effect (82286C80) fades each channel in and out by the square root of a
// level ramping to a target (+64 + 4n, target +12 + 12n): a negative one gives NaN, which then stays in the
// mix. Neither may go below 0.
extern "C" PPC_FUNC( __imp__sub_82286C80 );
PPC_FUNC( sub_82286C80 )
{
    const uint32_t fx = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    static std::atomic<int> reports;
    for ( uint32_t n = 0; n < 5; ++n )
        for ( uint32_t at : { fx + 64 + 4 * n, fx + 12 + 12 * n } )
        {
            const uint32_t v = guest_read32( at );
            if ( v >> 31 && v != 0x80000000u )  // negative (or a negative NaN)
            {
                float f;
                memcpy( &f, &v, 4 );
                if ( reports++ < 8 ) rt_log( "audio: delay effect %08X %s %u was %g, now 0\n", fx, at >= fx + 64 ? "level" : "target", n, f );
                guest_write32( at, 0 );
            }
        }
    __imp__sub_82286C80( ctx, base, frame );
}


