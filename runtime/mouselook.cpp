// Mouse look: raw mouse movement turns the camera directly, like a PC game, not through the right stick.
//
// The player's update (82564628, reached through its vtable;
// r3 = player, f1 = frame time) runs its control code (8255BE08, or another in another control mode). That reads the "Turn L/R" and
// "Look U/D" actions (0x82E27CB0 / 0x82E27CAC) as -127..127, makes rates of them (sign * value^2,
// then acceleration) and integrates them into the camera angles kept on the player: yaw at +5884
// (radians, wrapped to +-pi; 82567748 stores it) and pitch at +5880 (clamped between the settings
// object's +80 / +84, the object pointed to by 82E799DC). Mouse look adds the mouse's counts to those
// two angles just before that update, so the game's own code clamps, wraps and uses them that frame.
// Aiming (bit 1 of byte +4344 or +4346 of the character state, *(player+292) - 32) keeps the camera on
// the target; there the stick moves an aim offset, +5888 across and +5892 up / down, which the game
// clamps and adds to the angles, so the mouse moves that instead. Locked on (player_locked), the mouse
// picks the target's body or vehicle part directly (aim_parts).
// The mouse sensitivity setting scales it (default 1: 0.0015 radians per count); inverting Y flips
// the vertical.
#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime.h"
#include "settings.h"
#include "input.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

PPCFunc *rt_hook_indirect( uint32_t guest, PPCFunc *f );
double input_script_clock();  // xam.cpp

bool player_locked();

namespace
{
constexpr uint32_t YAW = 5884, PITCH = 5880, AIM_X = 5888, AIM_Y = 5892, SETTINGS_PTR = 0x82E799DC;
PPCFunc *g_update;
std::atomic<uint32_t> g_player;  // the player object of the last update
std::atomic<bool> g_in_vehicle;  // the player was in a vehicle at the last update

float rd( uint32_t a ) { uint32_t w = guest_read32( a ); float f; memcpy( &f, &w, 4 ); return f; }
void wr( uint32_t a, float f ) { uint32_t w; memcpy( &w, &f, 4 ); guest_write32( a, w ); }

void look( uintptr_t ctx )
{
    Settings set = settings();
    const float scale = 0.0015f * set.mouse_sensitivity;
    const bool invert = set.invert_y;
    uint32_t player = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    if ( player ) { video_player_update(); g_player = player; }
    if ( getenv( "CRACKDOWN_LOOK_LOG" ) )  // update rate, once a second
    {
        static int n; static int64_t t0;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count();
        ++n;
        if ( now - t0 >= 1000 ) { rt_log( "look: %d player updates in the last %lld ms\n", n, (long long)( now - t0 ) ); n = 0; t0 = now; }
    }
    // In a vehicle (character state byte +4340 from about 120: 124..143 seen while driving; on foot it
    // stays below 30) the car camera is the right stick's, so the mouse is left to drive that stick.
    uint32_t st = player ? guest_read32( player + 292 ) : 0;
    uint8_t state = st ? g_base[st - 32 + 4340] : 0;
    if ( getenv( "CRACKDOWN_LOOK_LOG" ) )
    {
        static uint32_t last = 1000;
        if ( state != last ) { rt_log( "look: player %08X state %u (block %08X)\n", player, state, st ); last = state; }
    }
    if ( getenv( "CRACKDOWN_LOOK_LOG" ) && st )
    {
        static uint32_t last_aim = 1000;
        uint32_t aim = g_base[st - 32 + 4344] << 8 | g_base[st - 32 + 4346];
        if ( aim != last_aim ) { rt_log( "look: aim bytes %04X\n", aim ); last_aim = aim; }
    }
    g_in_vehicle = state >= 120;
    if ( state >= 120 ) { video_drop_look(); return; }
    // Locked on, the mouse picks the target's part (aim_parts), so it is left for that.
    if ( player_locked() ) return;
    float dx, dy;
    video_take_look( dx, dy );
    if ( !player || ( dx == 0 && dy == 0 ) ) return;
    bool aiming = st && ( ( g_base[st - 32 + 4344] & 2 ) || ( g_base[st - 32 + 4346] & 2 ) );
    if ( aiming )
    {
        wr( player + AIM_X, rd( player + AIM_X ) - dx * scale );
        wr( player + AIM_Y, rd( player + AIM_Y ) - ( invert ? -dy : dy ) * scale );
    }
    else
    {
        float yaw = rd( player + YAW ) - dx * scale;  // mouse right turns the camera right (yaw decreases)
        if ( yaw > 3.14159265f ) yaw -= 6.28318531f;
        if ( yaw < -3.14159265f ) yaw += 6.28318531f;
        wr( player + YAW, yaw );
        float pitch = rd( player + PITCH ) - ( invert ? -dy : dy ) * scale;  // mouse down looks down (pitch decreases)
        if ( uint32_t settings = guest_read32( SETTINGS_PTR ) )
            pitch = std::fmin( std::fmax( pitch, rd( settings + 80 ) ), rd( settings + 84 ) );
        wr( player + PITCH, pitch );
    }
}
// Testing (headless water, places away from the routes):
// CRACKDOWN_POS_LOG=1: the player's position (+304/+308/+312) and yaw once a script second.
// CRACKDOWN_TELEPORT="t:x,y,z[,yaw[,pitch]]": at script time t, the player there, by the game's own teleport
// (the scripts' Teleport, 823DE390, and TeleportToNamedLocation: the entity's virtual +168 with
// x, y, z, 1 in memory); yaw (radians) turns the camera too.
void pos_probe( uintptr_t ctx, uint32_t p )
{
    static const bool on = getenv( "CRACKDOWN_POS_LOG" );
    static const char *tp = getenv( "CRACKDOWN_TELEPORT" );
    if ( ( !on && !tp ) || !p ) return;
    double t = input_script_clock();
    // (Several: "t:x,y,z[,yaw];t:x,y,z...", in time order.)
    static const char *next = tp;
    float tt, x, y, z, yaw = 1e9f, pitch = 1e9f;
    if ( next && *next && sscanf( next, "%f:%f,%f,%f,%f,%f", &tt, &x, &y, &z, &yaw, &pitch ) >= 4 && t >= tt )
    {
        const char *semi = strchr( next, ';' );
        next = semi ? semi + 1 : nullptr;
        PPCContext &c = *(PPCContext *)( ctx - 0x80 );
        PPCContext saved = c;
        uint32_t v = ( saved.r1.u32 - 512 ) & ~15u;
        wr( v, x ), wr( v + 4, y ), wr( v + 8, z ), wr( v + 12, 1.0f );
        c.r3.u64 = p, c.r4.u64 = v;
        c.r1.u64 = ( saved.r1.u32 - 1024 ) & ~15u;
        uint32_t fn = guest_read32( guest_read32( p ) + 168 );
        rt_lookup( fn )( ctx, (uintptr_t)g_base, 0 );
        c = saved;
        if ( yaw < 1e8f ) wr( p + YAW, yaw );
        if ( pitch < 1e8f ) wr( p + PITCH, pitch );
        rt_log( "pos: teleported to %g %g %g (%08X)\n", x, y, z, fn );
    }
    static double last = -1;
    if ( on && t - last >= 1 ) { rt_log( "pos: t %.0f  %.1f %.1f %.1f  yaw %.2f pitch %.2f\n", t, rd( p + 304 ), rd( p + 308 ), rd( p + 312 ), rd( p + YAW ), rd( p + PITCH ) ); last = t; }
}

// Bursts of speed (the agent shooting off in a direction, too fast to record): after the player's update, its
// position against the last one by the real time between, on foot. Logged always: speed, the move, the game's
// measured frame time (82E2C268), the frame clock's modes (82E79A14: +13 fixed, +40/+44 min / max step),
// the input then; and the 2 s around it at most once a second.
void speed_watch( uint32_t p )
{
    static uint32_t last_p;
    static float lx, ly, lz;
    static auto last_t = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if ( !p ) { last_p = 0; return; }
    const float x = rd( p + 304 ), y = rd( p + 308 ), z = rd( p + 312 );
    const double dt = std::chrono::duration<double>( now - last_t ).count();
    if ( last_p == p && !g_in_vehicle && dt > 0.002 && dt < 0.5 )
    {
        const double h = std::hypot( x - lx, z - lz ), speed = h / dt;
        static auto quiet_until = now;
        if ( speed > 40.0 && h < 200.0 && now >= quiet_until )  // (a teleport, a respawn: hundreds of metres at once)
        {
            const uint32_t clk = guest_read32( 0x82E79A14 );
            const PadState pad = video_pad_state();
            rt_log( "speed: %.0f m/s on foot: moved %.2f m (%+.2f, %+.2f, up %+.2f) in %.1f ms; game ms %u, clock fixed %u min %u max %u; stick %d,%d, %s, focus %s; at %.1f, %.1f, %.1f\n",
                    speed, h, x - lx, z - lz, y - ly, dt * 1000, guest_read32( 0x82E2C268 ), clk ? g_base[clk + 13] : 0, clk ? guest_read32( clk + 40 ) : 0, clk ? guest_read32( clk + 44 ) : 0,
                    pad.lx, pad.ly, video_in_game() ? "in game" : "menu", video_menu_control() ? "menu" : "game", x, y, z );
            quiet_until = now + std::chrono::milliseconds( 250 );
        }
    }
    last_p = p, lx = x, ly = y, lz = z, last_t = now;
}

void update( uintptr_t ctx, uintptr_t base, uintptr_t frame )
{
    look( ctx );
    pos_probe( ctx, g_player );
    g_update( ctx, base, frame );
    speed_watch( g_player );
}
} // namespace

// Whether the player is locked on: the targeting object (*(character state +3188)) has a target (+748)
// and its hard-lock byte (+1997) set.
bool player_in_vehicle() { return g_in_vehicle; }

bool player_locked()
{
    uint32_t p = g_player, st = p ? guest_read32( p + 292 ) : 0, t = st ? guest_read32( st - 32 + 3188 ) : 0;
    return t && guest_read32( t + 748 ) && g_base[t + 1997];
}

// Whether the player is zoomed in (scope view): byte +7503 of the player object is 1 then.
bool player_zoomed()
{
    uint32_t p = g_player;
    return p && g_base[p + 7503];
}

void mouselook_install() { g_update = rt_hook_indirect( 0x82564628, update ); }

namespace
{
// Calls a guest function on this guest thread (registers restored afterwards).
uint32_t guest_call( PPCContext &c, uint32_t addr, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0 )
{
    PPCContext saved = c;
    c.r3.u64 = a0;
    c.r4.u64 = a1;
    c.r5.u64 = a2;
    c.r1.u64 = ( saved.r1.u32 - 1024 ) & ~15u;
    rt_lookup( addr )( (uintptr_t)&c + 0x80, (uintptr_t)g_base, 0 );
    uint32_t r = c.r3.u32;
    c = saved;
    return r;
}
uint32_t vcall( PPCContext &c, uint32_t obj, uint32_t slot, uint32_t a1 = 0, uint32_t a2 = 0 )
{
    return guest_call( c, guest_read32( guest_read32( obj ) + slot ), obj, a1, a2 );
}

// Locked on, the mouse picks the target's part (head, legs, arms / wheels, fuel tank ...) directly, as a
// pointer over the target. The targeting object keeps its target record at +736 (entity reference at
// +748, entity = that - 32) and, each frame, every part's screen position in pixels at +768 + 16 * part
// (x, y; up to 12 parts, the count is the entity's vtable +428); the part aimed at is byte +1993 and its
// level byte +1995 (entity vtable +436 of the part). The game's own stick steps (Sub-Target Up / Down /
// Left / Right, vtable +152..+164 of the targeting object) move to the nearest part that way; here a
// cursor starts on the current part, follows the target (kept relative to the parts' centre), moves with
// the mouse (CRACKDOWN_PART_SENS pixels per count, default 0.5) within the parts' box, and the nearest
// part the entity lets be aimed at (vtable +432: part, owner +1184) becomes the aimed part, as a stick
// step does (+1993, +1995, vtable +172 and the part-change cue). Only while the game allows part
// aiming (targeting vtable +144: a target, and the owner able to).
void aim_parts( PPCContext &c, uint32_t targeting )
{
    static const float sens = 0.5f * ( getenv( "CRACKDOWN_PART_SENS" ) ? (float)atof( getenv( "CRACKDOWN_PART_SENS" ) ) : 1.0f );
    const bool invert = settings().invert_y;
    static uint32_t last_entity;
    static float off_x, off_y;  // cursor relative to the parts' centre
    uint32_t ref = targeting ? guest_read32( targeting + 748 ) : 0;
    if ( !player_locked() ) { last_entity = 0; return; }
    if ( !ref || !( vcall( c, targeting, 144 ) & 0xFF ) ) { last_entity = 0; video_drop_look(); return; }  // no camera jump after
    uint32_t entity = ref - 32, owner = guest_read32( targeting + 4 );
    int n = std::min<int>( vcall( c, entity, 428 ) & 0xFF, 12 );
    if ( n <= 0 ) return;
    float cx = 0, cy = 0, x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for ( int i = 0; i < n; ++i )
    {
        float x = rd( targeting + 768 + 16 * i ), y = rd( targeting + 772 + 16 * i );
        cx += x; cy += y;
        x0 = std::fmin( x0, x ); x1 = std::fmax( x1, x ); y0 = std::fmin( y0, y ); y1 = std::fmax( y1, y );
    }
    cx /= n; cy /= n;
    int cur = (int8_t)g_base[targeting + 1993];
    if ( entity != last_entity )  // a new target: the cursor starts on the part the game picked
    {
        last_entity = entity;
        off_x = off_y = 0;
        if ( getenv( "CRACKDOWN_LOOK_LOG" ) )
        {
            rt_log( "look: target %08X (vtable %08X), %d parts, accept mask %08X, part %d level %d\n", entity, guest_read32( entity ), n, guest_read32( targeting + 1972 ), cur, (int8_t)g_base[targeting + 1995] );
            for ( int i = 0; i < n; ++i )
                rt_log( "look:   part %d at %.0f,%.0f kind %u level %d aimable %u\n", i, rd( targeting + 768 + 16 * i ), rd( targeting + 772 + 16 * i ), g_base[vcall( c, entity, 424, i ) + 20],
                        (int8_t)vcall( c, entity, 436, i ), vcall( c, entity, 432, i, owner + 1184 ) & 0xFF );
        }
        if ( cur >= 0 && cur < n ) { off_x = rd( targeting + 768 + 16 * cur ) - cx; off_y = rd( targeting + 772 + 16 * cur ) - cy; }
        video_drop_look();
        return;
    }
    float dx, dy;
    video_take_look( dx, dy );
    if ( dx == 0 && dy == 0 ) return;
    constexpr float MARGIN = 24;  // pixels the cursor may go past the outermost parts
    off_x = std::fmin( std::fmax( off_x + dx * sens, x0 - cx - MARGIN ), x1 - cx + MARGIN );
    off_y = std::fmin( std::fmax( off_y + ( invert ? -dy : dy ) * sens, y0 - cy - MARGIN ), y1 - cy + MARGIN );
    // Which parts may be aimed at: those the game's four steps (targeting vtable +152..+164) can reach
    // between them. Each needs the entity to let the part be aimed at (entity vtable +432: part, owner
    // +1184). Vehicles (entities of the class *830C5210 names: their +52 object's vtable +4 is-a test):
    // the body (part 0, 825E3B28, unchecked), the wheels (level 0, 825E3848 via 825E3228), the kind-6
    // parts (record +20, entity vtable +424; 825E3490) and the kind-5 part (825E3DD0), each only if the
    // targeting object accepts it (vtable +148: its bit in the mask at +1972, which the parts update
    // fills once the target is seen). Entities with part levels (entity vtable +440): up / down go
    // through the levels and left / right within one, all only accepted parts. Others: up / down take
    // the nearest part that way (no acceptance needed).
    bool vehicle = vcall( c, entity + 52, 4, guest_read32( 0x830C5210 ) ) & 0xFF, levels = vcall( c, entity, 440 ) & 0xFF;
    auto aimable = [&]( int i ) {
        if ( vehicle && i == 0 ) return true;
        if ( ( vehicle || levels ) && !( vcall( c, targeting, 148, i ) & 0xFF ) ) return false;
        return ( vcall( c, entity, 432, i, owner + 1184 ) & 0xFF ) != 0;
    };
    int best = -1;
    float best_d = 1e18f;
    for ( int i = 0; i < n; ++i )
    {
        float ex = rd( targeting + 768 + 16 * i ) - cx - off_x, ey = rd( targeting + 772 + 16 * i ) - cy - off_y, d = ex * ex + ey * ey;
        if ( d < best_d && aimable( i ) ) { best = i; best_d = d; }
    }
    if ( getenv( "CRACKDOWN_LOOK_LOG" ) ) rt_log( "look: part cursor %+.0f %+.0f of %d parts, nearest %d (was %d), accept mask %08X\n", off_x, off_y, n, best, cur, guest_read32( targeting + 1972 ) );
    if ( best < 0 || best == cur ) return;
    g_base[targeting + 1993] = (uint8_t)best;
    g_base[targeting + 1995] = (uint8_t)vcall( c, entity, 436, best );
    // What the lock-on update (82562630) does after a step: the targeting object's vtable +172, then
    // 8227C9A0(owner character's +388, 51, 0.0, 0), the part-change cue (the HUD's highlight).
    vcall( c, targeting, 172 );
    if ( uint32_t st = guest_read32( g_player + 292 ) )
    {
        PPCContext saved = c;
        c.f1.f64 = 0.0;
        c.r6.u64 = 0;
        guest_call( c, 0x8227C9A0, guest_read32( st - 32 + 388 ), 51 );
        c = saved;
    }
}
} // namespace

// The lock-on update (82562630: player, -, controls), then the mouse's part aiming. CRACKDOWN_LOCK_LOG=1:
// every change of the target (+748) and hard-lock byte (+1997, which only counts with a target).
extern "C" PPC_FUNC( __imp__sub_82562630 );
PPC_FUNC( sub_82562630 )
{
    static const bool log = getenv( "CRACKDOWN_LOCK_LOG" );
    auto *c = (PPCContext *)( ctx - 0x80 );
    uint32_t player = c->r3.u32;
    __imp__sub_82562630( ctx, base, frame );
    if ( uint32_t st = guest_read32( player + 292 ) )
    {
        aim_parts( *c, guest_read32( st - 32 + 3188 ) );
    }
    if ( !log ) return;
    if ( uint32_t st = guest_read32( player + 292 ) )
    {
        static uint64_t last = ~0ull;
        uint32_t tgt = guest_read32( st - 32 + 3188 );
        uint64_t now = tgt ? (uint64_t)guest_read32( tgt + 748 ) << 8 | g_base[tgt + 1997] : 0;
        if ( now != last ) { rt_log( "lock: target %08X, hard lock %u\n", (uint32_t)( now >> 8 ), (unsigned)( now & 0xFF ) ); last = now; }
    }
}

// CRACKDOWN_TARGET_EVERYTHING=1 (testing): every entity in the evaluator's (825E11F8) targetable groups
// (a vector of groups at 830F1F50, each holding a vector of entities at +4) can be locked on to (the
// Agency's own cars and agents in the garage, for headless lock-on tests): *(entity +152) +488 set.
extern "C" PPC_FUNC( __imp__sub_825E1C20 );
PPC_FUNC( sub_825E1C20 )
{
    uint32_t tgt = ( (PPCContext *)( ctx - 0x80 ) )->r3.u32;
    if ( static const bool all = getenv( "CRACKDOWN_TARGET_EVERYTHING" ); all && video_in_game() )
        for ( uint32_t g = guest_read32( 0x830F1F50 ), ge = guest_read32( 0x830F1F54 ); g < ge && g; g += 4 )
            for ( uint32_t grp = guest_read32( g ), e = guest_read32( grp + 4 ), ee = guest_read32( grp + 8 ); e < ee && e; e += 4 )
                if ( uint32_t ent = guest_read32( e ), r = guest_read32( ent + 152 ); ent && r && ent != guest_read32( tgt + 4 ) )
                    g_base[r + 488] = 1;
    __imp__sub_825E1C20( ctx, base, frame );
}
