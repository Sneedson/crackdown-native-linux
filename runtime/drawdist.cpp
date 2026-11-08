// Draw Distance (the Recomp Options page's slider, settings.h lod: 0 the game's own, 1..4 farther).
//
// Two things in the game decide how far the city reaches:
//
// - The active radius around the players: 100 m at 0x82C3B438 (plus a 5 m margin at 0x82C3B43C). The
//   population spawns people and traffic inside it (82147058 weighs spawn places by it) and removes
//   them past radius + margin: people vanished at 105 m from the player in a test drive. The game's own
//   setter (82149788, a debug command) shows what follows from it: radius squared at 0x82E7DA68,
//   radius + margin at 0x82E7DA6C and that squared at 0x82E7DA70. The setting widens the radius and
//   writes those the same way. A wider ring with the same head count would thin people out near the
//   player, so the population's caps grow with the area: people (byte +28 of the population state at
//   0x830CCBD0, 100; the code also stops at 150 in all) and traffic (byte +29, 30).
// - The LOD picker (823F6E30): for each world object it scales the camera distance by the game's factor
//   at 0x82C3972C (1, less while zoomed through a scope) and compares that with its kind's switch and
//   cut-off distances (the table at *0x82E79B60 + 36, 20 bytes a kind). The setting divides that factor
//   for the call, so objects keep their detail and stay drawn proportionally farther.
//
// Whatever the game itself puts in those places (scripts, the scope) is taken as its new value and
// scaled from there. CRACKDOWN_POP_LOG=1 logs the population's sizes every 10 s.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
float rd( uint32_t a ) { uint32_t w = guest_read32( a ); float f; memcpy( &f, &w, 4 ); return f; }
void wr( uint32_t a, float f ) { uint32_t w; memcpy( &w, &f, 4 ); guest_write32( a, w ); }
uint8_t rb( uint32_t a ) { return *(uint8_t *)( g_base + a ); }
void wb( uint32_t a, uint8_t v ) { *(uint8_t *)( g_base + a ) = v; }

constexpr uint32_t RADIUS = 0x82C3B438, MARGIN = 0x82C3B43C;
constexpr uint32_t RADIUS_SQ = 0x82E7DA68, OUTER = 0x82E7DA6C, OUTER_SQ = 0x82E7DA70;
constexpr uint32_t POP_STATE = 0x830CCBD0, PEOPLE_CAP = POP_STATE + 28, TRAFFIC_CAP = POP_STATE + 29;
constexpr uint32_t LOD_FACTOR = 0x82C3972C;

// Per level: how much wider the active radius is, and how much farther objects keep their LOD.
// The radius stops at 1.6 times: the game holds people to 150 (about 60 live in its own 100 m), so a
// wider ring would thin them out around the player. The 150 isn't one number: the people count's limit
// (a 150 in 8257A3F0) and the entity manager's budgets (*0x82E79AD8 +0x2C094: people 150, characters 152,
// vehicles 32, each +4 the capacity): pools the game's structures are built around. More capacity at run
// time gave no more people, made bigger at load the game crashed (popdebug.cpp).
constexpr float RADIUS_SCALE[5] = { 1.0f, 1.15f, 1.3f, 1.45f, 1.6f };
constexpr float LOD_SCALE[5] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };

int level() { return std::clamp( settings().lod, 0, 4 ); }

// A value the game owns, scaled: `base` is the game's own (what was there before the first write and
// whenever the game writes something new), `written` what was last put there.
struct Scaled
{
    float base = 0, written = 0;
    bool seen = false;
    float update( float now, float scale )
    {
        if ( !seen || now != written ) { base = now; seen = true; }
        written = base * scale;
        return written;
    }
};
struct ScaledByte
{
    uint8_t base = 0, written = 0;
    bool seen = false;
    uint8_t update( uint8_t now, float scale )
    {
        if ( !seen || now != written ) { base = now; seen = true; }
        written = (uint8_t)std::min( 255.0f, std::round( base * scale ) );
        return written;
    }
};

void apply()
{
    static Scaled radius;
    static ScaledByte people, traffic;
    const float now = rd( RADIUS );
    if ( !( now > 0 ) ) return;  // not set up yet
    const float m = RADIUS_SCALE[level()];
    const float r = radius.update( now, m );
    if ( r != now || rd( RADIUS_SQ ) != r * r )
    {
        const float outer = r + rd( MARGIN );
        wr( RADIUS, r );
        wr( RADIUS_SQ, r * r );
        wr( OUTER, outer );
        wr( OUTER_SQ, outer * outer );
    }
    wb( PEOPLE_CAP, people.update( rb( PEOPLE_CAP ), m * m ) );
    wb( TRAFFIC_CAP, traffic.update( rb( TRAFFIC_CAP ), m * m ) );
    if ( static int last = -1; getenv( "CRACKDOWN_POP_LOG" ) && last != level() )
    {
        last = level();
        rt_log( "drawdist: level %d: active radius %g m (game %g), people cap %u, traffic cap %u, LOD x%g\n", last, r, radius.base,
                rb( PEOPLE_CAP ), rb( TRAFFIC_CAP ), LOD_SCALE[last] );
    }
}

// CRACKDOWN_POP_LOG: sizes of the object lists the game looks up by kind (8214C4E8: r4 the kind, returns
// a list with begin at +4 and end at +8), the people's and traffic's among them.
struct Count { uint32_t kind, size; };
Count g_counts[64];
int g_ncounts;
}

// The population's update (once a frame in game): keeps the radius and caps as the setting says.
extern "C" PPC_FUNC( __imp__sub_82577C68 );
void budget_log();  // popdebug.cpp
PPC_FUNC( sub_82577C68 )
{
    budget_log();
    apply();
    __imp__sub_82577C68( ctx, base, frame );
    if ( static const bool log = getenv( "CRACKDOWN_POP_LOG" ) != nullptr; log )
    {
        static double next = 10;
        extern double input_script_clock();
        if ( input_script_clock() >= next )
        {
            next += 10;
            char line[512]; int n = 0;
            for ( int i = 0; i < g_ncounts && n < 480; i++ )
                if ( g_counts[i].size >= 5 ) n += snprintf( line + n, sizeof( line ) - n, " %08X:%u", g_counts[i].kind, g_counts[i].size );
            rt_log( "drawdist: %.0f s: radius %g, lists%s\n", input_script_clock(), rd( RADIUS ), line );
        }
    }
}

extern "C" PPC_FUNC( __imp__sub_8214C4E8 );
PPC_FUNC( sub_8214C4E8 )
{
    static const bool log = getenv( "CRACKDOWN_POP_LOG" ) != nullptr;
    PPCContext &c = *(PPCContext *)( ctx - 0x80 );
    const uint32_t kind = c.r4.u32;
    __imp__sub_8214C4E8( ctx, base, frame );
    if ( !log || !c.r3.u32 ) return;
    const uint32_t size = ( guest_read32( c.r3.u32 + 8 ) - guest_read32( c.r3.u32 + 4 ) ) / 4;
    for ( int i = 0; i < g_ncounts; i++ )
        if ( g_counts[i].kind == kind ) { g_counts[i].size = size; return; }
    if ( g_ncounts < 64 ) g_counts[g_ncounts++] = { kind, size };
}

// The LOD picker: the game's distance factor divided for the call.
extern "C" PPC_FUNC( __imp__sub_823F6E30 );
PPC_FUNC( sub_823F6E30 )
{
    const int l = level();
    if ( l == 0 ) { __imp__sub_823F6E30( ctx, base, frame ); return; }
    const float f = rd( LOD_FACTOR );
    wr( LOD_FACTOR, f / LOD_SCALE[l] );
    __imp__sub_823F6E30( ctx, base, frame );
    wr( LOD_FACTOR, f );
}

