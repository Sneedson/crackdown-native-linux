// Debug: finding the camera's yaw angle in one run (heap addresses move between runs).
// CRACKDOWN_FIND_YAW=T: snapshots guest 40000000-4FFFFFFF at T, T+3, T+7 and T+11 (input script clock);
// the input script turns the camera right between T+4 and T+6 and back left between T+8 and T+10.
// Floats steady between the first two, moved by an angle-like amount by the right turn and back by
// the left are candidates; each is then poked by +1 (radian) and the frame thumbnails before and
// after compared: the camera's own angle changes the picture most. Results are logged, best first.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern std::atomic<int> g_sig_request;
extern std::atomic<uint32_t> g_sig_seq;
extern float g_sig[64 * 36];

namespace
{
// CRACKDOWN_FIND_RANGE="base:bytes" (hex) picks the memory searched (default 40000000:10000000).
uint32_t BASE = 0x40000000u, WORDS = 0x10000000u / 4;
std::vector<uint32_t> g_a, g_b, g_c;
std::vector<uint32_t> g_cand;
struct Result { uint32_t addr; float diff; };
std::vector<Result> g_results;
size_t g_next;
int g_phase;  // 0 before, per candidate: 1 waiting for 'before' thumbnail, 2 waiting a few frames after the poke
uint32_t g_seq_mark;
float g_before[64 * 36];

float as_float( uint32_t w ) { float f; memcpy( &f, &w, 4 ); return f; }
uint32_t as_word( float f ) { uint32_t w; memcpy( &w, &f, 4 ); return w; }
void snapshot( std::vector<uint32_t> &v )
{
    v.resize( WORDS );
    const uint32_t *src = (const uint32_t *)( g_base + BASE );
    for ( uint32_t i = 0; i < WORDS; ++i ) v[i] = __builtin_bswap32( src[i] );
}
void poke( uint32_t addr, float delta ) { guest_write32( addr, as_word( as_float( guest_read32( addr ) ) + delta ) ); }
} // namespace

void finder_poll( double t )
{
    static const double t0 = getenv( "CRACKDOWN_FIND_YAW" ) ? atof( getenv( "CRACKDOWN_FIND_YAW" ) ) : -1;
    static int step;
    if ( t0 < 0 ) return;
    static bool ranged = [] {
        unsigned b, n;
        if ( const char *r = getenv( "CRACKDOWN_FIND_RANGE" ); r && sscanf( r, "%x:%x", &b, &n ) == 2 ) { BASE = b; WORDS = n / 4; }
        return true;
    }();
    (void)ranged;
    if ( step == 0 && t >= t0 ) { snapshot( g_a ); step = 1; rt_log( "find: snapshot 1\n" ); }
    if ( step == 1 && t >= t0 + 3 ) { snapshot( g_b ); step = 2; rt_log( "find: snapshot 2\n" ); }
    if ( step == 2 && t >= t0 + 7 ) { snapshot( g_c ); step = 3; rt_log( "find: snapshot 3\n" ); }
    if ( step == 3 && t >= t0 + 11 )
    {
        const uint32_t *src = (const uint32_t *)( g_base + BASE );
        for ( uint32_t i = 0; i < WORDS; ++i )
        {
            if ( g_a[i] != g_b[i] ) continue;
            float b = as_float( g_b[i] ), c = as_float( g_c[i] ), d = as_float( __builtin_bswap32( src[i] ) );
            if ( !std::isfinite( b ) || !std::isfinite( c ) || !std::isfinite( d ) || std::fabs( b ) > 6.3f || std::fabs( c ) > 6.3f ) continue;
            if ( b == 0 || c == 0 || std::fabs( b ) == 1 || std::fabs( c ) == 1 || std::fabs( b ) < 1e-3f ) continue;
            float dc = std::fabs( c - b ), dd = std::fabs( d - b );
            if ( ( ( dc > 0.8f && dc < 3.6f ) || ( dc > 2.7f && dc < 5.6f ) ) && dd < 0.02f ) g_cand.push_back( BASE + i * 4 );
        }
        // CRACKDOWN_FIND_MATRIX=1: instead of poking, find a unit direction (x, z two words apart, x*x+z*z
        // = 1 before and after the turn) that the turn rotated, and at T+13 (during a second turn in the
        // script) watch it for 1.5 s to see which functions write it.
        if ( getenv( "CRACKDOWN_FIND_MATRIX" ) )
        {
            for ( uint32_t i = 0; i + 2 < WORDS; ++i )
            {
                if ( g_a[i] != g_b[i] || g_a[i + 2] != g_b[i + 2] ) continue;
                float bx = as_float( g_b[i] ), bz = as_float( g_b[i + 2] ), cx = as_float( g_c[i] ), cz = as_float( g_c[i + 2] );
                float dx = as_float( __builtin_bswap32( src[i] ) ), dz = as_float( __builtin_bswap32( src[i + 2] ) );
                if ( !( std::fabs( bx * bx + bz * bz - 1 ) < 1e-3f && std::fabs( cx * cx + cz * cz - 1 ) < 1e-3f ) ) continue;
                if ( std::fabs( bx - cx ) + std::fabs( bz - cz ) < 0.8f || std::fabs( bx - dx ) + std::fabs( bz - dz ) > 0.05f ) continue;
                if ( std::fabs( bx ) == 1 || std::fabs( bz ) == 1 ) continue;
                g_cand.push_back( BASE + i * 4 );
            }
            rt_log( "find: %zu rotated unit directions, first %08X\n", g_cand.size(), g_cand.empty() ? 0 : g_cand[0] );
            for ( size_t k = 0; k < g_cand.size() && k < 12; ++k ) rt_log( "find:   %08X\n", g_cand[k] );
            step = 6;
        }
        std::vector<uint32_t>().swap( g_a );
        std::vector<uint32_t>().swap( g_b );
        std::vector<uint32_t>().swap( g_c );
        rt_log( "find: %zu candidates\n", g_cand.size() );
        if ( g_cand.size() > 400 ) g_cand.resize( 400 );
        if ( step != 6 ) step = 4;
    }
    if ( step == 6 && t >= t0 + 13 && !g_cand.empty() )
    {
        void watch_arm( uint32_t lo, uint32_t hi, double seconds );
        watch_arm( g_cand[0] - 32, g_cand[0] + 32, 1.5 );
        step = 7;
    }
    if ( step != 4 ) return;
    if ( g_next >= g_cand.size() )
    {
        std::sort( g_results.begin(), g_results.end(), []( const Result &x, const Result &y ) { return x.diff > y.diff; } );
        for ( size_t i = 0; i < g_results.size() && i < 15; ++i ) rt_log( "find: %08X changes the picture by %.1f\n", g_results[i].addr, g_results[i].diff );
        step = 5;
        return;
    }
    uint32_t addr = g_cand[g_next];
    if ( g_phase == 0 ) { g_sig_request = 1; g_seq_mark = g_sig_seq; g_phase = 1; return; }
    if ( g_phase == 1 && g_sig_seq != g_seq_mark )
    {
        memcpy( g_before, g_sig, sizeof( g_before ) );
        poke( addr, 1.0f );
        g_seq_mark = g_sig_seq;
        g_phase = 2;
        return;
    }
    static int frames;
    if ( g_phase == 2 && g_sig_seq == g_seq_mark && !g_sig_request ) { g_sig_request = 1; return; }
    if ( g_phase == 2 && g_sig_seq != g_seq_mark )
    {
        if ( ++frames < 3 ) { g_seq_mark = g_sig_seq; return; }  // let the camera take it up
        frames = 0;
        float diff = 0;
        for ( int i = 0; i < 64 * 36; ++i ) diff += std::fabs( g_sig[i] - g_before[i] );
        diff /= 64 * 36;
        poke( addr, -1.0f );
        g_results.push_back( { addr, diff } );
        rt_log( "find: poked %08X: picture changed by %.1f\n", addr, diff );
        ++g_next;
        g_phase = 0;
    }
}

// CRACKDOWN_FIND_FOV=T: snapshots at T, T+3, T+7 (zoomed: the script toggles the zoom at T+4 and back at
// T+8) and compares at T+11: floats steady before, changed while zoomed and back after are logged with
// their values (the camera's field of view among them).
void finder_fov_poll( double t )
{
    static const double t0 = getenv( "CRACKDOWN_FIND_FOV" ) ? atof( getenv( "CRACKDOWN_FIND_FOV" ) ) : -1;
    static int step;
    static std::vector<uint32_t> a, b, c;
    if ( t0 < 0 || step > 3 ) return;
    static bool ranged = [] {
        unsigned rb, rn;
        if ( const char *r = getenv( "CRACKDOWN_FIND_RANGE" ); r && sscanf( r, "%x:%x", &rb, &rn ) == 2 ) { BASE = rb; WORDS = rn / 4; }
        return true;
    }();
    (void)ranged;
    if ( step == 0 && t >= t0 ) { snapshot( a ); step = 1; rt_log( "find fov: snapshot 1\n" ); }
    if ( step == 1 && t >= t0 + 3 ) { snapshot( b ); step = 2; rt_log( "find fov: snapshot 2\n" ); }
    if ( step == 2 && t >= t0 + 7 ) { snapshot( c ); step = 3; rt_log( "find fov: snapshot 3 (zoomed)\n" ); }
    if ( step == 3 && t >= t0 + 11 )
    {
        const uint32_t *src = (const uint32_t *)( g_base + BASE );
        int n = 0;
        for ( uint32_t i = 0; i < WORDS; ++i )
        {
            if ( a[i] != b[i] || b[i] == c[i] ) continue;
            float fb = as_float( b[i] ), fc = as_float( c[i] ), fd = as_float( __builtin_bswap32( src[i] ) );
            if ( !std::isfinite( fb ) || !std::isfinite( fc ) || std::fabs( fb ) < 0.05f || std::fabs( fb ) > 200 ) continue;
            if ( std::fabs( fd - fb ) > 1e-4f * std::fabs( fb ) ) continue;
            if ( std::fabs( fc - fb ) < 0.02f * std::fabs( fb ) ) continue;
            if ( ++n <= 5000 ) rt_log( "find fov: %08X %g -> zoomed %g\n", BASE + i * 4, fb, fc );
            // CRACKDOWN_FIND_FOV_VALUE=v: the first float of that value (+-0.01) changed by the zoom is watched
            // for 3 s (the script zooms again meanwhile), naming the functions writing it.
            static const char *want = getenv( "CRACKDOWN_FIND_FOV_VALUE" );
            static bool armed;
            if ( want && !armed && std::fabs( fb - (float)atof( want ) ) < 0.01f && fc < fb * 0.8f )
            {
                void watch_arm( uint32_t lo, uint32_t hi, double seconds );
                watch_arm( BASE + i * 4, BASE + i * 4 + 4, 3.0 );
                armed = true;
                rt_log( "find fov: watching %08X\n", BASE + i * 4 );
            }
        }
        rt_log( "find fov: %d candidates\n", n );
        std::vector<uint32_t>().swap( a ), std::vector<uint32_t>().swap( b ), std::vector<uint32_t>().swap( c );
        step = 4;
    }
}

// CRACKDOWN_PEEK="t:pointer:words[:t2]" (hex pointer): at t (and t2) logs the floats of the object the
// pointer points to.
void finder_peek_poll( double t )
{
    static const char *p = getenv( "CRACKDOWN_PEEK" );
    static int done;
    if ( !p ) return;
    double at = 0, at2 = -1;
    unsigned ptr = 0, words = 0;
    if ( sscanf( p, "%lf:%x:%u:%lf", &at, &ptr, &words, &at2 ) < 3 ) return;
    if ( !( ( done == 0 && t >= at ) || ( done == 1 && at2 >= 0 && t >= at2 ) ) ) return;
    ++done;
    uint32_t o = guest_read32( ptr );
    rt_log( "peek %.1f: %08X -> %08X\n", t, ptr, o );
    for ( uint32_t i = 0; o && i < words; i += 4 )
        rt_log( "peek   +%-4u %g %g %g %g\n", i * 4, as_float( guest_read32( o + i * 4 ) ), as_float( guest_read32( o + i * 4 + 4 ) ),
                as_float( guest_read32( o + i * 4 + 8 ) ), as_float( guest_read32( o + i * 4 + 12 ) ) );
}

// CRACKDOWN_FIND_FLOAT="t:value:tolerance": at t, the addresses of floats that close to value (the range
// of CRACKDOWN_FIND_RANGE, and the image's data), with the float 20 bytes on (a matrix's [1][1]).
void finder_float_poll( double t )
{
    static const char *p = getenv( "CRACKDOWN_FIND_FLOAT" );
    static bool done;
    double at, v, tol;
    if ( !p || done || sscanf( p, "%lf:%lf:%lf", &at, &v, &tol ) != 3 || t < at ) return;
    done = true;
    int n = 0;
    for ( auto [lo, words] : { std::pair<uint32_t, uint32_t>{ BASE, WORDS }, { 0x82000000u, 0x16C0000u / 4 } } )
        for ( uint32_t i = 0; i + 5 < words; ++i )
        {
            uint32_t a = lo + i * 4;
            float f = as_float( guest_read32( a ) );
            if ( std::fabs( f - (float)v ) <= tol && n++ < 200 )
                rt_log( "find float: %08X %g (+20: %g)\n", a, f, as_float( guest_read32( a + 20 ) ) );
        }
    rt_log( "find float: %d\n", n );
}
